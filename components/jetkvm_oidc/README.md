# JetKVM Google OIDC validator

This component validates the Google ID token in the upstream JetKVM cloud protocol. `jetkvm_oidc_validate_registration()` requires the client ID in `aud`. `jetkvm_oidc_validate_session()` returns `aud[0]:sub` for exact comparison with the registered identity.

It accepts only `RS256` tokens from `https://accounts.google.com` or `accounts.google.com`, and checks `exp` and `iat` against a trusted wall clock with 60 seconds of skew. An optional `nbf` gets five minutes, as in go-oidc v3.16. It fetches keys only from Google's JSON Web Key Set (JWKS) URL over verified HTTPS, or from the mock cloud in `JETKVM_MOCK_CLOUD_JWKS_URL` bench builds. All inputs are size-limited and lexically checked before cJSON parses them. Keys are cached per `Cache-Control: max-age`, with rate-limited refresh. Tokens and identities are never logged.

`jetkvm_oidc_core.c` is platform-neutral, `jetkvm_oidc_mbedtls.c` verifies signatures, and `jetkvm_oidc_esp.c` adapts HTTPS, clock and locking. Depends on `espressif/cjson` `^1.7.19`. Tests: `tests/run_host_tests.sh`.
