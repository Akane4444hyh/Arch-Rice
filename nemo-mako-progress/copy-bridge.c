/* Keep GIO's copy/overwrite/metadata/cancellation implementation. Bound its
 * Linux copy syscalls only while copying to a removable block device. This
 * prevents a whole file being buffered before the first progress callback.
 * fdatasync checkpoints make the displayed byte count meaningful on SD cards.
 */
#define _GNU_SOURCE
#include "progress.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CHECKPOINT (1024 * 1024)

typedef struct {
    Transfer *transfer;
    GFileProgressCallback callback;
    gpointer data;
    gboolean durable;
    dev_t device;
    goffset written, synced;
    int sync_error;
} CopyContext;

static _Thread_local CopyContext *copy_context;

static gboolean removable_device(dev_t device)
{
    char link[128], resolved[PATH_MAX], path[PATH_MAX + 32];
    g_snprintf(link, sizeof(link), "/sys/dev/block/%u:%u",
               major(device), minor(device));
    if (!realpath(link, resolved)) return FALSE;
    /* A partition's removable attribute lives on its parent block device. */
    for (unsigned i = 0; i < 2; i++) {
        g_snprintf(path, sizeof(path), "%s/removable", resolved);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            char value = '0';
            ssize_t n = read(fd, &value, 1);
            close(fd);
            return n == 1 && value == '1';
        }
        char *slash = strrchr(resolved, '/');
        if (!slash) break;
        *slash = '\0';
    }
    return FALSE;
}

static gboolean destination_device(GFile *destination, dev_t *device)
{
    char *path = g_file_get_path(destination);
    if (!path) return FALSE;
    char *parent = g_path_get_dirname(path);
    struct stat st;
    gboolean found = stat(parent, &st) == 0;
    if (found) *device = st.st_dev;
    g_free(parent);
    g_free(path);
    return found;
}

static gboolean output_is_target(int fd)
{
    struct stat st;
    return copy_context && copy_context->durable &&
           fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_dev == copy_context->device;
}

static int sync_data(int fd)
{
    int result;
    do result = fdatasync(fd); while (result < 0 && errno == EINTR);
    return result;
}

static ssize_t checkpoint(int fd, ssize_t result)
{
    if (result <= 0) return result;
    CopyContext *ctx = copy_context;
    ctx->written += result;
    if (ctx->written - ctx->synced >= CHECKPOINT) {
        if (sync_data(fd) < 0) {
            ctx->sync_error = errno;
            return -1;
        }
        ctx->synced = ctx->written;
    }
    return result;
}

ssize_t copy_file_range(int in, off64_t *in_offset, int out,
                       off64_t *out_offset, size_t length, unsigned flags)
{
    gboolean target = output_is_target(out);
    if (target) length = MIN(length, (size_t)CHECKPOINT);
    ssize_t result = syscall(SYS_copy_file_range, in, in_offset, out,
                             out_offset, length, flags);
    return target ? checkpoint(out, result) : result;
}

ssize_t splice(int in, off64_t *in_offset, int out, off64_t *out_offset,
               size_t length, unsigned flags)
{
    gboolean target = output_is_target(out);
    if (target) length = MIN(length, (size_t)CHECKPOINT);
    ssize_t result = syscall(SYS_splice, in, in_offset, out, out_offset,
                             length, flags);
    return target ? checkpoint(out, result) : result;
}

ssize_t write(int fd, const void *buffer, size_t length)
{
    gboolean target = output_is_target(fd);
    if (target) length = MIN(length, (size_t)CHECKPOINT);
    ssize_t result = syscall(SYS_write, fd, buffer, length);
    return target ? checkpoint(fd, result) : result;
}


static void report_progress(goffset current, goffset total, gpointer data)
{
    CopyContext *ctx = data;
    /* No percentage is invented for bytes still only present in the cache. */
    transfer_progress(ctx->transfer, ctx->durable ? MIN(current, ctx->synced)
                                                : current, total);
    if (ctx->callback) ctx->callback(current, total, ctx->data);
}

