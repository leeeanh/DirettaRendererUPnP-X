# FLAC Bypass and Adaptive Network FIFO Design

**Date:** 2026-01-16
**Status:** Draft
**Goal:** Enable PCM bypass for FLAC streams and implement adaptive buffering for network sources

## Overview

Two optimizations for Qobuz/WiFi streaming:

### Optimization A: FLAC Packed Format Request

**Problem:** Current `request_sample_fmt` logic checks `sample_fmt` before `avcodec_open2()`, but FLAC decoders don't report format until after opening. Result: bypass never activates, SwrContext created unnecessarily.

**Solution:** Request packed format based on `codecpar->bits_per_raw_sample` (container metadata, available pre-open) instead of codec context format.

**Expected result:** FLAC streams use bypass path, eliminating swr overhead.

### Optimization B: Adaptive Network FIFO

**Problem:** Fixed FIFO sizing (~170-186ms) is insufficient for WiFi jitter. Network streams underrun while local files don't need extra latency.

**Solution:**
- Detect network sources via URL prefix (`http://`, `https://`)
- Start network streams at 250ms FIFO
- Track underruns in DirettaSync
- Increase to 500ms if underruns occur (applies to next track)
- Local files keep current sizing

**Expected result:** Resilient Qobuz playback over WiFi without penalizing local file latency.

## Implementation

### 1. FLAC Packed Format Request

**File: `src/AudioEngine.cpp`**

Replace the current `request_sample_fmt` logic (lines 240-266) with bit-depth based detection:

```cpp
// Request packed output format BEFORE avcodec_open2()
// For FLAC/ALAC: use bits_per_raw_sample (known from container)
// For PCM: use existing planar check as fallback
AVSampleFormat preferredFormat = AV_SAMPLE_FMT_NONE;

int rawBits = codecpar->bits_per_raw_sample;
if (rawBits == 0) rawBits = codecpar->bits_per_coded_sample;

if (rawBits > 0) {
    // Bit-depth based request (works for FLAC/ALAC)
    preferredFormat = (rawBits <= 16) ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32;
    DEBUG_LOG("[AudioDecoder] Requesting packed format based on " << rawBits << "-bit source");
} else {
    // Fallback: planar check for other codecs
    AVSampleFormat srcFmt = m_codecContext->sample_fmt;
    if (srcFmt == AV_SAMPLE_FMT_S16P)
        preferredFormat = AV_SAMPLE_FMT_S16;
    else if (srcFmt == AV_SAMPLE_FMT_S32P)
        preferredFormat = AV_SAMPLE_FMT_S32;
}

// Capability check (unchanged from current code)
if (preferredFormat != AV_SAMPLE_FMT_NONE && codec->sample_fmts != nullptr) {
    bool packedSupported = false;
    for (const AVSampleFormat* fmt = codec->sample_fmts; *fmt != AV_SAMPLE_FMT_NONE; fmt++) {
        if (*fmt == preferredFormat) {
            packedSupported = true;
            break;
        }
    }
    if (packedSupported) {
        m_codecContext->request_sample_fmt = preferredFormat;
        DEBUG_LOG("[AudioDecoder] Requesting packed format: " << av_get_sample_fmt_name(preferredFormat));
    } else {
        DEBUG_LOG("[AudioDecoder] Packed format not supported by decoder, will use swr");
    }
}
```

**Why this works:**
- `bits_per_raw_sample` is populated from FLAC stream headers before decode
- Fallback to `bits_per_coded_sample` covers edge cases
- Existing capability check ensures decoder supports the format

### 2. Network Source Detection

**File: `src/AudioEngine.h`**

Add `isNetworkSource` to TrackInfo:

```cpp
struct TrackInfo {
    // ... existing fields ...
    bool isCompressed;      // existing
    bool isNetworkSource;   // NEW: true if http:// or https://

    TrackInfo() : sampleRate(0), bitDepth(0), channels(2), duration(0),
                  isDSD(false), dsdRate(0), isCompressed(true),
                  isNetworkSource(false),  // NEW
                  dsdSourceFormat(DSDSourceFormat::Unknown),
                  s24Alignment(S24Alignment::Unknown) {}
};
```

**File: `src/AudioEngine.cpp`**

In `AudioDecoder::open()`, after successful `avformat_open_input` (~line 136):

```cpp
// Detect network source for adaptive buffering
if (url.find("http://") == 0 || url.find("https://") == 0) {
    m_trackInfo.isNetworkSource = true;
    DEBUG_LOG("[AudioDecoder] Network source detected - adaptive buffering enabled");
} else {
    m_trackInfo.isNetworkSource = false;
}
```

