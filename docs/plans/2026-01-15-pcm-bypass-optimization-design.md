# PCM Bypass and Bit-Perfect Optimization Design

**Date:** 2026-01-15
**Status:** Approved
**Goal:** Achieve bit-perfect PCM playback with reduced CPU overhead

## Overview

This design implements five optimizations to eliminate unnecessary audio processing when the source format matches the output format, and to improve robustness and efficiency in the PCM path.

## Optimizations

### 1. True PCM Bypass

**Problem:** Current code always creates `SwrContext` even when no conversion is needed.

**Solution:** Detect when formats match exactly and skip SwrContext creation entirely.

In `AudioDecoder::open()`, after opening codec:

```cpp
// Request packed output format from decoder
AVSampleFormat preferredFormat = AV_SAMPLE_FMT_NONE;
if (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S16P)
    preferredFormat = AV_SAMPLE_FMT_S16;
else if (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S32P)
    preferredFormat = AV_SAMPLE_FMT_S32;
else if (m_codecContext->sample_fmt == AV_SAMPLE_FMT_FLTP)
    preferredFormat = AV_SAMPLE_FMT_FLT;

if (preferredFormat != AV_SAMPLE_FMT_NONE) {
    m_codecContext->request_sample_fmt = preferredFormat;
}
```

In `AudioDecoder::initResampler()`:

```cpp
bool canBypass =
    (m_codecContext->sample_rate == outputRate) &&
    (m_codecContext->ch_layout.nb_channels == m_trackInfo.channels) &&
    formatMatchesOutput(m_codecContext->sample_fmt, outputBits);

if (canBypass) {
    m_bypassMode = true;
    // Skip SwrContext creation entirely
    // Still create m_pcmFifo for overflow handling
    return true;
}
```

**New members in AudioDecoder:**
- `bool m_bypassMode = false;`

### 2. Request Packed Output from Decoders

**Problem:** FFmpeg decoders often output planar formats (S16P, S32P, FLTP) requiring conversion.

**Solution:** Use `request_sample_fmt` to ask decoder for packed output. If decoder honors the request, bypass is enabled; if not, fall back to SwrContext.

This is integrated with optimization #1 above.

### 3. Hardened 24-bit Pack Detection

**Problem:** Current `detectS24PackMode()` checks first 32 samples for zero bytes. Silence or fade-ins cause mis-detection.

**Solution:** Hybrid approach with FFmpeg metadata hint and deferred detection.

```cpp
enum class S24PackMode { Unknown, LsbAligned, MsbAligned, Deferred };

S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) {
    // Phase 1: Check FFmpeg metadata hint
    if (m_s24MetadataHint != S24PackMode::Unknown) {
        return m_s24MetadataHint;
    }

    // Phase 2: Scan for non-zero samples
    size_t checkSamples = std::min<size_t>(numSamples, 64);
    bool hasNonZeroLsb = false;
    bool hasNonZeroMsb = false;
    bool allZero = true;

    for (size_t i = 0; i < checkSamples; i++) {
        uint8_t b0 = data[i * 4 + 0];  // LSB position
        uint8_t b3 = data[i * 4 + 3];  // MSB position

        if (b0 != 0x00) hasNonZeroLsb = true;
        if (b3 != 0x00) hasNonZeroMsb = true;
        if (b0 != 0x00 || data[i*4+1] != 0x00 || data[i*4+2] != 0x00)
            allZero = false;
    }

    // Phase 3: Decision
    if (allZero) return S24PackMode::Deferred;  // Wait for real audio
    if (hasNonZeroLsb) return S24PackMode::LsbAligned;
    return S24PackMode::MsbAligned;
}
```

**Deferred handling:** Buffer data and re-check on next call. Use LSB-aligned as safe default after 500ms timeout.

**New members in DirettaRingBuffer:**
- `S24PackMode m_s24MetadataHint = S24PackMode::Unknown;`

### 4. Adaptive PCM Chunk Sizing

**Problem:** Fixed chunk sizes cause bursty writes and jitter when decoder produces variable-sized frames.

**Solution:** Use `getBufferLevel()` to keep ring buffer at ~50% full.

