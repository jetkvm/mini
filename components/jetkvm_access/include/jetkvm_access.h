#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"
#include "access_contract.h"

/* Initialize before exposing any listener. A corrupt password record fails
 * startup closed; it must never silently become noPassword. */
esp_err_t jetkvm_access_init(void (*revoke_local)(void));
bool jetkvm_access_password_enabled(void);
bool jetkvm_access_is_setup(void);
bool jetkvm_access_epoch_valid(uint32_t epoch);
bool jetkvm_access_lock_epoch(uint32_t epoch);
void jetkvm_access_unlock(void);
esp_err_t jetkvm_access_attach(httpd_handle_t server, bool secure);
void jetkvm_access_detach(httpd_handle_t server);
esp_err_t jetkvm_access_register(httpd_handle_t server, const httpd_uri_t *uri);
#define JETKVM_ACCESS_HOST_NAME_CAPACITY 128U
/* The DNS names the device answers to besides the address a client connected
 * to: `names` fills up to `capacity` names and returns their count. A name
 * also matches with ".local" appended. */
void jetkvm_access_set_host_names(size_t (*names)(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY],
                                                  size_t capacity));
/* Other names for the device that a user gave it, which the Host check
 * accepts besides those above: at most JETKVM_ACCESS_EXTRA_HOST_NAMES DNS
 * host names, stored in lower case without a trailing dot. The setter
 * validates and stores the whole list, or returns why it took none. */
#define JETKVM_ACCESS_EXTRA_HOST_NAMES 8U
size_t jetkvm_access_get_extra_host_names(char (*names)[JETKVM_ACCESS_HOST_NAME_CAPACITY], size_t capacity);
const char *jetkvm_access_set_extra_host_names(const char *const *names, size_t count);
/* Refuses with 421 a Host that is neither the connection's address nor one of
 * the device's names. Non-OK as for jetkvm_access_check_request(), which
 * includes this check. */
esp_err_t jetkvm_access_check_host(httpd_req_t *req);
/* Non-OK means the rejection response was already sent; the handler must
 * then return jetkvm_access_rejection_result(). */
esp_err_t jetkvm_access_check_request(httpd_req_t *req, bool require_origin);
/* URI handler result after a rejection: closes only when the response said
 * so. A ws_pre_handshake_cb returns ESP_FAIL instead; the rejection response
 * already carried Connection: close because an Upgrade request cannot be
 * continued on its connection. */
esp_err_t jetkvm_access_rejection_result(httpd_req_t *req);
uint32_t jetkvm_access_request_epoch(httpd_req_t *req);
/* Idle timeout of the local login with a password (upstream
 * localAuthSessionTimeoutMinutes), stored in NVS; 0 is off. A login that saw
 * no activity for the timeout expires: a new epoch ends its session, and a
 * request with its cookie gets 401 with the code session_expired. The token
 * is in RAM only, so a restart ends the login with or without a timeout.
 * The timeout is at most JETKVM_ACCESS_MAX_IDLE_MINUTES. */
unsigned jetkvm_access_get_idle_minutes(void);
/* Returns NULL, or why the value was not taken. Counts as activity. */
const char *jetkvm_access_set_idle_minutes(unsigned minutes);
/* Activity of the local login: input and state-changing RPC calls of a local
 * session. Protected HTTP requests count by themselves. */
void jetkvm_access_note_activity(void);
/* Expires an idle login; true if it did. Cheap while the login is not idle,
 * so the signaling socket's poll calls it. */
bool jetkvm_access_expire_idle(void);
/* True for the epoch that the last idle expiry ended. */
bool jetkvm_access_epoch_expired(uint32_t epoch);
