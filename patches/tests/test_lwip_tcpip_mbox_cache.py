#!/usr/bin/env python3
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HELPER = ROOT / "cmake" / "configure_lwip_tcpip_mbox.cmake"
PROJECT = ROOT / "CMakeLists.txt"
HELPER_TEXT = HELPER.read_text()
ALLOWED = (64, 128, 256, 512, 1024)


def configure(
    tcpip_value: int,
    tcp_value: int = 256,
    window_value: int | None = 1280 * 1024,
    board: str | None = None,
) -> tuple[
    subprocess.CompletedProcess[str], Path, str, tempfile.TemporaryDirectory[str]
]:
    temporary = tempfile.TemporaryDirectory()
    directory = Path(temporary.name)
    source = directory / "source"
    build = directory / "build"
    source.mkdir()
    (source / "sdkconfig").write_text(
        "CONFIG_IDF_TARGET=\"esp32p4\"\n"
        "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE=256\n"
        "CONFIG_LWIP_TCP_RECVMBOX_SIZE=256\n"
        "CONFIG_LWIP_TCP_WND_DEFAULT=1310720\n"
    )
    (source / "CMakeLists.txt").write_text(
        "cmake_minimum_required(VERSION 3.22)\n"
        "project(tcpip_mbox_contract NONE)\n"
        f'include("{HELPER}")\n'
        'file(WRITE "${CMAKE_BINARY_DIR}/selected.txt" "${SDKCONFIG}")\n'
    )
    arguments = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        f"-DJETKVM_LWIP_TCPIP_RECVMBOX_SIZE={tcpip_value}",
        f"-DJETKVM_LWIP_TCP_RECVMBOX_SIZE={tcp_value}",
    ]
    if window_value is not None:
        arguments.append(f"-DJETKVM_LWIP_TCP_WND={window_value}")
    if board is not None:
        arguments.append(f"-DJETKVM_BOARD_VARIANT={board}")
    result = subprocess.run(
        arguments,
        text=True,
        capture_output=True,
        check=False,
    )
    selected = (build / "selected.txt").read_text() if result.returncode == 0 else ""
    # Keep the temporary directory alive until the caller has inspected it.
    return result, build, selected, temporary


for tcpip_value in ALLOWED:
    result, build, selected, temporary = configure(tcpip_value)
    try:
        assert result.returncode == 0, result.stdout + result.stderr
        selected_path = Path(selected)
        assert selected_path.exists()
        configured = selected_path.read_text()
        assert (
            f"CONFIG_LWIP_TCPIP_RECVMBOX_SIZE={tcpip_value}" in configured
        )
        assert "CONFIG_LWIP_TCP_RECVMBOX_SIZE=256" in configured
        assert "CONFIG_LWIP_TCP_WND_DEFAULT=1310720" in configured
        if tcpip_value == 256:
            assert selected_path.name == "sdkconfig"
        else:
            assert selected_path.parent == build
            assert selected_path.name.endswith(
                f"g{tcpip_value}-t256-w1310720"
            )
    finally:
        temporary.cleanup()

# The two queues and receive window are independent knobs, but every
# substitution must land in one build-local sdkconfig so ESP-IDF cannot
# silently use one base value.
for tcpip_value, tcp_value, window_value in (
    (64, 256, 1280 * 1024),
    (256, 512, 1280 * 1024),
    (256, 1024, 2 * 1024 * 1024),
    (256, 256, 1024 * 1024),
):
    result, build, selected, temporary = configure(
        tcpip_value, tcp_value, window_value
    )
    try:
        assert result.returncode == 0, result.stdout + result.stderr
        selected_path = Path(selected)
        assert selected_path.parent == build
        assert selected_path.name.endswith(
            f"g{tcpip_value}-t{tcp_value}-w{window_value}"
        )
        configured = selected_path.read_text()
        assert (
            f"CONFIG_LWIP_TCPIP_RECVMBOX_SIZE={tcpip_value}" in configured
        )
        assert f"CONFIG_LWIP_TCP_RECVMBOX_SIZE={tcp_value}" in configured
        assert f"CONFIG_LWIP_TCP_WND_DEFAULT={window_value}" in configured
    finally:
        temporary.cleanup()