### 3. Underrun Tracking in DirettaSync

**File: `src/DirettaSync.h`**

Add underrun counter and accessor:

```cpp
class DirettaSync : public DIRETTA::Sync {
public:
    // ... existing public methods ...

    // Underrun tracking for adaptive buffering
    uint32_t getUnderrunCount() const {
        return m_underrunCount.load(std::memory_order_acquire);
    }
    void resetUnderrunCount() {
        m_underrunCount.store(0, std::memory_order_release);
    }

private:
    // ... existing members ...
    std::atomic<uint32_t> m_underrunCount{0};
};
```

**File: `src/DirettaSync.cpp`**

In `getNewStream()`, update existing underrun detection (~line 1076):

```cpp
// Underrun
if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
    m_underrunCount.fetch_add(1, std::memory_order_relaxed);  // NEW
    std::cerr << "[DirettaSync] UNDERRUN #" << count
              << " avail=" << avail << " need=" << currentBytesPerBuffer << std::endl;
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
    m_workerActive = false;
    return true;
}
```

In `fullReset()` (~line 607), reset the counter:

```cpp
void DirettaSync::fullReset() {
    // ... existing reset logic ...
    m_underrunCount.store(0, std::memory_order_release);  // NEW
}
```

### 4. Adaptive FIFO Sizing

**File: `src/AudioEngine.h`**

Add a public setter/getter and a private atomic flag for elevated buffer mode:

```cpp
class AudioDecoder {
public:
    static void setElevatedBufferMode(bool enabled);
    static bool elevatedBufferMode();

private:
    // Adaptive FIFO: elevated after underrun, persists for session
    static std::atomic<bool> s_elevatedBufferMode;
};

class AudioEngine {
public:
    void setElevatedBufferMode(bool enabled);
};
```

**File: `src/AudioEngine.cpp`**

Initialize and expose the flag:

```cpp
std::atomic<bool> AudioDecoder::s_elevatedBufferMode{false};

void AudioDecoder::setElevatedBufferMode(bool enabled) {
    s_elevatedBufferMode.store(enabled, std::memory_order_release);
}

bool AudioDecoder::elevatedBufferMode() {
    return s_elevatedBufferMode.load(std::memory_order_acquire);
}

void AudioEngine::setElevatedBufferMode(bool enabled) {
    AudioDecoder::setElevatedBufferMode(enabled);
}
```

Replace FIFO sizing in `initResampler()` (~lines 1101-1104 and 1173-1180):

```cpp
// Calculate FIFO size based on source type and underrun history
size_t fifoSize;

if (m_trackInfo.isNetworkSource) {
    // Network source: 250ms base, 500ms if elevated
    bool elevated = elevatedBufferMode();
    float bufferMs = elevated ? 500.0f : 250.0f;
    fifoSize = static_cast<size_t>((outputRate * bufferMs) / 1000.0f);

    // Cap at reasonable maximum
    fifoSize = std::min(fifoSize, static_cast<size_t>(65536));

    DEBUG_LOG("[AudioDecoder] Network FIFO: " << fifoSize << " samples ("
              << bufferMs << "ms" << (elevated ? " elevated" : "") << ")");
} else {
    // Local source: existing rate-based sizing
    fifoSize = 8192;
    if (outputRate > 96000) fifoSize = 16384;
    if (outputRate > 192000) fifoSize = 32768;
}
```

### 5. FIFO Prefill for Network Sources

**Problem:** Simply enlarging `m_pcmFifo` doesn't provide buffering protection. The current `readSamples()` decode loop stops as soon as `numSamples` are satisfied, so the FIFO only ever holds frame overflow (typically <1 frame). Network stalls still propagate directly into underruns.

**Solution:** Add explicit prefill after track open for network sources. Decode ahead until FIFO reaches target depth before returning samples to the audio thread.

**File: `src/AudioEngine.h`**

```cpp
class AudioDecoder {
public:
    // Prefill FIFO to target level (call after open for network sources)
    bool prefillFifo(uint32_t outputRate, uint32_t outputBits);

    // Check if input is exhausted (for gapless preload triggering)
    // Use this instead of isEOF() when deciding to preload next track
    bool isInputExhausted() const { return m_inputExhausted; }

private:
    size_t m_fifoTargetSamples = 0;   // Target fill level (0 = no prefill)
    bool m_inputExhausted = false;    // Input EOF but FIFO may still have data

    // Pending frame storage for overflow (when FIFO can't fit entire decoded frame)
    AudioBuffer m_pendingFrameBuffer;
    size_t m_pendingFrameSamples = 0;
    size_t m_pendingFrameOffset = 0;  // Consumed samples from pending frame

    // CRITICAL: Clear pending buffer state in seek/reset (see below)
};
```

