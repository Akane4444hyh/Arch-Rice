#include <gtk/gtk.h>
#include <glib/gstdio.h>
#include <fcntl.h>
#include <unistd.h>

static GMainLoop *loop;
static char *directory, *source_path;

static void cancel_progress(goffset current, goffset total, gpointer data)
{
    (void)total;
    if (current >= 512 * 1024) g_cancellable_cancel(data);
}

static gboolean quit_loop(gpointer unused)
{
    (void)unused;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gpointer exercise(gpointer unused)
{
    (void)unused;
    GFile *source = g_file_new_for_path(source_path);
    char *destination_path = g_build_filename(directory, "copy.dat", NULL);
    GFile *destination = g_file_new_for_path(destination_path);
    GError *error = NULL;
    gboolean result = g_file_copy(source, destination, G_FILE_COPY_NONE,
                                  NULL, NULL, NULL, &error);
    if (g_getenv("NEMO_PROGRESS_EXPECT_IO_ERROR")) {
        g_assert_false(result);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
        g_clear_error(&error);
        g_print("PASS: sync failure propagated as a copy error\n");
        goto out;
    }
    g_assert_no_error(error);
    g_assert_true(result);
    char *a, *b;
    gsize a_length, b_length;
    g_assert_true(g_file_get_contents(source_path, &a, &a_length, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(destination_path, &b, &b_length, &error));
    g_assert_no_error(error);
    g_assert_cmpmem(a, a_length, b, b_length);
    g_free(a); g_free(b);

    g_assert_false(g_file_copy(source, destination, G_FILE_COPY_NONE,
                               NULL, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_EXISTS);
    g_clear_error(&error);
    g_assert_true(g_file_copy(source, destination, G_FILE_COPY_OVERWRITE,
                              NULL, NULL, NULL, &error));
    g_assert_no_error(error);

    g_assert_true(g_file_delete(destination, NULL, &error));
    g_assert_no_error(error);
    GCancellable *cancel = g_cancellable_new();
    g_assert_false(g_file_copy(source, destination, G_FILE_COPY_NONE,
                               cancel, cancel_progress, cancel, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancel);
    g_file_delete(destination, NULL, NULL);

    char *link_path = g_build_filename(directory, "source.link", NULL);
    g_assert_cmpint(symlink(source_path, link_path), ==, 0);
    GFile *link = g_file_new_for_path(link_path);
    g_assert_true(g_file_copy(link, destination, G_FILE_COPY_NOFOLLOW_SYMLINKS,
                              NULL, NULL, NULL, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(destination_path, G_FILE_TEST_IS_SYMLINK));
    g_file_delete(destination, NULL, NULL);
    g_object_unref(link);
    g_unlink(link_path);
    g_free(link_path);

    g_assert_true(g_file_move(source, destination, G_FILE_COPY_NONE,
                              NULL, NULL, NULL, &error));
    g_assert_no_error(error);
    g_assert_false(g_file_test(source_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(destination_path, G_FILE_TEST_IS_REGULAR));
    g_print("PASS: contents, overwrite, exists error, cancellation, symlink, cross-device move\n");
out:
    g_file_delete(destination, NULL, NULL);
    g_free(destination_path);
    g_object_unref(destination); g_object_unref(source);
    /* Let the real GTK notification path finish its eased completion frame. */
    g_timeout_add(1600, quit_loop, NULL);
    return NULL;
}

int main(int argc, char **argv)
{
    g_set_prgname("nemo");
    gtk_init(&argc, &argv);
    GError *error = NULL;
    directory = g_dir_make_tmp("nemo-copy-test-XXXXXX", &error);
    g_assert_no_error(error);
    /* tmpfs -> local filesystem exercises GIO's real cross-device fallback. */
    int fd = g_file_open_tmp("unused-XXXXXX", &source_path, &error);
    g_assert_no_error(error);
    close(fd); g_unlink(source_path); g_free(source_path);
    source_path = g_strdup("/dev/shm/nemo-copy-test-XXXXXX");
    fd = g_mkstemp(source_path);
    g_assert_cmpint(fd, >=, 0);
    guint8 *bytes = g_malloc(4 * 1024 * 1024);
    for (guint i = 0; i < 4 * 1024 * 1024; i++) bytes[i] = (guint8)(i * 31 + i / 251);
    g_assert_cmpint(write(fd, bytes, 4 * 1024 * 1024), ==, 4 * 1024 * 1024);
    g_free(bytes); close(fd);
    loop = g_main_loop_new(NULL, FALSE);
    GThread *worker = g_thread_new("real-copy", exercise, NULL);
    g_main_loop_run(loop);
    g_thread_join(worker);
    g_unlink(source_path); g_rmdir(directory);
    g_free(source_path); g_free(directory);
    g_main_loop_unref(loop);
    return 0;
}
