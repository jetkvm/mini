# JetKVM cloud registration

This component runs the JetKVM cloud registration transaction for `device_api`. It ignores the request's `cloudApi`, posts `{"tempToken": ...}` to `{CloudURL}/devices/token` over verified HTTPS, and requires a non-empty `secretToken`. Only then does the injected OIDC validator derive the `aud:sub` identity. The save callback runs once, after every check passes.

- `jetkvm_cloud_registration_register_snapshot()` is the entry point for a device-state owner. Its save callback must compare and swap on the state generation and `CloudURL`, so a concurrent URL change is not overwritten or given credentials from the old API.
- `jetkvm_cloud_registration_run_transaction()` also takes an injected exchange, for tests.
- `jetkvm_cloud_registration_register()` reads and writes NVS directly and must be serialized against all other cloud configuration writes.

Tests: `tests/run_host_tests.sh`.
