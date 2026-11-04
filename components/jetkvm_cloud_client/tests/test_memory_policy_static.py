#!/usr/bin/env python3
"""Static contracts for bounded Cloud WSS connect-attempt ownership."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[3]
SOURCE = (ROOT / "components/jetkvm_cloud_client/jetkvm_cloud_client.c").read_text()
WSS_SOURCE = (
    ROOT
    / "managed_components/espressif__esp_websocket_client/esp_websocket_client.c"
).read_text()
CMAKE = (ROOT / "CMakeLists.txt").read_text()

for contract in (
    "websocket_memory_reservation",
    '"cloud-wss"',
    '"cloud-wss-connect"',
    "jetkvm_memory_policy_reserve_tls",
    "release_websocket_memory_reservation(client)",
    ".disable_auto_reconnect = true",
    ".enable_close_reconnect = false",
    "completed_connect_memory",
    "JETKVM_CLOUD_IDENTITY_RETRYABLE",
    "CLOUD_IDENTITY_RETRY_ATTEMPTS",
    "offers_retry_exhausted_identity",
    "websocket_lifecycle_lock",
    "jetkvm_memory_policy_control_alloc(sizeof(*client))",
    "jetkvm_memory_policy_control_free(client)",
):
    assert contract in SOURCE, f"missing WSS reservation contract: {contract}"

assert "calloc(1, sizeof(*client))" not in SOURCE

# Every attempt, the first one included, is started by the worker when the
# retry is due, so no TLS ticket is taken while the network has no address.
start_body = SOURCE[
    SOURCE.index("esp_err_t jetkvm_cloud_client_start(") :
    SOURCE.index("esp_err_t jetkvm_cloud_client_stop(")
]
assert "esp_websocket_client_start(" not in start_body
assert "jetkvm_memory_policy_reserve_tls(" not in start_body
for contract in (
    "jetkvm_cloud_retry_due(",
    "cloud_network_up()",
    "client->reconnect_timeout_ms);",
):
    assert contract in SOURCE, f"missing WSS retry contract: {contract}"
assert not re.search(r"(?<!control_)\bfree\s*\(client\)", SOURCE)

assert "xTaskCreatePinnedToCoreWithCaps(" in WSS_SOURCE
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in WSS_SOURCE
assert "vTaskDeleteWithCaps(NULL)" in WSS_SOURCE
assert "client->run = true;" in WSS_SOURCE
assert WSS_SOURCE.index("client->run = true;") < WSS_SOURCE.index(
    "xTaskCreatePinnedToCoreWithCaps("
)
task_body = WSS_SOURCE[
    WSS_SOURCE.index("static void esp_websocket_client_task(") :
    WSS_SOURCE.index("esp_err_t esp_websocket_client_start(")
]
assert "client->run = true;" not in task_body
stop_start = WSS_SOURCE.index("esp_err_t esp_websocket_client_stop(")
stop_body = WSS_SOURCE[
    stop_start : WSS_SOURCE.index(
        "static int esp_websocket_client_send_close(", stop_start
    )
]
assert "return ESP_ERR_INVALID_STATE;" in stop_body
assert (
    "esp_websocket_client-1.8.0-external-task-stack.patch" in CMAKE
), "managed WSS stack patch must be reproducible"

print("jetkvm_cloud_client bounded WSS memory contract: PASS")
