# JetKVM local access

This component owns local authentication: onboarding, the password, the session cookie, the `Host` check and TLS settings. It implements the upstream JetKVM local access contract, so the upstream Welcome, Access and Login pages work unchanged.

## Interface

- Onboarding: `GET /device/status`, and `POST /device/setup` with `localAuthMode` set to `"password"` (plus `password`) or `"noPassword"`.
- Session: `POST` and `PUT /auth/password-local`, `DELETE /auth/local-password`, `POST /auth/login-local`, `POST /auth/logout`.
- RPCs, through `control_protocol`: `getTLSState`, `setTLSState`, `setExtraHostnames`, `getLocalAuthSessionTimeout`, `setLocalAuthSessionTimeout`.
- HTTP servers call `jetkvm_access_attach()` and `jetkvm_access_register()`; handlers call `jetkvm_access_check_request()`.

## Behavior

- The password record in NVS also marks setup complete; a saved disabled record means no-password mode. Until setup, guarded routes and the signaling WebSocket return 401 `Setup required`. Factory reset erases the record.
- Passwords use PBKDF2-HMAC-SHA256 with 200,000 iterations on the ESP32-P4 SHA engine. The derivation releases the shared SHA/AES lock after at most 32 HMACs.
- The `authToken` cookie lasts seven days, with `HttpOnly` and `SameSite=Strict` but not `Secure`, as upstream, so HTTP login still works after TLS is disabled.
- Login, password change and logout advance the local authority epoch, invalidating old requests, WebSockets, queued HID input and uploads. Cloud sessions have their own authority.
- Session timeout (upstream jetkvm/kvm#1788): with a password, a login that has no activity for 1 to 1440 minutes expires. 0 (the default) turns the timeout off. The value is in NVS (`jk_access/idle_minutes`). Activity is a protected HTTP request other than signaling, HID input other than the keepalive of a held key, and an RPC call that changes state (not `get*`, `list*`, `check*`, `is*` or `ping`) of a local session. At expiry the epoch advances, which ends the local session; its signaling socket closes with code 4003. A request with the expired cookie gets 401 with `"code":"session_expired"` and a cleared cookie. The poll of an open signaling socket and every protected request check for expiry.
- Before setup and in no-password mode, requests must carry a `Host` that is the connected address, the hostname (optionally `.local`), a Tailscale name, or one of up to 8 extra names. Others get 421, which blocks DNS rebinding.
- HTTP and HTTPS serve the same routes. If HTTPS fails to start, authenticated HTTP stays up. A failed TLS change restores the previous configuration. The private key is never returned.

## Constraints

- Call `jetkvm_access_init()` before any listener starts. A malformed password record fails startup closed.
- ESP-IDF 6.0.2's PKCS#8 parser omits the RSA public key, so `jetkvm_tls.c` calls `mbedtls_pk_set_pubkey_from_prv()` before `mbedtls_pk_check_pair()`.

Tests: `tests/run_password_test.sh`. `test_apps/pbkdf2` is a standalone hardware benchmark that leaves NVS untouched; reflash the normal application afterwards.
