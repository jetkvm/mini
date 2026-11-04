#pragma once

#define OIDC_FIXTURE_KID "fixture-key-2026"
#define OIDC_FIXTURE_NEW_KID "fixture-key-rotated"
#define OIDC_FIXTURE_MODULUS                                                \
    "yuOpbfVfx1OR0ks-kKIqh8eQNnpZacnG2Kv4cm_6dhv1R23mjOi44vi0OBNt"      \
    "PPzosXMHbU-Qg7lYFvLh0iVd7zObxid8dxVJA1yUTF3XEIRfhoVuzDVHpWDF"      \
    "M9Rtf4Tf-WyWEMxR05gV5uIrPJLZdqKjpbnmU3dW7ADr5btD1YdMazRR0O5f"      \
    "_fzacyFCDG7EV3zsPlKn48Ai30JCCYu-z3Gf6hoClVE5AKPh3K5eLHnepnLm"      \
    "uIE-E9YQG839pl9FsmTelEvE1CUONzkNdnKXo2S9Le-r1NubSCac1t9HdCcs"      \
    "ZU9AB-58AkOuK6T1UKNaxnARBXHLJVHgrOhp4CYelQ"
#define OIDC_FIXTURE_EXPONENT "AQAB"

#define OIDC_FIXTURE_JWKS_FOR(kid_value)                                   \
    "{\"keys\":[{\"kty\":\"EC\",\"kid\":\"ignored-ec\"},"                  \
    "{\"kty\":\"RSA\",\"use\":\"enc\",\"kid\":\"ignored-enc\"},"         \
    "{\"kty\":\"RSA\",\"use\":\"sig\",\"alg\":\"RS256\","                 \
    "\"kid\":\"" kid_value "\",\"n\":\"" OIDC_FIXTURE_MODULUS               \
    "\",\"e\":\"" OIDC_FIXTURE_EXPONENT "\"}]}"

#define OIDC_FIXTURE_JWKS OIDC_FIXTURE_JWKS_FOR(OIDC_FIXTURE_KID)
#define OIDC_FIXTURE_ROTATED_JWKS                                           \
    OIDC_FIXTURE_JWKS_FOR(OIDC_FIXTURE_NEW_KID)

#define OIDC_FIXTURE_PRIVATE_KEY                                            \
    "-----BEGIN PRIVATE KEY-----\n"                                        \
    "MIIEvgIBADANBgkqhkiG9w0BAQEFAASCBKgwggSkAgEAAoIBAQDK46lt9V/HU5HS\n" \
    "Sz6QoiqHx5A2ellpycbYq/hyb/p2G/VHbeaM6Lji+LQ4E208/OixcwdtT5CDuVgW\n" \
    "8uHSJV3vM5vGJ3x3FUkDXJRMXdcQhF+GhW7MNUelYMUz1G1/hN/5bJYQzFHTmBXm\n" \
    "4is8ktl2oqOlueZTd1bsAOvlu0PVh0xrNFHQ7l/9/NpzIUIMbsRXfOw+UqfjwCLf\n" \
    "QkIJi77PcZ/qGgKVUTkAo+Hcrl4sed6mcua4gT4T1hAbzf2mX0WyZN6US8TUJQ43\n" \
    "OQ12cpejZL0t76vU25tIJpzW30d0JyxlT0AH7nwCQ64rpPVQo1rGcBEFccslUeCs\n" \
    "6GngJh6VAgMBAAECggEAKBn2f5rbuwCcFmYz8A91e4/ow7PMH3o32NSozymt5hFh\n" \
    "miQxSRGolYgVyu2gDklozsYPJj5vMHH+ZM7N7qTfG6vmMrTUR49H9qeiUjPMEeIw\n" \
    "JV0TSL6RMjFd2QA4Qlenk1uwj5Jq6/kn/xId/KOD4wyDbvq++Slh31TRS3+HiVkS\n" \
    "EWo8Oy7zZ6Ns5G5vgU2uSRhd4Ux42KNU4s580H5BoY+m/ter+TASbvFYCILh6cgn\n" \
    "66Y0BKaiks2XzwP8S1InTQPHFRy1eqDkb4AAInYimfqZ/fjy+IvPbkJJ/XLagXRw\n" \
    "J83IGNj4FNgXYHHeroPqCytJZvQVxWQXa6leQYcUUwKBgQDrHWpdo4q9xAYOkCMH\n" \
    "ni0R84RNCLICJNFgSy0fw2z4AIcIfZ3FROBWxxMa8+zLHU7eByo4sVBoStNl3qap\n" \
    "OyXZfgL3eO+QTo0xhbD2BD3ANhnzoKAlO2o9Nw+qTqImPAZYk5AhJTlWNRYxCGCM\n" \
    "RlfjxVGSF3XKQpHw/mHtGAc/kwKBgQDc6W0nXXWHAEoCB77rMjP9S/TExArpBFOw\n" \
    "cXNNW41CWbJJHO0R8nUFioJhT3xivEfVKEdQFmpFlfCFNfQ/BPOqsR/7OlAq7hzE\n" \
    "WPwZBJ2BTrHrzMBD0f+9lHg+3zgFnTeSjBfWtWPGYSjcI0WxQaivjfam9veIV4Cc\n" \
    "BRx4b5hyNwKBgQC1x4Jrb3xfh+Ve8RYEV8lk7bBGlwh6G9k96CG1QzVG8bF1YRnQ\n" \
    "YZZJwZUF674StFA73bJJLt6KJAAZ7PFSOobjB0hn6brfCbyko0bUjybnK20ZLgFj\n" \
    "h/J17dJQzIcVpKcLaNuwpMUV3+mOc3miOujNE8wjdbv+a0XG3Ab+dutzvQKBgQCj\n" \
    "TFXovHhNNBSBDbK+b1zpB4XHEdeyygUHL8X40A1Kl71nuF8yXhTM5MdeIYCkB+Pd\n" \
    "mKmmdPtXtu+VuP10vDdW3Rnz/vYV1fAj4FdyMuwvEroIwVD6kThHroF4kE78L2xO\n" \
    "Awr4Jxu7IEUw/U3eKtgD9Z/3aiYufehO7KRQDsYU3wKBgAKbExFu0ZapgeZMmplo\n" \
    "xS0hkQym8By8rXUH25tZU3GzFCIRe3imCbbmhnip1I5XUx1ZT97J4Fjsyj1lsS8n\n" \
    "/uc20FNrTzupCA69vlkAogk5hlSKAboBdQY9GlXG0lzQQvnkbLS56BcHjnVeSBpC\n" \
    "YIx9D2S0kIr/wbbp325j4iNm\n"                                     \
    "-----END PRIVATE KEY-----\n"

