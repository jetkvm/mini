#!/usr/bin/env python3

from pathlib import Path


root = Path(__file__).resolve().parents[3]
registration = (root / "components/jetkvm_cloud_registration/jetkvm_cloud_registration_core.c").read_text()
registration_esp = (root / "components/jetkvm_cloud_registration/jetkvm_cloud_registration_esp.c").read_text()
device_state = (root / "components/jetkvm_device_state/jetkvm_device_state.c").read_text()
nvs = (root / "components/jetkvm_cloud_config/jetkvm_cloud_config_nvs.c").read_text()

transaction = registration[
    registration.index("jetkvm_cloud_registration_run_transaction("):
    registration.index("const char *jetkvm_cloud_registration_status_name")
]
for required in (
    "registration_workspace_t *workspace = cJSON_malloc(sizeof(*workspace))",
    "workspace->secret_token",
    "workspace->identity",
    "workspace->staged",
    "release_workspace(workspace)",
):
    assert required in transaction
for forbidden in (
    "char secret_token[JETKVM_CLOUD_TOKEN_CAPACITY]",
    "char identity[JETKVM_CLOUD_IDENTITY_CAPACITY]",
    "jetkvm_cloud_config_t staged =",
):
    assert forbidden not in transaction

assert "jetkvm_cloud_config_t *current_config =" in registration_esp
assert "cJSON_malloc(sizeof(*current_config))" in registration_esp
assert "jetkvm_cloud_config_t current_config;" not in registration_esp

for required in (
    "cloud_config_alloc()",
    "jetkvm_memory_policy_control_alloc(sizeof(*config))",
    "cloud_config_free(staged)",
    "cloud_config_free(loaded)",
):
    assert required in device_state
for forbidden in (
    "jetkvm_cloud_config_t before =",
    "jetkvm_cloud_config_t staged =",
    "jetkvm_cloud_config_t loaded;",
):
    assert forbidden not in device_state

assert "cloud_nvs_record_t *record = record_alloc()" in nvs
assert "jetkvm_memory_policy_control_alloc(sizeof(*record))" in nvs
assert "cloud_nvs_record_t record;" not in nvs

print("cloud registration control-arena and stack-budget contract: PASS")
