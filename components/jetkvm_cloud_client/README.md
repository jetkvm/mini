# JetKVM cloud client

This component is the device side of the JetKVM cloud WebSocket signaling protocol, built on `espressif/esp_websocket_client`. The owner passes all configuration to `jetkvm_cloud_client_init()`; the component never reads NVS. TLS always verifies against the ESP-IDF certificate bundle. `jetkvm_cloud_core.c` holds the host-testable logic (`tests/run_host_tests.sh`).
