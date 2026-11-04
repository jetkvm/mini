/* Compile/link coverage for the public ESP adapter. */

#include "jetkvm_oidc.h"

void app_main(void)
{
    jetkvm_oidc_config_t config;
    jetkvm_oidc_default_config(&config);

    /* The default is nonzero. Keeping this runtime branch forces every public
     * adapter entry point and its crypto/HTTP dependencies through the linker
     * without performing a network request when this smoke app is run. */
    if (config.http_timeout_ms == 0U) {
        jetkvm_oidc_validator_t *validator = NULL;
        char identity[JETKVM_OIDC_IDENTITY_CAPACITY];
        if (jetkvm_oidc_validator_create(&config, &validator) ==
            JETKVM_OIDC_OK) {
            (void)jetkvm_oidc_validate_registration(
                validator, "invalid", "client", identity, sizeof(identity));
            (void)jetkvm_oidc_validate_session(
                validator, "invalid", identity, sizeof(identity));
            jetkvm_oidc_validator_invalidate_keys(validator);
            jetkvm_oidc_validator_destroy(validator);
        }
    }
}
