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

**CRITICAL:** Request packed format BEFORE `avcodec_open2()` - FFmpeg ignores it afterward.

In `AudioDecoder::open()`, after `avcodec_parameters_to_context()` but BEFORE `avcodec_open2()`:

```cpp
// Request packed output format from decoder (integer formats ONLY)
// IMPORTANT: Float formats (FLT, FLTP) are NOT eligible for bypass
// because downstream expects signed integer PCM. Float would corrupt audio.
AVSampleFormat preferredFormat = AV_SAMPLE_FMT_NONE;
AVSampleFormat srcFmt = m_codecContext->sample_fmt;

if (srcFmt == AV_SAMPLE_FMT_S16P)
    preferredFormat = AV_SAMPLE_FMT_S16;
else if (srcFmt == AV_SAMPLE_FMT_S32P)
    preferredFormat = AV_SAMPLE_FMT_S32;
// NOTE: Do NOT request FLT for FLTP - float bypass would corrupt audio

if (preferredFormat != AV_SAMPLE_FMT_NONE) {
    m_codecContext->request_sample_fmt = preferredFormat;
}

// NOW open the codec
if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
    // error handling...
}
```

In `AudioDecoder::initResampler()`:

```cpp
// Check if bypass is possible (integer formats only)
bool isIntegerFormat = (m_codecContext->sample_fmt == AV_SAMPLE_FMT_S16 ||
                        m_codecContext->sample_fmt == AV_SAMPLE_FMT_S32);
bool canBypass =
    isIntegerFormat &&
    (m_codecContext->sample_rate == outputRate) &&
    (m_codecContext->ch_layout.nb_channels == m_trackInfo.channels) &&
    formatMatchesOutput(m_codecContext->sample_fmt, outputBits);

if (canBypass) {
    m_bypassMode = true;
    // Skip SwrContext creation entirely
    // Still create m_pcmFifo for overflow handling
    DEBUG_LOG("[AudioDecoder] PCM BYPASS enabled - bit-perfect path");
    return true;
}
```

**IMPORTANT:** Float formats (FLT, FLTP from AAC/Vorbis/etc.) are NEVER bypassed. The downstream pipeline (DirettaSync, ring buffer) interprets 32-bit as signed integer. Bypassing float would produce corrupted audio. SwrContext handles float→S32 conversion.

**New members in AudioDecoder:**
- `bool m_bypassMode = false;`

### 2. Request Packed Output from Decoders

**Problem:** FFmpeg decoders often output planar formats (S16P, S32P, FLTP) requiring conversion.

**Solution:** Use `request_sample_fmt` to ask decoder for packed integer output BEFORE codec open. If decoder honors the request and produces S16/S32, bypass is enabled; if it produces float or planar, fall back to SwrContext.

**Float handling:** Codecs like AAC/Vorbis output FLTP. We do NOT request FLT because float bypass would corrupt audio. These codecs always go through SwrContext for float→integer conversion.

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
- `void setS24PackHint(S24PackMode hint) { m_s24MetadataHint = hint; }`

**API propagation path for metadata hint:**

The hint must flow from FFmpeg decoder metadata to the ring buffer. Data path:

```
AudioDecoder (has codecpar->bits_per_raw_sample)
    ↓ via TrackInfo or AudioCallback
DirettaRenderer (receives track info)
    ↓ via DirettaSync API
DirettaSync (owns ring buffer)
    ↓ via new setter
DirettaRingBuffer (uses hint)
```

**Implementation:**

1. In `AudioEngine.h`, extend `TrackInfo`:
```cpp
struct TrackInfo {
    // ... existing fields ...

    // 24-bit alignment hint from FFmpeg (for S24_P32 packing)
    enum class S24Alignment { Unknown, LsbAligned, MsbAligned };
    S24Alignment s24Alignment = S24Alignment::Unknown;
};
```

2. In `AudioDecoder::open()`, detect alignment from FFmpeg:
```cpp
// Detect 24-bit alignment from codec parameters
if (m_trackInfo.bitDepth == 24) {
    // FFmpeg's bits_per_coded_sample vs bits_per_raw_sample indicates alignment
    // If bits_per_coded_sample == 32 and bits_per_raw_sample == 24: LSB-aligned (common)
    // If bits_per_coded_sample == 24: tightly packed, assume LSB
    if (codecpar->bits_per_coded_sample == 32) {
        m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
    }
    // Some DACs/formats use MSB alignment - detect from codec ID if known
}
```

