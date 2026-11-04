#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Route ESP_LOG output through a PSRAM queue that a low-priority task writes
 * to the console. The console UART is unbuffered at 115200 baud (11.5 KB/s),
 * so without this every task that logs waits for all console output before
 * it: 170 ms behind TinyUSB's descriptor table. A log call now formats its
 * message into the queue and returns; when the queue is full the message is
 * dropped and counted, and the console task reports the count (at most once a
 * second). A message longer than half the queue is cut. Output printed before
 * this call, direct printf(), and panic output keep the blocking path. */
esp_err_t jetkvm_console_start(void);

/* Send later messages straight to the console, wait up to 500 ms for the
 * console task to write the queued ones, and wait for the UART to finish.
 * Called on restart so the last lines are not lost. */
void jetkvm_console_flush(void);

#ifdef __cplusplus
}
#endif
