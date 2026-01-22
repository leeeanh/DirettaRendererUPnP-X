# Latest Design Plan Summary

This summary covers the two newest design plans in `docs/plans`.

## 2026-01-15 - PCM Bypass and Bit-Perfect Optimization

**Problems fixed:**
- SwrContext created even when no conversion is needed, wasting CPU and adding latency.
- Decoder outputs planar formats that force extra conversion.
- 24-bit pack mode mis-detection during silence or fade-ins.
- Fixed chunk sizes cause bursty writes and jitter.
- Fixed FIFO sizing risks overflow at high rates and wastes memory at low rates.

**Advantages:**
- Bit-perfect PCM bypass for matching integer formats (S16/S32) with lower CPU overhead.
- Packed format request before `avcodec_open2()` avoids unnecessary planar conversion.
- More robust 24-bit alignment handling with sample-first detection and safe hints.
- Adaptive chunk sizing keeps buffer levels stable to reduce jitter.
- FIFO sizing scales with sample rate to avoid underruns/overruns.

## 2026-01-14 - Resample Path Memory Copy Optimization

**Problems fixed:**
- Extra memcpy/memmove steps in the resample path add latency and CPU cost.
- Overflow handling uses O(n) memmove and mixes PCM/DSD remainder state.
- Temp buffer usage always forces at least one extra copy per frame.

**Advantages:**
- Direct write path eliminates one or two copies in the common case.
- AVAudioFifo replaces O(n) memmove with O(1) circular buffer reads/writes.
- Clear separation of PCM FIFO and DSD remainder buffer improves correctness.
- Lower CPU usage and reduced jitter under load.