3. In `DirettaSync.h`, add setter:
```cpp
void setS24PackHint(S24PackMode hint) {
    m_ringBuffer.setS24PackHint(hint);
}
```

4. In `DirettaRenderer` (or audio callback), propagate on track change:
```cpp
void onTrackChange(const TrackInfo& info) {
    if (info.bitDepth == 24 && info.s24Alignment != TrackInfo::S24Alignment::Unknown) {
        S24PackMode hint = (info.s24Alignment == TrackInfo::S24Alignment::MsbAligned)
            ? S24PackMode::MsbAligned : S24PackMode::LsbAligned;
        m_direttaSync->setS24PackHint(hint);
    } else {
        m_direttaSync->setS24PackHint(S24PackMode::Unknown);  // Use detection
    }
}
```

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

1. `avformat_open_input()` - open container (existing)
2. `avformat_find_stream_info()` - find streams (existing)
3. `avcodec_find_decoder()` - find decoder (existing)
4. `avcodec_alloc_context3()` - allocate codec context (existing)
5. `avcodec_parameters_to_context()` - copy params (existing)
6. **Request packed integer format (NEW)** - set `request_sample_fmt` for S16P→S16, S32P→S32 (NOT for FLTP)
7. `avcodec_open2()` - open codec (existing, but AFTER step 6)
8. Store actual output format for bypass detection
9. Detect S24 alignment hint from `bits_per_coded_sample` (NEW)

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
| `src/AudioEngine.h` | Add `m_bypassMode`, `m_resamplerInitialized`, `m_actualDecoderFormat`, `BufferLevelCallback`, `getAdaptiveChunkSize()`, extend `TrackInfo` with `S24Alignment` enum |
| `src/AudioEngine.cpp` | Packed format request BEFORE `avcodec_open2()`, bypass detection in `initResampler()` (integer formats only), bypass path in `readSamples()`, dynamic FIFO sizing, adaptive chunk sizing, S24 alignment detection |
| `src/DirettaRingBuffer.h` | Add `S24PackMode::Deferred`, `m_s24MetadataHint`, `setS24PackHint()`, enhanced `detectS24PackMode()` with hybrid logic, deferred handling in `push24BitPacked()` |
| `src/DirettaSync.h` | Add `setS24PackHint()` to propagate hint to ring buffer |
| `src/DirettaRenderer.cpp` | Propagate S24 alignment hint on track change via `DirettaSync::setS24PackHint()` |

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| Decoder ignores packed request | `canBypass()` returns false, uses swr |
| Decoder outputs float (FLT/FLTP) | Bypass explicitly disabled, swr converts to S32 |
| AAC/Vorbis/MP3 streams | Always use swr (these output float), no bypass |
| FLAC/ALAC/WAV streams | May bypass if decoder outputs S16/S32 and rate matches |
| Mid-stream format change | Reset `m_resamplerInitialized`, re-evaluate bypass |
| Seek operation | FIFO reset (existing), bypass state preserved |
| 24-bit silence at start | Deferred detection, LSB default after 500ms timeout |
| Buffer callback not set | `getAdaptiveChunkSize()` returns `maxSamples` unchanged |
| S24 hint not propagated | Falls back to sample-based detection (existing behavior) |

## Risk Assessment

| Risk | Mitigation |
|------|------------|
| Bypass incorrectly enabled | Conservative `canBypass()` requires integer format (S16/S32), matching rate, and matching layout |
| Float bypass corruption | Float formats explicitly excluded from bypass check |
| request_sample_fmt ignored | Check actual format after `avcodec_open2()`, not requested format |
| FIFO too small at edge rates | Clamped to safe minimum 4096 samples |
| Deferred detection never resolves | Timeout with LSB default after 500ms |
| Adaptive sizing causes underrun | Deadband prevents over-correction, MIN_SCALE = 0.25 |
| S24 hint propagation breaks | Graceful fallback to sample detection if hint is Unknown |

## Testing

1. Compare byte-for-byte output with bypass enabled vs disabled for known PCM files (FLAC, WAV)
2. Verify no SwrContext allocation in logs for matching integer formats
3. Verify SwrContext IS created for float formats (AAC, Vorbis, MP3) - bypass must NOT activate
4. Test with silence-leading tracks for 24-bit detection
5. Monitor buffer levels during playback to verify adaptive sizing
6. Test S24 hint propagation by checking logs for "hint: LsbAligned" on 24-bit FLAC
7. Verify `request_sample_fmt` is set BEFORE `avcodec_open2()` in debug logs
