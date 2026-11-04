#!/usr/bin/env python3
"""Fail-closed source contract for the target-USB lifecycle owner."""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
REPO = ROOT.parents[1]
SOURCE = (ROOT / "jetkvm_usb_service.c").read_text(encoding="utf-8")
HEADER = (ROOT / "include" / "jetkvm_usb_service.h").read_text(encoding="utf-8")
MAIN = (REPO / "main" / "main.c").read_text(encoding="utf-8")


def body(name: str) -> str:
    match = re.search(rf"\b{name}\s*\([^)]*\)\s*\{{", SOURCE)
    assert match, f"missing {name}"
    start = match.end()
    depth = 1
    cursor = start
    while cursor < len(SOURCE) and depth:
        if SOURCE[cursor] == "{":
            depth += 1
        elif SOURCE[cursor] == "}":
            depth -= 1
        cursor += 1
    assert depth == 0, f"unterminated {name}"
    return SOURCE[start : cursor - 1]


def matching_blocks(source: str, condition: str) -> list[str]:
    """Return every braced `if (condition)` body, including nested braces."""
    blocks: list[str] = []
    pattern = re.compile(rf"\bif\s*\(\s*{re.escape(condition)}\s*\)\s*\{{")
    for match in pattern.finditer(source):
        start = match.end()
        depth = 1
        cursor = start
        while cursor < len(source) and depth:
            if source[cursor] == "{":
                depth += 1
            elif source[cursor] == "}":
                depth -= 1
            cursor += 1
        assert depth == 0, f"unterminated if ({condition})"
        blocks.append(source[start : cursor - 1])
    return blocks


def call_position(source: str, name: str, first_argument: str = "") -> int:
    argument = rf"\s*{re.escape(first_argument)}" if first_argument else ""
    match = re.search(rf"\b{re.escape(name)}\s*\({argument}", source)
    assert match, f"missing {name}({first_argument}...)"
    return match.start()


def production_occurrences(name: str) -> dict[Path, int]:
    """Find API definitions/calls in production C, excluding all test trees."""
    occurrences: dict[Path, int] = {}
    pattern = re.compile(rf"\b{re.escape(name)}\s*\(")
    roots = (REPO / "components", REPO / "main")
    for search_root in roots:
        for path in search_root.rglob("*.c"):
            relative = path.relative_to(REPO)
            if "tests" in relative.parts:
                continue
            count = len(pattern.findall(path.read_text(encoding="utf-8")))
            if count:
                occurrences[relative] = count
    return occurrences


assert "USB_OWNER_TASK_CORE       0" in SOURCE
assert "xTaskNotifyGive(owner_task)" in SOURCE
assert "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)" in SOURCE
assert "command_queue" not in SOURCE
assert "jetkvm_usb_service_start(usb_service_state_changed, NULL)" in MAIN
assert "ret = jetkvm_usb_hid_init();" not in MAIN
assert "ret = jetkvm_hid_input_init();" not in MAIN
callback = re.search(
    r"static void usb_service_state_changed\s*\([^)]*\)\s*\{(.*?)\n\}",
    MAIN,
    re.S,
)
assert callback, "missing USB capability callback"
assert "jetkvm_hid_input_get_snapshot" in callback.group(1)
assert "p4_device_set_hid_available(hid_protocol_ready)" in callback.group(1)
assert "p4_device_set_hid_available(snapshot->hid_input_ready)" not in callback.group(1)

owner = body("apply_latest_enabled")
assert "enable_usb_stack" in owner
assert "disable_usb_stack" in owner
disable = body("disable_usb_stack")
assert disable.index("jetkvm_hid_input_set_usb_available(false)") < disable.index(
    "jetkvm_usb_hid_deinit"
)
assert "jetkvm_hid_input_deinit" not in disable

request = body("jetkvm_usb_service_request_enabled")
for forbidden in ("vTaskDelay", "xQueueReceive", "xSemaphoreTake", "portMAX_DELAY"):
    assert forbidden not in request, f"RPC-facing request path blocks via {forbidden}"

