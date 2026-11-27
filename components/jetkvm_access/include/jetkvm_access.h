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
/* `listener` runs on the HTTP task after POST /device/setup completes, with
 * no access lock held. */
void jetkvm_access_set_setup_listener(void (*listener)(void));
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
 * the device's names. The answer says how to add a name: a help page for a
 * browser that opens a page, JSON with a hint for every other request.
 * Non-OK as for jetkvm_access_check_request(), which includes this check. */
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
/* "Web interface through Tailscale only", stored in NVS and off by default.
 * While it is on, the web server admits a connection only when it arrived on
 * the device's Tailscale address from a Tailscale address
 * (jetkvm_access_tailscale_only_admits()), so the device does not answer on
 * its local network addresses or to a host of the local network. A factory reset
 * turns it off, as it erases every setting. */
bool jetkvm_access_get_tailscale_only(void);
/* Returns NULL, or why the value was not stored. It is not turned on while
 * the device has no Tailscale address. Turning it on also closes the open
 * connections that the rule does not admit. The caller decides who may turn
 * it on. */
const char *jetkvm_access_set_tailscale_only(bool enabled);
/* `address` fills the 4 bytes of the device's Tailscale IPv4 address and
 * returns true, or returns false while the device has none. Without a
 * provider the device has no Tailscale address. */
void jetkvm_access_set_tailscale_address(bool (*address)(uint8_t ipv4[4]));
/* True when the connection of `fd` arrived on the device's Tailscale address
 * from a Tailscale address (jetkvm_access_arrived_through_tailscale()). */
bool jetkvm_access_socket_through_tailscale(int fd);
/* False for a connection that the Tailscale-only setting does not admit. The
 * web server closes such a connection when it opens; the Host check closes
 * one that was open before the setting turned on. */
bool jetkvm_access_admits_socket(int fd);