**CRITICAL - Reset pending buffer on seek/reset:**

If a seek occurs after overflow samples are staged in the pending buffer, `readSamples()` will drain stale pre-seek samples before decoding from the new position, causing audible glitches. The pending buffer state must be cleared alongside FIFO reset.

```cpp
// In AudioDecoder::seek() and any reset path (e.g., close(), resetState()):
void AudioDecoder::clearPendingBuffer() {
    m_pendingFrameSamples = 0;
    m_pendingFrameOffset = 0;
    // m_pendingFrameBuffer can be left allocated for reuse
}

// In seek():
bool AudioDecoder::seek(double positionSeconds) {
    // ... existing seek logic (av_seek_frame, avcodec_flush_buffers) ...

    // Clear all buffered audio state
    if (m_pcmFifo) {
        av_audio_fifo_reset(m_pcmFifo);
    }
    clearPendingBuffer();          // NEW: Clear overflow buffer
    m_inputExhausted = false;      // NEW: Reset EOF state for continued reading
    m_eof = false;

    // ... trigger refill if network source ...
    return true;
}
```

**File: `src/AudioEngine.cpp`**

```cpp
bool AudioDecoder::prefillFifo(uint32_t outputRate, uint32_t outputBits) {
    if (!m_trackInfo.isNetworkSource) {
        return true;  // No prefill needed for local sources
    }

    // Initialize resampler if not already done (this allocates m_pcmFifo)
    if (!m_resamplerInitialized && !initResampler(outputRate, outputBits)) {
        return false;
    }

    // FIFO check AFTER initResampler - it allocates the FIFO
    if (!m_pcmFifo) {
        std::cerr << "[AudioDecoder] FIFO not allocated after initResampler" << std::endl;
        return false;
    }

    // Allocate packet/frame if not already done (same guard as readSamples)
    // These are lazily allocated, so prefill after open() would crash without this
    if (!m_packet) {
        m_packet = av_packet_alloc();
        if (!m_packet) {
            std::cerr << "[AudioDecoder] Failed to allocate packet for prefill" << std::endl;
            return false;
        }
    }
    if (!m_frame) {
        m_frame = av_frame_alloc();
        if (!m_frame) {
            std::cerr << "[AudioDecoder] Failed to allocate frame for prefill" << std::endl;
            return false;
        }
    }

    // Target: fill FIFO to 80% of capacity (leave room for frame overflow)
    size_t fifoCapacity = av_audio_fifo_space(m_pcmFifo) + av_audio_fifo_size(m_pcmFifo);
    m_fifoTargetSamples = (fifoCapacity * 80) / 100;

    size_t bytesPerSample = (outputBits == 16) ? 2 : 4;
    bytesPerSample *= m_trackInfo.channels;

    DEBUG_LOG("[AudioDecoder] Prefilling network FIFO to " << m_fifoTargetSamples
              << " samples (" << (m_fifoTargetSamples * 1000 / outputRate) << "ms)");

    // Decode until FIFO reaches target (use m_inputExhausted, NOT m_eof)
    // m_eof should only be true when input exhausted AND FIFO drained
    while (av_audio_fifo_size(m_pcmFifo) < (int)m_fifoTargetSamples && !m_inputExhausted) {
        int ret = av_read_frame(m_formatContext, m_packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                // Input exhausted, but FIFO still has data to play
                // Do NOT set m_eof here - let readSamples() drain FIFO first
                m_inputExhausted = true;
                DEBUG_LOG("[AudioDecoder] Prefill: input exhausted, FIFO has "
                          << av_audio_fifo_size(m_pcmFifo) << " samples to drain");
            }
            break;
        }

        if (m_packet->stream_index != m_audioStreamIndex) {
            av_packet_unref(m_packet);
            continue;
        }

        ret = avcodec_send_packet(m_codecContext, m_packet);
        av_packet_unref(m_packet);
        if (ret < 0) break;

        while (ret >= 0) {
            ret = avcodec_receive_frame(m_codecContext, m_frame);
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) break;

            // Process frame (bypass or resample)
            uint8_t* frameData;
            size_t frameSamples = m_frame->nb_samples;

            if (m_bypassMode) {
                frameData = m_frame->data[0];
            } else if (m_swrContext) {
                // Resample to staging buffer
                size_t maxOut = frameSamples + 256;
                if (m_resampleBuffer.size() < maxOut * bytesPerSample) {
                    m_resampleBuffer.resize(maxOut * bytesPerSample);
                }
                uint8_t* outPtrs[1] = { m_resampleBuffer.data() };
                int converted = swr_convert(m_swrContext, outPtrs, maxOut,
                    (const uint8_t**)m_frame->data, frameSamples);
                if (converted <= 0) {
                    av_frame_unref(m_frame);  // Unref before continue
                    continue;
                }
                frameData = m_resampleBuffer.data();
                frameSamples = converted;
            } else {
                av_frame_unref(m_frame);  // Unref before continue
                continue;
            }

            // Check available space - write what fits, store remainder
            int availableSpace = av_audio_fifo_space(m_pcmFifo);
            size_t samplesToWrite = std::min((size_t)availableSpace, frameSamples);

            if (samplesToWrite > 0) {
                uint8_t* writePtrs[1] = { frameData };
                int written = av_audio_fifo_write(m_pcmFifo, (void**)writePtrs, samplesToWrite);
                if (written < (int)samplesToWrite) {
                    std::cerr << "[AudioDecoder] Prefill FIFO write error" << std::endl;
                }
            }

            // Store overflow in pending frame buffer (don't drop samples!)
            if (frameSamples > samplesToWrite) {
                size_t overflow = frameSamples - samplesToWrite;
                size_t overflowBytes = overflow * bytesPerSample;
                size_t offsetBytes = samplesToWrite * bytesPerSample;

                if (m_pendingFrameBuffer.size() < overflowBytes) {
                    m_pendingFrameBuffer.resize(overflowBytes);
                }
                memcpy(m_pendingFrameBuffer.data(), frameData + offsetBytes, overflowBytes);
                m_pendingFrameSamples = overflow;
                m_pendingFrameOffset = 0;

                DEBUG_LOG("[AudioDecoder] Prefill: stored " << overflow
                          << " overflow samples in pending buffer");
                av_frame_unref(m_frame);  // Unref before goto
                goto prefill_done;  // FIFO full, stop prefill
            }

            // Release frame reference after processing (prevents memory growth)
            av_frame_unref(m_frame);
        }
    }
prefill_done:

    int prefilled = av_audio_fifo_size(m_pcmFifo);
    DEBUG_LOG("[AudioDecoder] Prefill complete: " << prefilled << "/" << m_fifoTargetSamples
              << " samples (" << (prefilled * 1000 / outputRate) << "ms)"
              << (m_inputExhausted ? " [input exhausted]" : "")
              << (m_pendingFrameSamples > 0 ? " [has pending]" : ""));

    return prefilled > 0 || m_pendingFrameSamples > 0;
}
```