# This component is the only production consumer of target-HID lifecycle,
# descriptor selection and persistent USB configuration APIs. The provider
# source itself is allowed because it contains each API definition.
service_path = Path("components/jetkvm_usb_service/jetkvm_usb_service.c")
hid_provider_path = Path("components/jetkvm_usb_hid/jetkvm_usb_hid.c")
config_provider_path = Path(
    "components/jetkvm_usb_config/jetkvm_usb_config_nvs.c"
)
owned_apis = {
    "jetkvm_usb_hid_set_descriptor_configuration": (hid_provider_path, 1),
    "jetkvm_usb_hid_init": (hid_provider_path, 1),
    "jetkvm_usb_hid_deinit": (hid_provider_path, 1),
    "jetkvm_usb_config_load": (config_provider_path, 1),
    # One candidate commit and one previous-config repair path.
    "jetkvm_usb_config_save": (config_provider_path, 2),
}
for api, (provider_path, service_call_count) in owned_apis.items():
    occurrences = production_occurrences(api)
    unexpected = set(occurrences) - {service_path, provider_path}
    assert not unexpected, f"{api} has non-owner callers: {sorted(unexpected)}"
    assert occurrences.get(service_path) == service_call_count, (
        f"{api} owner call count changed: {occurrences.get(service_path, 0)}"
    )
    assert provider_path in occurrences, f"missing provider definition for {api}"

descriptor_selector = body("select_descriptor_configuration")
assert descriptor_selector.count(
    "jetkvm_usb_hid_set_descriptor_configuration"
) == 1
enable = body("enable_usb_stack")
assert enable.count("jetkvm_usb_hid_init") == 1
assert "cleanup_error = disable_usb_stack()" in enable
assert disable.count("jetkvm_usb_hid_deinit") == 1
start = body("jetkvm_usb_service_start")
assert start.count("jetkvm_usb_config_load") == 1

# A configuration RPC owns no hardware work. It copies one request into the
# owner's single pending slot, notifies that owner, and waits for the matching
# completion revision before publishing the result to its caller.
configuration_request = body("apply_configuration_request")
for forbidden in (
    "apply_pending_configuration",
    "select_descriptor_configuration",
    "enable_usb_stack",
    "disable_usb_stack",
    "restore_configuration",
    "jetkvm_usb_hid_set_descriptor_configuration",
    "jetkvm_usb_hid_init",
    "jetkvm_usb_hid_deinit",
    "jetkvm_usb_config_load",
    "jetkvm_usb_config_save",
):
    assert forbidden not in configuration_request, (
        f"configuration caller directly executes owner work via {forbidden}"
    )
pending_position = configuration_request.index(
    "s_ctx.state.configuration_pending = true"
)
notify_position = configuration_request.index("xTaskNotifyGive(owner_task)")
completion_wait = re.search(
    r"xSemaphoreTake\s*\(\s*configuration_done",
    configuration_request[notify_position:],
)
assert pending_position < notify_position
assert completion_wait, "configuration caller never waits for owner completion"
assert "publish_state()" not in configuration_request
assert "s_ctx.configuration_completed_revision == request_revision" in (
    configuration_request
)
assert configuration_request.index(
    "s_ctx.configuration_completed_revision == request_revision"
) > notify_position

owner_task = body("usb_owner_task")
assert "configuration_pending = s_ctx.state.configuration_pending" in owner_task
assert "apply_pending_configuration();" in owner_task
assert SOURCE.count("apply_pending_configuration(") == 2, (
    "configuration executor must only be defined once and called by the owner"
)

# Latest-state mailboxes are intentional; a FIFO would allow obsolete USB
# configurations or enable states to execute after a newer request.
for forbidden in (
    "QueueHandle_t",
    "StaticQueue_t",
    "xQueue",
    "command_queue",
    "configuration_queue",
):
    assert forbidden not in SOURCE, f"USB owner gained a FIFO via {forbidden}"
assert SOURCE.count("jetkvm_usb_config_t requested_configuration;") == 1
assert "s_ctx.state.configuration_pending = true" in configuration_request
assert "s_ctx.configuration_force_reenumeration = force_reenumeration" in (
    configuration_request
)
assert "apply_configuration_request(configuration, false, -1)" in body(
    "jetkvm_usb_service_apply_configuration"
)
assert "apply_configuration_request(NULL, true, -1)" in body(
    "jetkvm_usb_service_reenumerate"
)
assert "USB_REENUMERATION_DETACH_MS" in SOURCE
assert (
    "vTaskDelay(milliseconds_to_ticks_ceil(USB_REENUMERATION_DETACH_MS))"
    in body("apply_pending_configuration")
), "USB re-enumeration does not hold the physical detach interval"