static gboolean sync_destination(GFile *destination, GError **error)
{
    char *path = g_file_get_path(destination);
    if (!path) return TRUE; /* Remote backends retain their own guarantees. */
    int saved = 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) saved = errno;
    else {
        int result;
        do result = fsync(fd); while (result < 0 && errno == EINTR);
        if (result < 0) saved = errno;
        close(fd);
    }
    if (!saved) {
        char *parent = g_path_get_dirname(path);
        fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        g_free(parent);
        if (fd < 0) saved = errno;
        else {
            int result;
            do result = fsync(fd); while (result < 0 && errno == EINTR);
            if (result < 0) saved = errno;
            close(fd);
        }
    }
    if (saved)
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved),
                    "Could not synchronize %s: %s", path, g_strerror(saved));
    g_free(path);
    return saved == 0;
}

typedef gboolean (*FileOperation)(GFile *, GFile *, GFileCopyFlags,
    GCancellable *, GFileProgressCallback, gpointer, GError **);

static gboolean run_operation(FileOperation original, gboolean move,
    GFile *source, GFile *destination, GFileCopyFlags flags,
    GCancellable *cancellable, GFileProgressCallback callback,
    gpointer data, GError **error)
{
    /* GIO can call copy internally while falling back from a cross-device
     * move. Leave that inner call inside the outer operation's context. */
    if (!progress_enabled() || copy_context)
        return original(source, destination, flags, cancellable, callback, data, error);

    GFileInfo *info = g_file_query_info(source,
        G_FILE_ATTRIBUTE_STANDARD_TYPE "," G_FILE_ATTRIBUTE_STANDARD_SIZE,
        flags & G_FILE_COPY_NOFOLLOW_SYMLINKS ? G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS
                                           : G_FILE_QUERY_INFO_NONE,
        cancellable, NULL);
    if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
        g_clear_object(&info);
        return original(source, destination, flags, cancellable, callback, data, error);
    }
    CopyContext ctx = { .callback = callback, .data = data };
    gboolean local = destination_device(destination, &ctx.device);
    ctx.durable = local && (removable_device(ctx.device) ||
        g_strcmp0(g_getenv("NEMO_PROGRESS_TEST_DURABLE"), "1") == 0);
    char *name = g_file_get_basename(source);
    ctx.transfer = transfer_begin(name, move, ctx.durable, local);
    g_free(name);
    transfer_progress(ctx.transfer, 0, g_file_info_get_size(info));
    g_object_unref(info);
    CopyContext *previous = copy_context;
    copy_context = &ctx;
    GError *operation_error = NULL;
    gboolean result = original(source, destination, flags, cancellable,
                              report_progress, &ctx, &operation_error);
    copy_context = previous;
    if (result && ctx.sync_error) {
        result = FALSE;
        g_set_error(&operation_error, G_IO_ERROR,
            g_io_error_from_errno(ctx.sync_error), "Write synchronization failed: %s",
            g_strerror(ctx.sync_error));
    }
    if (result && local) {
        transfer_phase(ctx.transfer, TRANSFER_SYNC, NULL);
        result = sync_destination(destination, &operation_error);
    }
    TransferPhase phase = result ? TRANSFER_DONE :
        g_error_matches(operation_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)
            ? TRANSFER_CANCELLED : TRANSFER_ERROR;
    transfer_phase(ctx.transfer, phase,
                   operation_error ? operation_error->message : NULL);
    transfer_release(ctx.transfer);
    if (operation_error) g_propagate_error(error, operation_error);
    return result;
}

gboolean g_file_copy(GFile *source, GFile *destination, GFileCopyFlags flags,
    GCancellable *cancellable, GFileProgressCallback callback,
    gpointer data, GError **error)
{
    FileOperation original = (FileOperation)dlsym(RTLD_NEXT, "g_file_copy");
    g_return_val_if_fail(original != NULL, FALSE);
    return run_operation(original, FALSE, source, destination, flags,
                         cancellable, callback, data, error);
}
