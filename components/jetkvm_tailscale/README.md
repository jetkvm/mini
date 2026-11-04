# Tailscale

This component joins the Mini to a tailnet over Ethernet or Wi-Fi with the native microlink client (WireGuard, DERP relays, STUN, DISCO). The attached computer needs no Tailscale setup.

## Interface

- `getTailscaleStatus`, `getTailscaleControlURL`, and `setTailscaleControlURL` match upstream JetKVM. Status reports `loggedIn` and `configurable`.
- `getTailscaleConfig`, `setTailscaleConfig`, and `logoutTailscale` are Mini additions for the auth key, enable state, and log out.
- A custom control server, such as Headscale, must use `https://` with a trusted certificate, because the auth key is encrypted to a key fetched from it.

## Enrollment

Settings and node identity are stored in NVS; keys never appear in RPC results. A completed enrollment stores a digest of the server and auth key, so a single-use key is not resent after reboot. An expired node blocks traffic and reports `NeedsLogin`; an auth key re-enrolls it with the same identity. Changing the control server erases the stored key. Log out erases the key and identity and completes at the next start if interrupted.

## Constraints

- Inbound IPv4 must pass the control server's packet filter. Traffic is blocked until a filter arrives.
- IPv4 only, at most 64 peers (`CONFIG_ML_MAX_PEERS`), no exit nodes, subnet routes, Taildrop, or Tailscale SSH.
- Client stacks and queues use PSRAM; the settings worker uses an internal stack because it writes NVS.
- The build requires `CONFIG_LWIP_ESP_NETIF_CLIENT_DATA`.
- Raw lwIP calls go through `wireguardif_lwip_lock()`. It takes the lwIP core lock on Ethernet, which sets `CONFIG_LWIP_TCPIP_CORE_LOCKING`, and parks the TCP/IP thread on Wi-Fi.

The client in `third_party/microlink` is exported from [jetkvm/microlink](https://github.com/jetkvm/microlink), which has its changes as commits. Host tests run with `tests/run_host_test.sh`; on-device tests in `tools/integration` and `tools/combined_load` need an auth key file named by `JETKVM_TAILSCALE_AUTH_KEY_FILE`.