# The owner first rolls hardware back whenever it changed, independently of
# whether persistence was reached. If candidate persistence was attempted, a
# failed commit/readback is repaired by saving the previous snapshot again.
apply_configuration = body("apply_pending_configuration")
candidate_save = call_position(
    apply_configuration, "jetkvm_usb_config_save", "&candidate"
)
publish_candidate = call_position(
    apply_configuration, "complete_configuration_success", "&candidate"
)
assert candidate_save < publish_candidate, (
    "candidate became visible before its persistent commit/readback succeeded"
)
success_completion = body("complete_configuration_success")
assert "s_ctx.configuration = *configuration" in success_completion
assert "s_ctx.configuration_cancel_revision == request_revision" in (
    success_completion
)

hardware_failure_blocks = matching_blocks(
    apply_configuration, "hardware_changed"
)
assert any(
    "restore_configuration(&previous" in block
    for block in hardware_failure_blocks
), "hardware changes are not guarded by previous-config rollback"

persistence_failure_blocks = matching_blocks(
    apply_configuration, "persistence_attempted"
)
assert any(
    re.search(
        r"jetkvm_usb_config_save\s*\(\s*&previous\s*,\s*&repaired_store",
        block,
    )
    for block in persistence_failure_blocks
), "failed candidate commit/readback does not repair the previous config"

rollback = body("restore_configuration")
rollback_disable = call_position(rollback, "disable_usb_stack")
rollback_descriptor = call_position(
    rollback, "select_descriptor_configuration", "configuration"
)
rollback_enable = call_position(rollback, "enable_usb_stack")
assert rollback_disable < rollback_descriptor < rollback_enable
assert "if (restore_enabled)" in rollback

# Every completion publishes a quiescent state before releasing the waiter.
# External callbacks run afterwards and therefore cannot delay completion.
for completion_name in (
    "complete_configuration_request",
    "complete_configuration_success",
):
    completion = body(completion_name)
    completion_pending_clear = completion.index(
        "s_ctx.state.configuration_pending = false"
    )
    completion_transition_clear = completion.index(
        "s_ctx.state.configuration_transitioning = false"
    )
    completion_revision = completion.index(
        "s_ctx.configuration_completed_revision = request_revision"
    )
    completion_signal = completion.index("xSemaphoreGive(s_ctx.configuration_done)")
    assert completion_revision < completion_signal
    assert completion_pending_clear < completion_signal
    assert completion_transition_clear < completion_signal
    assert completion_signal < completion.index("publish_state()")
assert SOURCE.count("xSemaphoreGive(s_ctx.configuration_done)") == 2

# Static RTOS objects are created into locals and become externally reachable
# only together with the gated owner and initialized state under one lock.
start = body("jetkvm_usb_service_start")
semaphore_publish = start.index(
    "s_ctx.initial_attempt_done = initial_attempt_done"
)
owner_publish = start.index("s_ctx.owner_task = owner_task")
initialized_publish = start.index("s_ctx.state.initialized = true")
owner_release = start.index("xTaskNotifyGive(owner_task)")
assert semaphore_publish < owner_publish < initialized_publish < owner_release
configuration_request = body("apply_configuration_request")
assert "configuration_call_mutex = s_ctx.configuration_call_mutex" in (
    configuration_request
)
assert "configuration_done = s_ctx.configuration_done" in configuration_request
assert "xSemaphoreTake(configuration_done, remaining)" in configuration_request

# Once the synchronous deadline expires, the caller marks its exact revision
# cancelled and keeps ownership until the owner acknowledges rollback. It may
# not return FAILED while that same candidate can still become committed.
assert "s_ctx.configuration_cancel_revision = request_revision" in configuration_request
assert "remaining = portMAX_DELAY" in configuration_request
assert "configuration_request_cancelled(request_revision)" in apply_configuration

combined = SOURCE + HEADER
for forbidden in (
    "TINYUSB_PORT_FULL_SPEED_0",
    "GPIO_NUM_24",
    "GPIO_NUM_25",
    "GPIO_NUM_26",
    "GPIO_NUM_27",
    "tinyusb_driver_uninstall",
    "USB_SERIAL_JTAG",
):
    assert forbidden not in combined, f"owner crossed protected boundary: {forbidden}"

print("jetkvm_usb_service owner static contract: PASS")
