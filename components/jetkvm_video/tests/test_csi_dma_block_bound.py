#!/usr/bin/env python3
"""The CSI bridge DMA block limit never passes the buffer the DMA writes.

The bridge ends a DMA block at the frame end, or after dmablk_size bursts when
it misses the frame end. This test builds the real csi_dma_block_bursts() and
capture capacity macro on the host and checks, for every capture geometry, that
a block stays inside its buffer and that a normal frame still fits. It also
checks that both places that program the bridge use the function.
"""

from pathlib import Path
import re
import subprocess
import tempfile


COMPONENT = Path(__file__).resolve().parents[1]
source = (COMPONENT / "jetkvm_video.c").read_text()


def extract_function(text, signature):
    start = text.index(signature)
    opening = text.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"


def define_block(name):
    """The full #define of name, including continuation lines."""
    match = re.search(rf"#define {name}\b(?:.*\\\n)*.*\n", source)
    assert match, name
    return match.group(0)


# Both bridge setups (csi_init and csi_restart_rx) program the bounded value.
writes = re.findall(r"MIPI_CSI_BRIDGE\.dmablk_size\.dmablk_size = ([^;]+);", source)
assert len(writes) == 2, writes
assert all(value.strip() == "csi_dma_block_bursts(ctx)" for value in writes), writes

# Both capacity computations (video_mode_prepare and csi_init) round up.
assert "capture_capacity = CSI_CAPTURE_CAPACITY(frame_bytes);" in source
assert ("capture_buffer_capacity = CSI_CAPTURE_CAPACITY(ctx->frame_buffer_size);"
        in source)

# The driver's backup buffer, the other DMA destination, holds whole bursts.
driver = (COMPONENT.parent / "p4_csi_capture/p4_cam_ctlr_csi.c").read_text()
driver_header = (COMPONENT.parent / "p4_csi_capture/include/p4_csi_capture.h").read_text()
assert re.search(r"heap_caps_aligned_alloc\(alignment, backup_bytes,", driver)
assert "fb_size_in_bytes + P4_CSI_BACKUP_BUFFER_ALIGN_BYTES - 1U" in driver
backup_align = int(re.search(r"#define P4_CSI_BACKUP_BUFFER_ALIGN_BYTES (\d+)U",
                             driver_header).group(1))

helper = extract_function(source, "static uint32_t csi_dma_block_bursts(")
macros = "".join(define_block(name) for name in (
    "CSI_DMA_BURST_LEN_OVERRIDE", "CSI_DMA_BLOCK_BURSTS_MAX",
    "CSI_DMA_BURST_BYTES", "CSI_CAPTURE_CAPACITY"))

# Accepted EDID modes plus two host modes whose frames are not whole bursts.
modes = [(1920, 1080), (1280, 720), (1024, 768), (1280, 800), (1280, 1024),
         (1600, 900), (1366, 768)]
frames = sorted({width * height * half_bytes // 2
                 for width, height in modes
                 for half_bytes in (3, 4, 6)})  # YUV420, YUV422, RGB888
direct_capacity = 1920 * 1080 * 3 // 2
cases = "\n".join(f"    check({frame}U);" for frame in frames)

PROGRAM = f"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

{macros}
typedef struct {{
    bool use_driver_backup;
    size_t frame_buffer_size;
    size_t capture_buffer_capacity;
}} app_ctx_t;

{helper}

static size_t block_bytes(app_ctx_t ctx)
{{
    return (size_t)csi_dma_block_bursts(&ctx) * CSI_DMA_BURST_BYTES;
}}

static void check(size_t frame)
{{
    const size_t limit = (size_t)CSI_DMA_BLOCK_BURSTS_MAX * CSI_DMA_BURST_BYTES;
    /* Our capture buffers, sized for this frame. */
    app_ctx_t own = {{ .frame_buffer_size = frame,
                      .capture_buffer_capacity = CSI_CAPTURE_CAPACITY(frame) }};
    assert(block_bytes(own) <= own.capture_buffer_capacity);
    if (frame <= limit) {{
        assert(block_bytes(own) >= frame); /* a normal frame is not cut */
    }}
    /* Direct capture: buffers sized for the largest mode. */
    if (frame <= {direct_capacity}U) {{
        app_ctx_t direct = {{ .frame_buffer_size = frame,
                             .capture_buffer_capacity = {direct_capacity}U }};
        assert(block_bytes(direct) <= direct.capture_buffer_capacity);
        assert(block_bytes(direct) >= frame);
        assert(block_bytes(direct) - frame < CSI_DMA_BURST_BYTES);
    }}
    /* The driver's backup buffer: the frame rounded up to its alignment. It
     * is a destination in the debug backup mode and whenever the ring has no
     * free buffer outside direct capture. */
    const size_t backup_bytes = (frame + {backup_align}U - 1U) / {backup_align}U *
                                {backup_align}U;
    assert(block_bytes(own) <= backup_bytes);
    app_ctx_t backup = {{ .use_driver_backup = true, .frame_buffer_size = frame,
                         .capture_buffer_capacity = 1920U * 1080U * 3U }};
    assert(block_bytes(backup) <= backup_bytes);
    if (frame <= limit) {{
        assert(block_bytes(backup) >= frame);
    }}
}}

int main(void)
{{
{cases}
    puts("CSI DMA block bound within the capture buffer: PASS");
    return 0;
}}
"""

with tempfile.TemporaryDirectory() as tmp:
    c_file = Path(tmp) / "block_bound.c"
    binary = Path(tmp) / "block_bound"
    c_file.write_text(PROGRAM)
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
         str(c_file), "-o", str(binary)],
        check=True,
    )
    subprocess.run([str(binary)], check=True)