**Important - Two-phase EOF for gapless preload:**

The `m_inputExhausted` flag indicates no more data from the network, but FIFO/pending buffer may still have samples. The existing `m_eof` flag should only be set when BOTH input is exhausted AND all buffered samples are consumed.

**Why two flags?** `AudioEngine::process()` uses `isEOF()` to trigger gapless preloading. If we defer `m_eof` until buffers drain, preloading won't start until the last 250-500ms of audio is playing - not enough time for network sources. By exposing `isInputExhausted()`, the engine can trigger preload early while `isEOF()` still correctly reports when playback should end.

Update `readSamples()` EOF handling:

**CRITICAL - Modify existing decode loop:** The current `readSamples()` decode loop likely sets `m_eof = true` directly when `av_read_frame()` returns `AVERROR_EOF`. This must be changed to set `m_inputExhausted` instead:

```cpp
// In readSamples() decode loop - CHANGE existing EOF handling:
int ret = av_read_frame(m_formatContext, m_packet);
if (ret < 0) {
    if (ret == AVERROR_EOF) {
        // OLD (wrong): m_eof = true;
        // NEW: Mark input exhausted, let FIFO/pending drain before true EOF
        m_inputExhausted = true;
    }
    break;
}
```

**Set true EOF only when all buffers empty** (add at end of `readSamples()`):

```cpp
// At end of readSamples(), after FIFO drain and decode loop:
if (m_inputExhausted && av_audio_fifo_size(m_pcmFifo) == 0 && m_pendingFrameSamples == 0) {
    m_eof = true;  // Now truly at end
}
```

**Update AudioEngine::process()** to use `isInputExhausted()` for preload triggering:

**CRITICAL - Preload must be async:** With `isInputExhausted()` triggering preload while 250-500ms of buffered audio remains, a synchronous `preloadNextTrack()` call that performs network open + prefill could block longer than the remaining buffer, causing underruns. The preload must be dispatched to a background thread.

