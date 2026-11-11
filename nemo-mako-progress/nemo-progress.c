/* A Nemo-only GTK module and GIO copy bridge. Workers publish byte counts;
 * only the GTK thread talks to Mako. No private Nemo structs or GTK scraping.
 */
#include <gtk/gtk.h>
#include <gmodule.h>
#include <math.h>
#include "progress.h"

struct Transfer {
    gint references;
    GMutex mutex;
    char *name, *error;
    gboolean move, durable, local;
    goffset current, total;
    TransferPhase phase;
    double displayed;
    guint64 id;
};

static gint enabled;
static GPtrArray *transfers;
static Transfer *last_finished;
static GDBusConnection *bus;
static guint notification_id, tick_id;
static gboolean in_flight, dismissed, held;
static guint64 next_id, displayed_id;
static gint last_pixel = -1, last_phase = -1, last_percent = -1;
static gint64 last_tick, retry_after, final_deadline;
static GApplication *application;

gboolean progress_enabled(void) { return g_atomic_int_get(&enabled); }

static Transfer *transfer_ref(Transfer *transfer)
{
    g_atomic_int_inc(&transfer->references);
    return transfer;
}

void transfer_release(Transfer *transfer)
{
    if (!g_atomic_int_dec_and_test(&transfer->references)) return;
    g_mutex_clear(&transfer->mutex);
    g_free(transfer->name);
    g_free(transfer->error);
    g_free(transfer);
}

static void log_event(Transfer *transfer, const char *event)
{
    if (!g_getenv("NEMO_PROGRESS_DEBUG")) return;
    g_message("nemo-progress: id=%" G_GUINT64_FORMAT " event=%s phase=%d bytes=%"
        G_GOFFSET_FORMAT "/%" G_GOFFSET_FORMAT " shown=%.5f durable=%d name=%s%s%s",
        transfer->id, event, transfer->phase, transfer->current, transfer->total,
        transfer->displayed, transfer->durable, transfer->name, transfer->error ? " error=" : "",
        transfer->error ? transfer->error : "");
}

static gboolean tick(gpointer unused);

static gboolean add_transfer(gpointer data)
{
    Transfer *transfer = data;
    transfer->id = ++next_id;
    if (!transfers->len) {
        dismissed = FALSE;
        if (last_finished) transfer_release(last_finished);
        last_finished = NULL;
    }
    g_ptr_array_add(transfers, transfer);
    if (!held && (application = g_application_get_default())) {
        g_application_hold(application);
        held = TRUE;
    }
    g_mutex_lock(&transfer->mutex);
    log_event(transfer, "begin");
    g_mutex_unlock(&transfer->mutex);
    final_deadline = 0;
    if (!tick_id) {
        last_tick = g_get_monotonic_time();
        tick_id = g_timeout_add(33, tick, NULL);
    }
    return G_SOURCE_REMOVE;
}

Transfer *transfer_begin(const char *name, gboolean move, gboolean durable,
                         gboolean local)
{
    Transfer *transfer = g_new0(Transfer, 1);
    transfer->references = 1;
    g_mutex_init(&transfer->mutex);
    transfer->name = g_utf8_make_valid(name ? name : "檔案", -1);
    transfer->move = move;
    transfer->durable = durable;
    transfer->local = local;
    g_idle_add(add_transfer, transfer_ref(transfer));
    return transfer;
}

void transfer_progress(Transfer *transfer, goffset current, goffset total)
{
    g_mutex_lock(&transfer->mutex);
    transfer->current = MAX(transfer->current, current);
    transfer->total = MAX(transfer->total, total);
    g_mutex_unlock(&transfer->mutex);
}

void transfer_phase(Transfer *transfer, TransferPhase phase, const char *error)
{
    g_mutex_lock(&transfer->mutex);
    transfer->phase = phase;
    if (phase == TRANSFER_DONE) transfer->current = transfer->total;
    g_free(transfer->error);
    transfer->error = error ? g_utf8_make_valid(error, -1) : NULL;
    g_mutex_unlock(&transfer->mutex);
}

