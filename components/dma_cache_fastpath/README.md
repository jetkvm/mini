# DMA cache fast path

This component skips whole-frame cache maintenance on buffers that only DMA engines touch. It wraps `esp_cache_msync()` with GNU ld `--wrap`; the stock IDF function handles everything the wrapper does not skip. `jetkvm_video` registers each CSI capture buffer, so CSI and the H.264 encoder input share frames without cache operations.

`dma_cache_fastpath_register()` flushes and invalidates a cache-line-aligned buffer and marks it device-exclusive (at most eight, non-overlapping). While a buffer is device-exclusive, the wrapper skips only data-cache calls whose address and length match it exactly. Any other overlapping call switches the buffer back to stock behavior until it is rearmed. `dma_cache_fastpath_set_enabled(false)` turns skipping off everywhere; re-enabling requires rearming each buffer.

## Constraints

- The CPU must not access a device-exclusive buffer. Call `dma_cache_fastpath_cpu_acquire()` before CPU access and `dma_cache_fastpath_device_rearm()` before the next DMA.
- Register, rearm, and unregister only while no DMA is in flight.
- Do not register H.264 bitstream output buffers; the encoder's own cache operations on them must run.
- The CSI interrupt can run with the flash cache disabled and relies on capture buffers being registered.