```cpp
// In AudioEngine::process(), change preload trigger:
// OLD: if (!m_nextDecoder && !m_nextURI.empty() && m_currentDecoder->isEOF()) {
// NEW: Use isInputExhausted() for earlier preload trigger - ASYNC dispatch
if (!m_nextDecoder && !m_nextURI.empty() && m_currentDecoder->isInputExhausted()
    && !m_preloadInProgress.load(std::memory_order_acquire)) {
    std::cout << "[AudioEngine] Input exhausted, dispatching async preload..." << std::endl;

    m_preloadInProgress.store(true, std::memory_order_release);

    // Dispatch to background thread (use existing thread pool or std::async)
    std::async(std::launch::async, [this]() {
        preloadNextTrack();  // Opens decoder + prefills FIFO
        m_preloadInProgress.store(false, std::memory_order_release);
    });
}
```

**Required additions to AudioEngine:**

```cpp
// In AudioEngine.h:
class AudioEngine {
private:
    std::atomic<bool> m_preloadInProgress{false};
};
```

**Why async is required:** The 250-500ms buffer provides playback time, not blocking time. If `preloadNextTrack()` takes 300ms on a slow network and the buffer only has 250ms remaining, the audio callback will underrun waiting for `process()` to return. Async dispatch lets the audio callback continue draining the buffer while preload happens in parallel.

**Consuming pending frame** (add to readSamples() after FIFO drain, before decode loop):

```cpp
// Drain pending frame buffer before decoding new frames
while (m_pendingFrameSamples > 0 && totalSamplesRead < numSamples) {
    size_t remaining = m_pendingFrameSamples - m_pendingFrameOffset;
    size_t needed = numSamples - totalSamplesRead;
    size_t toCopy = std::min(remaining, needed);

    size_t offsetBytes = m_pendingFrameOffset * bytesPerSample;
    memcpy(outputPtr, m_pendingFrameBuffer.data() + offsetBytes, toCopy * bytesPerSample);

    outputPtr += toCopy * bytesPerSample;
    totalSamplesRead += toCopy;
    m_pendingFrameOffset += toCopy;

    if (m_pendingFrameOffset >= m_pendingFrameSamples) {
        // Pending buffer fully consumed
        m_pendingFrameSamples = 0;
        m_pendingFrameOffset = 0;
    }
}
```

**CRITICAL - Handle FIFO overflow in normal decode loop:**

The overflow handling shown in `prefillFifo()` must also be applied in the main `readSamples()` decode loop. Without this, decoded frames that don't fit in the FIFO (likely when FIFO is near 80% capacity for network sources) will be truncated and samples dropped, causing audible gaps.

Add to `readSamples()` decode loop, after frame processing:

```cpp
// In readSamples() decode loop, after processing frame into FIFO:
// Check available space - write what fits, store remainder in pending buffer
int availableSpace = av_audio_fifo_space(m_pcmFifo);
size_t samplesToWrite = std::min((size_t)availableSpace, frameSamples);

if (samplesToWrite > 0) {
    uint8_t* writePtrs[1] = { frameData };
    int written = av_audio_fifo_write(m_pcmFifo, (void**)writePtrs, samplesToWrite);
    if (written < (int)samplesToWrite) {
        std::cerr << "[AudioDecoder] FIFO write error" << std::endl;
    }
}

// Store overflow in pending frame buffer (MUST NOT drop samples!)
if (frameSamples > samplesToWrite) {
    size_t overflow = frameSamples - samplesToWrite;
    size_t overflowBytes = overflow * bytesPerSample;
    size_t offsetBytes = samplesToWrite * bytesPerSample;

    if (m_pendingFrameBuffer.size() < overflowBytes) {
        m_pendingFrameBuffer.resize(overflowBytes);
    }
    memcpy(m_pendingFrameBuffer.data(), frameData + offsetBytes, overflowBytes);
    m_pendingFrameSamples = overflow;
    m_pendingFrameOffset = 0;

    DEBUG_LOG("[AudioDecoder] Stored " << overflow << " overflow samples in pending buffer");
    // Stop decoding - pending buffer is occupied, drain it on next readSamples() call
    break;
}
```

**Why this matters:** With network sources keeping FIFO near 80% for buffering protection, frame overflow is expected. The current code only logs partial writes and drops samples. This change ensures no samples are lost by routing overflow into the pending buffer, which is drained first on the next `readSamples()` call.

**Calling prefill** (in AudioEngine, after decoder opens for network source):

```cpp
// In AudioEngine::openCurrentTrack():
if (m_currentDecoder && m_currentTrackInfo.isNetworkSource) {
    // Prefill before first readSamples() call
    if (!m_currentDecoder->prefillFifo(outputRate, outputBits)) {
        std::cerr << "[AudioEngine] Network prefill failed" << std::endl;
    }
}
```

