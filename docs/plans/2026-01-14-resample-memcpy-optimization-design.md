# Resample Path Memory Copy Optimization Design

**Date:** 2026-01-14
**Goal:** Eliminate unnecessary memory copies in PCM resample path
**Benefits:** Reduced latency, lower CPU usage, less jitter

---

## 1. Architecture Overview

**Current bottlenecks:**

| Operation | Location | Cost |
|-----------|----------|------|
| `swr_convert()` → temp buffer | Always | 1 memcpy |
| temp buffer → outputPtr | Always | 1 memcpy |
| excess → m_remainingSamples | When excess exists | 1 memcpy |
| m_remainingSamples shift | Every consume | 1 memmove (O(n)) |

**Optimized flow:**

```
┌─────────────────────────────────────────────────────────────┐
│  readSamples() - Optimized Flow                             │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  1. Drain FIFO first (if has data)                          │
│     └─ av_audio_fifo_read() → outputPtr  [O(1) circular]    │
│                                                             │
│  2. For each decoded frame:                                 │
│     ├─ If samplesNeeded >= maxOutput:                       │
│     │   └─ swr_convert() → outputPtr     [DIRECT, no copy]  │
│     │                                                       │
│     └─ Else (need temp buffer):                             │
│         ├─ swr_convert() → m_resampleBuffer                 │
│         ├─ memcpy needed → outputPtr                        │
│         └─ av_audio_fifo_write(excess)   [O(1) circular]    │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

**Expected improvement:** Eliminates 1-2 memcpy operations per frame in the common case, and replaces O(n) memmove with O(1) FIFO operations.

---

## 2. Direct Write Path

**Condition:** When `samplesNeeded >= maxOutput`, write directly to `outputPtr`.

```cpp
// Calculate max possible output from this frame
int64_t maxOutput = swr_get_out_samples(m_swrContext, frameSamples);

size_t samplesNeeded = numSamples - totalSamplesRead;

if ((size_t)maxOutput <= samplesNeeded) {
    // DIRECT PATH: write straight to output buffer
    uint8_t* outPtrs[1] = { outputPtr };

    int convertedSamples = swr_convert(
        m_swrContext,
        outPtrs,
        maxOutput,
        (const uint8_t**)m_frame->data,
        frameSamples
    );

    if (convertedSamples > 0) {
        outputPtr += convertedSamples * bytesPerSample;
        totalSamplesRead += convertedSamples;
    }
} else {
    // TEMP BUFFER PATH: use m_resampleBuffer, excess goes to FIFO
}
```

**Why this works:**
- `swr_get_out_samples()` returns the maximum possible output
- Actual output is always ≤ this estimate
- Since `samplesNeeded >= maxOutput >= actualOutput`, we never overflow
- Packed PCM (S16/S32 interleaved) uses single pointer in `outPtrs[1]`

---

## 3. AVAudioFifo Integration

**Replace `m_remainingSamples` + `m_remainingCount` + memmove with AVAudioFifo.**

**Member changes in AudioDecoder:**

```cpp
class AudioDecoder {
private:
    // Remove:
    // std::vector<uint8_t> m_remainingSamples;
    // size_t m_remainingCount;

    // Add:
    AVAudioFifo* m_audioFifo = nullptr;
};
```

**Lifecycle management:**

| Event | Action |
|-------|--------|
| `initResampler()` | Allocate FIFO for output format (S16/S32, channels) |
| Format/channel change | Free old FIFO, allocate new one |
| `seek()` | `av_audio_fifo_reset(m_audioFifo)` |
| `close()` | `av_audio_fifo_free(m_audioFifo)` |

**FIFO allocation (in initResampler):**

```cpp
// Free existing FIFO if format changed
if (m_audioFifo) {
    av_audio_fifo_free(m_audioFifo);
    m_audioFifo = nullptr;
}