```cpp
size_t AudioEngine::getAdaptiveChunkSize(size_t maxSamples) const {
    float level = m_bufferLevelCallback ? m_bufferLevelCallback() : 0.5f;

    constexpr float TARGET_LEVEL = 0.50f;
    constexpr float DEADBAND = 0.10f;
    constexpr float MIN_SCALE = 0.25f;
    constexpr float MAX_SCALE = 1.50f;

    float scale = 1.0f;
    float deviation = level - TARGET_LEVEL;

    if (deviation > DEADBAND) {
        // Buffer too full - reduce chunk size
        scale = 1.0f - ((deviation - DEADBAND) / (1.0f - TARGET_LEVEL - DEADBAND));
        scale = std::max(scale, MIN_SCALE);
    } else if (deviation < -DEADBAND) {
        // Buffer too empty - increase chunk size
        scale = 1.0f + ((-deviation - DEADBAND) / (TARGET_LEVEL - DEADBAND)) * 0.5f;
        scale = std::min(scale, MAX_SCALE);
    }

    return static_cast<size_t>(maxSamples * scale);
}
```

**New members in AudioEngine:**
- `using BufferLevelCallback = std::function<float()>;`
- `void setBufferLevelCallback(const BufferLevelCallback& cb);`

### 5. Dynamic FIFO Sizing

**Problem:** Fixed 8192-sample FIFO. High-rate streams can overflow; low-rate streams waste memory.

**Solution:** Scale FIFO size based on output rate.

```cpp
constexpr int BASE_RATE = 44100;
constexpr int BASE_FRAME_SIZE = 4096;
constexpr int MIN_FIFO_SAMPLES = 4096;
constexpr int MAX_FIFO_SAMPLES = 32768;

int fifoSamples = (2 * outputRate * BASE_FRAME_SIZE) / BASE_RATE;
fifoSamples = std::clamp(fifoSamples, MIN_FIFO_SAMPLES, MAX_FIFO_SAMPLES);

m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSamples);
```

**Resulting sizes:**

| Output Rate | FIFO Samples | Duration |
|-------------|--------------|----------|
| 44.1 kHz    | 8192         | ~186ms   |
| 96 kHz      | 17825        | ~186ms   |
| 192 kHz     | 32768 (cap)  | ~170ms   |
| 384 kHz     | 32768 (cap)  | ~85ms    |

**Bypass mode:** Smaller FIFO since no resampling expansion:
```cpp
if (m_bypassMode) {
    fifoSamples = std::clamp(BASE_FRAME_SIZE * 2, MIN_FIFO_SAMPLES, 8192);
}
```

## Integration

**Initialization flow in `AudioDecoder::open()`:**

1. Open codec (existing)
2. Request packed format (NEW)
3. Open codec with `avcodec_open2()` (existing)
4. Store actual output format for bypass detection

**First call to `readSamples()` - lazy resampler init:**

```cpp
if (!m_resamplerInitialized && !m_trackInfo.isDSD) {
    m_bypassMode = canBypass(outputRate, outputBits);
    if (!m_bypassMode) {
        initResampler(outputRate, outputBits);
    }
    initFifo(outputRate, m_bypassMode);
    m_resamplerInitialized = true;
}
```

**Bypass path in decode loop:**

```cpp
if (m_bypassMode) {
    size_t frameSamples = m_frame->nb_samples;
    size_t bytesToCopy = frameSamples * m_bytesPerSample * m_trackInfo.channels;
    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
    // FIFO overflow handling same as before
}
```

## Files Modified

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `m_bypassMode`, `m_resamplerInitialized`, `m_actualDecoderFormat`, `BufferLevelCallback`, `getAdaptiveChunkSize()` |
| `src/AudioEngine.cpp` | Packed format request in `open()`, bypass detection in `initResampler()`, bypass path in `readSamples()`, dynamic FIFO sizing, adaptive chunk sizing |
| `src/DirettaRingBuffer.h` | Add `S24PackMode::Deferred`, `m_s24MetadataHint`, enhanced `detectS24PackMode()` with hybrid logic, deferred handling in `push24BitPacked()` |
| `src/DirettaSync.h` | No changes (already has `getBufferLevel()`) |

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Decoder ignores packed request | `canBypass()` returns false, uses swr |
| Mid-stream format change | Reset `m_resamplerInitialized`, re-evaluate bypass |
| Seek operation | FIFO reset (existing), bypass state preserved |
| 24-bit silence at start | Deferred detection, LSB default after 500ms timeout |
| Buffer callback not set | `getAdaptiveChunkSize()` returns `maxSamples` unchanged |

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Bypass incorrectly enabled | Conservative `canBypass()` checks rate, format, and layout |
| FIFO too small at edge rates | Clamped to safe minimum 4096 samples |
| Deferred detection never resolves | Timeout with LSB default after 500ms |
| Adaptive sizing causes underrun | Deadband prevents over-correction, MIN_SCALE = 0.25 |

## Testing

1. Compare byte-for-byte output with bypass enabled vs disabled for known PCM files
2. Verify no SwrContext allocation in logs for matching formats
3. Test with silence-leading tracks for 24-bit detection
4. Monitor buffer levels during playback to verify adaptive sizing