**CRITICAL - Also prefill gapless preloaded tracks:**

Gapless playback uses `preloadNextTrack()` and `transitionToNextTrack()` without calling `openCurrentTrack()`. Preloaded network tracks must also be prefilled to avoid starting with an empty FIFO at track boundaries.

```cpp
// Option A: Prefill during preload (preferred - fills buffer ahead of time)
// In AudioEngine::preloadNextTrack(), after decoder opens:
if (m_nextDecoder && m_nextTrackInfo.isNetworkSource) {
    if (!m_nextDecoder->prefillFifo(outputRate, outputBits)) {
        std::cerr << "[AudioEngine] Next track network prefill failed" << std::endl;
    }
}

// Option B: Prefill immediately after transition (fallback if preload is async)
// In AudioEngine::transitionToNextTrack(), after m_currentDecoder = std::move(m_nextDecoder):
if (m_currentDecoder && m_currentTrackInfo.isNetworkSource &&
    av_audio_fifo_size(m_currentDecoder->getFifo()) == 0) {
    // Only prefill if FIFO is empty (wasn't prefilled during preload)
    if (!m_currentDecoder->prefillFifo(outputRate, outputBits)) {
        std::cerr << "[AudioEngine] Transition prefill failed" << std::endl;
    }
}
```

**Why Option A is preferred:** Prefilling during preload fills the buffer while the current track is still playing, hiding the prefill latency. Option B blocks at the transition point, which could cause a gap if the network is slow.

**Modified readSamples()** - maintain FIFO level during playback:

**CRITICAL - Refill must be non-blocking:** The refill step runs after `numSamples` is already satisfied. If `av_read_frame()` blocks on network I/O, it will stall the audio callback and cause underruns - the opposite of the intended protection. Three options:

**Option A: Skip refill entirely in audio callback path (NOT RECOMMENDED)**

Remove opportunistic refill from `readSamples()`. Rely solely on the initial prefill depth (250-500ms) to absorb network jitter.

**Why this is insufficient:** The FIFO drains steadily during playback and is never topped up. Any WiFi jitter occurring after the initial prefill is consumed will cause underruns - the same problem we're trying to solve. This defeats the "adaptive buffering" goal and fails the test expectation that FIFO stays above watermark during playback.

**Option B: Non-blocking I/O with bounded timeout (more complex)**

```cpp
// At end of readSamples(), after satisfying numSamples request:
// For network sources, opportunistically decode more to maintain FIFO level
// Use m_inputExhausted (not m_eof) - we can still decode if input isn't exhausted
// CRITICAL: Only refill when pending buffer is empty - pending samples are older
// than any newly decoded frames and must be consumed first to preserve order
if (m_trackInfo.isNetworkSource && m_fifoTargetSamples > 0 && !m_inputExhausted
    && m_pendingFrameSamples == 0) {
    int currentLevel = av_audio_fifo_size(m_pcmFifo);
    int lowWatermark = m_fifoTargetSamples / 2;  // Refill when below 50%

    if (currentLevel < lowWatermark) {
        // Non-blocking refill: use interrupt callback with timeout
        // Set short timeout (e.g., 5ms) to avoid stalling audio callback
        AVIOInterruptCB prevCb = m_formatContext->interrupt_callback;
        auto startTime = std::chrono::steady_clock::now();

        m_formatContext->interrupt_callback.callback = [](void* ctx) -> int {
            auto* start = static_cast<std::chrono::steady_clock::time_point*>(ctx);
            auto elapsed = std::chrono::steady_clock::now() - *start;
            return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() > 5;
        };
        m_formatContext->interrupt_callback.opaque = &startTime;

        // Try to decode one frame (will abort if >5ms)
        int ret = av_read_frame(m_formatContext, m_packet);
        m_formatContext->interrupt_callback = prevCb;  // Restore

        if (ret >= 0) {
            // ... process frame, write to FIFO with overflow handling ...
        }
        // On timeout or error: just skip this refill attempt, try again next call
    }
}
```

**Option C: Move refill off audio thread (RECOMMENDED)**

Spawn a dedicated decode thread that keeps FIFO filled. Audio callback only reads from FIFO. This completely decouples network I/O from real-time audio delivery.

