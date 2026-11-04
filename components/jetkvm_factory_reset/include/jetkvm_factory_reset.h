#pragma once
#include <stdbool.h>
#include "esp_err.h"
/* Before settings readers: load intent, show progress, then erase local data.
 * Success remains pending until radio cleanup and finish; never touches TF. */
esp_err_t jetkvm_factory_reset_boot_begin(void (*show_progress)(void));
/* Complete after radio cleanup, before provisioning. An error preserves the
 * intent for the next boot. With no pending reset, returns error unchanged. */
esp_err_t jetkvm_factory_reset_finish(esp_err_t error);

bool jetkvm_factory_reset_pending(void);
/* Boot cleanup failure, available to the UI until the next reboot retries. */
const char *jetkvm_factory_reset_error(void);
/* Worker-only. Persists reset intent and prepares a reboot worker.
 * Returns an error before acknowledging if persistence or admission fails. */
const char *jetkvm_factory_reset_request(void *ctx);

/* Release the one-second delay after response/event send attempts finish.
 * Also called when an accepted request has no surviving response channel. */
void jetkvm_factory_reset_dispatch_complete(void);