static void notify_done(GObject *object, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    GError *error = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(object), result, &error);
    in_flight = FALSE;
    if (reply) {
        g_variant_get(reply, "(u)", &notification_id);
        g_variant_unref(reply);
    } else {
        /* A missing/restarting notification daemon must not affect copying. */
        retry_after = g_get_monotonic_time() + G_USEC_PER_SEC;
        notification_id = 0;
        last_pixel = -1;
        g_debug("Nemo notification unavailable: %s", error->message);
        g_clear_error(&error);
    }
}

static void send_notification(Transfer *transfer, TransferPhase phase,
                               goffset current, goffset total, const char *error)
{
    if (!bus || in_flight || dismissed || g_get_monotonic_time() < retry_after) return;
    int pixel = (int)lround(transfer->displayed * 300 * 8);
    int percent = (int)floor(transfer->displayed * 100 + 0.00001);
    if (displayed_id == transfer->id && pixel == last_pixel &&
        (int)phase == last_phase && percent == last_percent) return;

    char *title;
    gboolean finished = phase >= TRANSFER_DONE;
    if (phase == TRANSFER_DONE) title = g_strdup(transfer->local
        ? "複製完成 · 已寫入磁碟" : "複製工作完成");
    else if (phase == TRANSFER_ERROR) title = g_strdup("複製失敗");
    else if (phase == TRANSFER_CANCELLED) title = g_strdup("已取消複製");
    else if (phase == TRANSFER_SYNC) title = g_strdup_printf("確認寫入 · %d%%", percent);
    else title = g_strdup_printf("%s%s · %d%%",
        transfer->move ? "移動" : "複製", transfer->durable ? "並寫入" : "中", percent);
    if (transfer->move && phase == TRANSFER_DONE) {
        g_free(title);
        title = g_strdup(transfer->local ? "移動完成 · 已寫入磁碟" : "移動工作完成");
    }
    char *size = g_format_size((guint64)MAX(current, 0));
    char *full = g_format_size((guint64)MAX(total, 0));
    char *safe_name = g_markup_escape_text(transfer->name, -1);
    char *safe_error = error ? g_markup_escape_text(error, -1) : NULL;
    char *body = phase == TRANSFER_ERROR
        ? g_strdup_printf("%s\n%s", safe_name, safe_error ? safe_error : "請查看 Nemo")
        : g_strdup_printf("%s\n%s / %s%s", safe_name, size, full,
            phase == TRANSFER_DONE ? " · 拔除前請彈出" : "");

    /* Raw pixels skip SVG decoding entirely. Antialias the fractional leading
     * edge, allowing subpixel easing while never exceeding confirmed bytes. */
    guint8 pixels[300 * 4 * 3];
    double width = transfer->displayed * 300;
    const guint8 track[] = { 0x45, 0x47, 0x5a };
    const guint8 purple[] = { 0xcb, 0xa6, 0xf7 };
    for (int y = 0; y < 4; y++) for (int x = 0; x < 300; x++) {
        double amount = CLAMP(width - x, 0, 1);
        for (int c = 0; c < 3; c++)
            pixels[(y * 300 + x) * 3 + c] = (guint8)lround(
                track[c] + amount * (purple[c] - track[c]));
    }
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string("nemo-progress"));
    g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("transfer"));
    g_variant_builder_add(&hints, "{sv}", "transient", g_variant_new_boolean(TRUE));
    g_variant_builder_add(&hints, "{sv}", "image-data",
        g_variant_new("(iiibii@ay)", 300, 4, 900, FALSE, 8, 3,
            g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, pixels, sizeof(pixels), 1)));
    /* Mako 1.11 frees a live notification if both replaces_id and a stack tag
     * name it. Use only the standard replaces_id, never a synchronous tag. */
    const gchar *actions[] = { NULL };
    GVariant *args = g_variant_new("(susss^as@a{sv}i)", "Nemo Progress",
        notification_id, "", title, body, actions,
        g_variant_builder_end(&hints), finished ? (phase == TRANSFER_ERROR ? 0 : 6000) : 0);
    in_flight = TRUE;
    displayed_id = transfer->id;
    last_pixel = pixel;
    last_phase = phase;
    last_percent = percent;
    log_event(transfer, "notify");
    g_dbus_connection_call(bus, "org.freedesktop.Notifications",
        "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
        args, G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, 3000, NULL, notify_done, NULL);
    g_free(title); g_free(body); g_free(size); g_free(full);
    g_free(safe_name); g_free(safe_error);
}