```cpp
// Add to AudioDecoder for background refill thread:
class AudioDecoder {
private:
    std::thread m_refillThread;
    std::atomic<bool> m_refillRunning{false};
    std::condition_variable m_refillCV;
    std::mutex m_refillMutex;

public:
    void startRefillThread();
    void stopRefillThread();
};

void AudioDecoder::startRefillThread() {
    if (!m_trackInfo.isNetworkSource || m_refillRunning.load()) return;

    m_refillRunning.store(true, std::memory_order_release);
    m_refillThread = std::thread([this]() {
        while (m_refillRunning.load(std::memory_order_acquire)) {
            std::unique_lock<std::mutex> lock(m_refillMutex);

            // Wait until FIFO below watermark or shutdown
            m_refillCV.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                if (!m_refillRunning.load()) return true;
                if (m_inputExhausted) return true;
                int level = av_audio_fifo_size(m_pcmFifo);
                return level < (int)(m_fifoTargetSamples / 2);
            });

            if (!m_refillRunning.load() || m_inputExhausted) break;

            // Decode frames until FIFO reaches target (blocking I/O OK here)
            while (av_audio_fifo_size(m_pcmFifo) < (int)m_fifoTargetSamples
                   && !m_inputExhausted && m_refillRunning.load()) {
                // ... decode one frame, write to FIFO with overflow handling ...
                // Lock FIFO access if readSamples() also accesses it
            }
        }
    });
}

void AudioDecoder::stopRefillThread() {
    m_refillRunning.store(false, std::memory_order_release);
    m_refillCV.notify_all();
    if (m_refillThread.joinable()) {
        m_refillThread.join();
    }
}
```

**Call sites:**
- `startRefillThread()`: After `prefillFifo()` completes (in `openCurrentTrack()` and `preloadNextTrack()`)
- `stopRefillThread()`: In destructor, `close()`, and before `seek()`

**Thread safety:** The FIFO is thread-safe for single-producer/single-consumer, but if both threads could write (refill thread writes, audio thread might also decode in fallback), add mutex protection around FIFO writes.

**Recommendation:** Implement Option C (background refill thread). Option B is acceptable as a simpler fallback but provides weaker guarantees under sustained network issues. Option A is insufficient for the stated goals.

// Update EOF state: only true when all sources exhausted
if (m_inputExhausted && av_audio_fifo_size(m_pcmFifo) == 0 && m_pendingFrameSamples == 0) {
    m_eof = true;
}
```

**Triggering elevation** (in DirettaRenderer, which owns both DirettaSync and AudioEngine):

```cpp
// In DirettaRenderer's audio callback, before track change triggers decoder init:
// DirettaRenderer owns m_direttaSync and can check underrun state
void DirettaRenderer::checkAndElevateBufferMode() {
    if (m_direttaSync && m_direttaSync->getUnderrunCount() > 0) {
        m_audioEngine->setElevatedBufferMode(true);
        DEBUG_LOG("[DirettaRenderer] Underruns detected - elevating buffer for next track");
    }
}

