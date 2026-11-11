#pragma once
#include <gio/gio.h>

typedef struct Transfer Transfer;
typedef enum { TRANSFER_COPY, TRANSFER_SYNC, TRANSFER_DONE,
               TRANSFER_ERROR, TRANSFER_CANCELLED } TransferPhase;

Transfer *transfer_begin(const char *name, gboolean move, gboolean durable,
                         gboolean local);
void transfer_progress(Transfer *transfer, goffset current, goffset total);
void transfer_phase(Transfer *transfer, TransferPhase phase, const char *error);
void transfer_release(Transfer *transfer);
gboolean progress_enabled(void);