# Without an explicit window each board gets its own: the Wi-Fi board 64 KiB,
# the Ethernet board 1.25 MiB.
for board, window_value in (("wifi", 64 * 1024), ("ethernet", 1280 * 1024)):
    result, build, selected, temporary = configure(256, 256, None, board)
    try:
        assert result.returncode == 0, result.stdout + result.stderr
        configured = Path(selected).read_text()
        assert f"CONFIG_LWIP_TCP_WND_DEFAULT={window_value}" in configured
    finally:
        temporary.cleanup()

# A build directory configured before the board defaults has
# JETKVM_LWIP_TCP_WND_DEFAULT=1310720 in its cache; the Wi-Fi board still gets
# 64 KiB there.
result, build, selected, temporary = configure(256, 256, None, "wifi")
try:
    assert result.returncode == 0, result.stdout + result.stderr
    cache = build / "CMakeCache.txt"
    cache.write_text(cache.read_text() + "JETKVM_LWIP_TCP_WND_DEFAULT:STRING=1310720\n")
    again = subprocess.run(["cmake", str(build)], text=True, capture_output=True, check=False)
    assert again.returncode == 0, again.stdout + again.stderr
    configured = Path((build / "selected.txt").read_text()).read_text()
    assert "CONFIG_LWIP_TCP_WND_DEFAULT=65536" in configured
finally:
    temporary.cleanup()

result, _, _, temporary = configure(96)
try:
    assert result.returncode != 0
    assert "must be one of" in result.stdout + result.stderr
finally:
    temporary.cleanup()

result, _, _, temporary = configure(1024, 1024, 1536 * 1024)
try:
    assert result.returncode != 0
    assert "JETKVM_LWIP_TCP_WND must be one of" in (
        result.stdout + result.stderr
    )
finally:
    temporary.cleanup()

result, _, _, temporary = configure(1024, 768)
try:
    assert result.returncode != 0
    assert "JETKVM_LWIP_TCP_RECVMBOX_SIZE must be one of" in (
        result.stdout + result.stderr
    )
finally:
    temporary.cleanup()

project_text = PROJECT.read_text()
for production_default in (
    'set(JETKVM_LWIP_TCPIP_RECVMBOX_SIZE "256" CACHE STRING',
    'set(JETKVM_LWIP_TCP_RECVMBOX_SIZE "256" CACHE STRING',
    'set(JETKVM_LWIP_TCP_WND "" CACHE STRING',
    'set(_jetkvm_tcp_wnd_board_default "65536")',
    'set(_jetkvm_tcp_wnd_board_default "1310720")',
):
    assert production_default in HELPER_TEXT
assert project_text.index("configure_lwip_tcpip_mbox.cmake") < project_text.index(
    "tools/cmake/project.cmake"
)
assert "CONFIG_LWIP_TCPIP_RECVMBOX_SIZE" in project_text
assert "CONFIG_LWIP_TCP_RECVMBOX_SIZE" in project_text
assert "CONFIG_LWIP_TCP_WND_DEFAULT" in project_text

# sys_mbox_new() allocates size * sizeof(void *) queue storage in Internal RAM.
# These exact fixed costs exclude demand-allocated packet payloads.
pointer_bytes = 4
assert (256 - 64) * pointer_bytes == 768
assert 256 * pointer_bytes == 1024
assert 256 * pointer_bytes * 16 == 16 * 1024
assert 6 * 1024 * pointer_bytes == 24 * 1024  # six browser connections at the former geometry
assert 6 * 256 * pointer_bytes == 6 * 1024
assert 2 * 1024 * 1024 - 1024 * 1024 == 1024 * 1024
assert 2 * 1024 * 1024 - 1280 * 1024 == 768 * 1024

print("lwIP mailbox CMake cache contract: PASS")