// Call in setAudioCallback lambda, before format change triggers new decoder:
// (inside the track change handling block)
checkAndElevateBufferMode();
```

**Note:** `AudioEngine` does not have access to `DirettaSync` - the underrun check must happen in `DirettaRenderer`, which then calls `m_audioEngine->setElevatedBufferMode(true)` to propagate the state without reaching into private `AudioDecoder` data.

## Files Modified

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `isNetworkSource` to TrackInfo; add `setElevatedBufferMode()`/`elevatedBufferMode()` and `s_elevatedBufferMode` to AudioDecoder; add `prefillFifo()`, `isInputExhausted()`, `clearPendingBuffer()`, `startRefillThread()`, `stopRefillThread()`, `m_fifoTargetSamples`, `m_inputExhausted`, `m_pendingFrameBuffer`, `m_pendingFrameSamples`, `m_pendingFrameOffset`, `m_refillThread`, `m_refillRunning`, `m_refillCV`, `m_refillMutex`; add `AudioEngine::setElevatedBufferMode()`, `m_preloadInProgress` |
| `src/AudioEngine.cpp` | Bit-depth based `request_sample_fmt` (~line 240); network detection (~line 136); adaptive FIFO sizing (~line 1101, 1173); implement `prefillFifo()` with decode-ahead loop, frame unref, and overflow storage; drain pending buffer in `readSamples()` before decode loop; handle FIFO overflow in normal decode loop (route to pending buffer); set `m_inputExhausted` (not `m_eof`) on av_read_frame EOF; set `m_eof` only when input exhausted AND buffers empty; async preload dispatch with `m_preloadInProgress` guard; call prefill in `openCurrentTrack()` AND `preloadNextTrack()`; clear pending buffer in `seek()` and reset paths; implement `startRefillThread()`/`stopRefillThread()` for background FIFO maintenance; implement elevated buffer setters/getters |
| `src/DirettaRenderer.h` | Add `checkAndElevateBufferMode()` private method |
| `src/DirettaRenderer.cpp` | Implement `checkAndElevateBufferMode()`; call before track change handling |
| `src/DirettaSync.h` | Add `m_underrunCount`, `getUnderrunCount()`, `resetUnderrunCount()` |
| `src/DirettaSync.cpp` | Increment counter on underrun (~line 1076); reset in `fullReset()` (~line 607) |

## Edge Cases

| Scenario | Behavior |
|----------|----------|
| FLAC decoder doesn't support packed | Capability check fails, falls back to swr (existing behavior) |
| `bits_per_raw_sample` is 0 | Falls back to `bits_per_coded_sample`, then planar check |
| Local file after network underrun | Elevated mode stays on (session-wide), slightly larger buffer than needed but harmless |
| First track underruns | Elevation applies to second track onward |
| WiFi recovers mid-session | Elevated mode persists (conservative - avoids repeated underruns) |
| Short track (< buffer size) | Prefill sets `m_inputExhausted`, FIFO+pending drained before `m_eof` set; track plays completely |
| Prefill stalls on network | Blocking call - may delay playback start (acceptable tradeoff vs. underruns) |
| Seek during playback | FIFO reset + pending buffer cleared + `m_inputExhausted` reset; refill thread continues filling from new position |
| FIFO nearly full during prefill | Overflow stored in pending buffer, consumed first by `readSamples()` - no samples dropped |
| Input EOF during prefill | `m_inputExhausted` set, `m_eof` deferred until FIFO+pending fully drained |
| Local source | `prefillFifo()` returns immediately, no refill thread started, no overhead added |
| Gapless transition (network track) | Next decoder prefilled during async preload; FIFO ready before transition |
| FIFO overflow during normal decode | Overflow routed to pending buffer, drained on next `readSamples()` - no samples dropped |
| Decoded frame while pending buffer occupied | Decode loop breaks, pending drained first before more decoding |
| Seek with pending overflow samples | Pending buffer cleared alongside FIFO - no stale pre-seek audio played |
| Preload triggered with slow network | Async dispatch allows audio callback to continue draining buffer while preload runs in background |
| Refill thread shutdown | `stopRefillThread()` called in destructor, `close()`, and before seek; thread joins cleanly |
| Mid-track WiFi jitter | Background refill thread maintains FIFO level; jitter absorbed by buffer depth |

## Testing

1. **FLAC bypass verification:**
   - Play 16-bit FLAC from Qobuz - verify "PCM BYPASS enabled" in logs
   - Play 24-bit FLAC from Qobuz - verify bypass and no swr allocation

2. **Network FIFO sizing:**
   - Play Qobuz track - verify "Network FIFO: X samples (250ms)" in logs
   - Verify local file still uses rate-based sizing

3. **Adaptive elevation:**
   - Simulate poor WiFi (throttle network)
   - Verify underrun count increments
   - Verify next track uses "500ms elevated" FIFO

4. **Mixed playback:**
   - Play local WAV after network track with underruns
   - Verify local sizing unchanged (unless elevated mode affects all sources - design choice)

5. **FIFO prefill verification:**
   - Play Qobuz track - verify "Prefilling network FIFO to X samples (Yms)" in logs
   - Verify "Prefill complete: X/Y samples" shows FIFO reached target before playback
   - Monitor FIFO level during playback - should stay above 50% watermark
   - Simulate brief network stall - verify no underrun if stall < buffer depth

6. **Prefill edge cases:**
   - Play very short network track (< 500ms) - verify partial prefill doesn't block indefinitely
   - Seek during network playback - verify refill triggers after seek completes
   - Local file - verify no prefill overhead (immediate return)

## Risk Assessment

| Risk | Likelihood | Impact | Mitigation |
|------|------------|--------|------------|
| FLAC decoder ignores packed request | Low | Medium | Capability check + swr fallback |
| `bits_per_raw_sample` not populated | Low | Low | Fallback chain: bits_per_coded_sample → planar check |
| 500ms FIFO causes memory pressure | Very Low | Low | Cap at 65536 samples (~1.5s at 44.1kHz) |
| Elevated mode never resets | By design | None | Conservative choice for session stability |
| Static flag thread safety | Low | Medium | Single writer (audio thread), atomic not needed but could add |
| Prefill blocks playback start | Medium | Low | Acceptable tradeoff - 250-500ms delay vs. underruns; could add timeout |
| FIFO maintenance adds decode overhead | Low | Low | Only triggers when below 50% watermark; single-frame decode is fast |