static gboolean tick(gpointer unused)
{
    (void)unused;
    gint64 now = g_get_monotonic_time();
    double dt = MIN((now - last_tick) / (double)G_USEC_PER_SEC, 0.1);
    last_tick = now;
    for (guint i = 0; i < transfers->len;) {
        Transfer *transfer = g_ptr_array_index(transfers, i);
        g_mutex_lock(&transfer->mutex);
        TransferPhase phase = transfer->phase;
        double target = phase == TRANSFER_DONE ? 1 :
            transfer->total > 0 ? MIN(0.99, (double)transfer->current / transfer->total) : 0;
        transfer->displayed += MAX(0, target - transfer->displayed) * (1 - exp(-dt / 0.14));
        if (target - transfer->displayed < 0.0005) transfer->displayed = target;
        gboolean finished = phase >= TRANSFER_DONE &&
            (phase != TRANSFER_DONE || transfer->displayed >= 1);
        g_mutex_unlock(&transfer->mutex);
        if (finished) {
            if (last_finished) transfer_release(last_finished);
            last_finished = transfer_ref(transfer);
            log_event(transfer, "finished");
            g_ptr_array_remove_index(transfers, i);
        } else i++;
    }
    Transfer *visible = transfers->len ? g_ptr_array_index(transfers, 0) : last_finished;
    if (visible) {
        g_mutex_lock(&visible->mutex);
        TransferPhase phase = visible->phase;
        if (phase == TRANSFER_DONE && visible->displayed < 1) phase = TRANSFER_SYNC;
        send_notification(visible, phase, visible->current, visible->total, visible->error);
        g_mutex_unlock(&visible->mutex);
    }
    if (!transfers->len && !final_deadline) final_deadline = now + 5 * G_USEC_PER_SEC;
    gboolean delivered = visible && last_pixel >= 0 &&
        displayed_id == visible->id && last_phase >= TRANSFER_DONE;
    if (!transfers->len && !in_flight &&
        (dismissed || delivered || now >= final_deadline)) {
        if (held) { g_application_release(application); held = FALSE; }
        tick_id = 0;
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void closed(GDBusConnection *connection, const gchar *sender,
    const gchar *path, const gchar *interface, const gchar *signal,
    GVariant *parameters, gpointer unused)
{
    (void)connection; (void)sender; (void)path; (void)interface; (void)signal; (void)unused;
    guint id, reason;
    g_variant_get(parameters, "(uu)", &id, &reason);
    if (id == notification_id) {
        notification_id = 0;
        if (reason == 2) dismissed = TRUE;
    }
}

static void bus_ready(GObject *object, GAsyncResult *result, gpointer unused)
{
    (void)object; (void)unused;
    bus = g_bus_get_finish(result, NULL);
    if (!bus) return;
    g_dbus_connection_signal_subscribe(bus, "org.freedesktop.Notifications",
        "org.freedesktop.Notifications", "NotificationClosed", "/org/freedesktop/Notifications",
        NULL, G_DBUS_SIGNAL_FLAGS_NONE, closed, NULL, NULL);
}

G_MODULE_EXPORT void gtk_module_init(gint *argc, gchar ***argv)
{
    (void)argc; (void)argv;
    if (progress_enabled() || g_strcmp0(g_get_prgname(), "nemo") != 0) return;
    transfers = g_ptr_array_new_with_free_func((GDestroyNotify)transfer_release);
    g_bus_get(G_BUS_TYPE_SESSION, NULL, bus_ready, NULL);
    g_atomic_int_set(&enabled, TRUE);
    g_message("nemo-progress: copy bridge ready; original GTK backend preserved");
}