// Allocate for output format
AVSampleFormat fifoFormat = (outputBits == 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
m_audioFifo = av_audio_fifo_alloc(fifoFormat, m_trackInfo.channels, 8192);
if (!m_audioFifo) {
    std::cerr << "[AudioDecoder] Failed to allocate audio FIFO" << std::endl;
    return false;
}
```

**Reading from FIFO (top of readSamples, before decode loop):**

```cpp
// Drain FIFO first
int fifoSamples = av_audio_fifo_size(m_audioFifo);
if (fifoSamples > 0) {
    int samplesToRead = std::min(fifoSamples, (int)(numSamples - totalSamplesRead));
    uint8_t* outPtrs[1] = { outputPtr };

    int read = av_audio_fifo_read(m_audioFifo, (void**)outPtrs, samplesToRead);
    if (read > 0) {
        outputPtr += read * bytesPerSample;
        totalSamplesRead += read;
    }

    if (totalSamplesRead >= numSamples) {
        return totalSamplesRead;
    }
}
```

**Writing excess to FIFO (temp buffer path):**

```cpp
if ((size_t)convertedSamples > samplesToUse) {
    size_t excess = convertedSamples - samplesToUse;
    uint8_t* excessPtr = m_resampleBuffer.data() + samplesToUse * bytesPerSample;
    uint8_t* excessPtrs[1] = { excessPtr };

    av_audio_fifo_write(m_audioFifo, (void**)excessPtrs, excess);
}
```

---

## 4. Implementation Plan

**Files to modify:**

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `AVAudioFifo* m_audioFifo`; remove `m_remainingSamples`, `m_remainingCount` |
| `src/AudioEngine.cpp` | Refactor `readSamples()`, update `initResampler()`, `seek()`, `close()` |

**Implementation order:**

| Step | Task | Risk |
|------|------|------|
| 1 | Add `#include <libavutil/audio_fifo.h>` | None |
| 2 | Replace member variables in header | Low |
| 3 | Add FIFO alloc/free in `initResampler()` and `close()` | Low |
| 4 | Add FIFO reset in `seek()` | Low |
| 5 | Refactor `readSamples()`: drain FIFO at top | Medium |
| 6 | Add direct write path (samplesNeeded >= maxOutput) | Medium |
| 7 | Replace m_remainingSamples write with FIFO write | Low |
| 8 | Remove old m_remainingSamples/memmove code | Low |
| 9 | Test PCM playback (various sample rates/bit depths) | Validation |

**DSD path:** No changes needed. The DSD native mode (`m_rawDSD`) has its own buffer handling and doesn't use the resampler.

---

## 5. Testing Strategy

**Functional tests:**

| Test | Purpose |
|------|---------|
| Play 44.1kHz/16-bit FLAC | Common case, verify basic playback |
| Play 96kHz/24-bit FLAC | Higher rate, exercises resampler |
| Play 192kHz/32-bit WAV | Uncompressed, tests direct path |
| Seek mid-track | Verify FIFO reset works |
| Gapless transition (same format) | Verify FIFO drains correctly between tracks |
| Format change between tracks | Verify FIFO recreated properly |

**Edge cases to verify:**

| Case | Expected behavior |
|------|-------------------|
| FIFO empty at start | Skip FIFO read, go straight to decode |
| Direct path fits exactly | No excess, FIFO stays empty |
| Small samplesNeeded (< maxOutput) | Uses temp buffer path, excess to FIFO |
| Seek while FIFO has data | FIFO cleared, no stale audio |

**Performance validation (optional):**
- Before/after comparison of CPU usage during playback
- Verify no new allocations in steady-state decode loop

---

## Scope

**In scope:**
- Direct write optimization for resample path
- AVAudioFifo to replace memmove-based leftover handling
- FIFO lifecycle management (init, seek, close)

**Out of scope:**
- DSD path changes (uses separate buffer logic)
- No-resampling path (already efficient)
- Runtime configuration
