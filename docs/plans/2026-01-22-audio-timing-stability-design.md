# Audio Timing Stability Design

## Overview

Four interconnected optimizations to reduce timing jitter and improve predictability in the audio pipeline:

1. **Aligned AudioBuffer** - 64-byte aligned, grow-only allocation
2. **Timing Stability** - Quantized chunk sizes, steady `sleep_until` cadence
3. **Prefill Alignment** - Whole-buffer boundaries for prefill targets
4. **PCM Jitter Buffer** - Fill-then-drain model with non-blocking callbacks

## Problem Statement

### Current Issues

| Component | Problem | Effect |
|-----------|---------|--------|
| `AudioBuffer::resize()` | `new[]` on every call, no alignment | Heap allocations on hot path, poor AVX performance |
| `calculateAdaptiveChunkSize()` | Continuous 0.25x-1.5x scaling | Thousands of unique sizes, buffer churn |
| `audioThreadFunc()` | Variable `sleep_for()` based on buffer level | Irregular timing, oscillation |
| `m_prefillTarget` | Arbitrary byte count | Prefill "done" mid-buffer, early underrun |
| Audio callback | Retry loops with `sleep_for()` | Decode spikes bleed into send timing |

### Root Cause

Decode time variance (FLAC frames: 10-50ms) couples directly to send timing. No buffering discipline to absorb spikes.

## Design

### Part 1: Aligned AudioBuffer

**File:** `src/AudioEngine.h`, `src/AudioEngine.cpp`

```cpp
class AudioBuffer {
public:
    AudioBuffer(size_t size = 0);
    ~AudioBuffer();

    // Move semantics
    AudioBuffer(AudioBuffer&& other) noexcept;
    AudioBuffer& operator=(AudioBuffer&& other) noexcept;

    // Prevent copying
    AudioBuffer(const AudioBuffer&) = delete;
    AudioBuffer& operator=(const AudioBuffer&) = delete;

    // New: separate capacity from logical size
    void resize(size_t size);           // Sets logical size, grows capacity if needed
    void ensureCapacity(size_t cap);    // Pre-allocate without changing logical size

    size_t size() const { return m_size; }
    size_t capacity() const { return m_capacity; }
    uint8_t* data() { return m_data; }
    const uint8_t* data() const { return m_data; }

private:
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_capacity = 0;

    static constexpr size_t ALIGNMENT = 64;  // AVX-512 cacheline
    void growCapacity(size_t needed);
};
```

**Implementation:**

```cpp
AudioBuffer::AudioBuffer(size_t size) : m_data(nullptr), m_size(0), m_capacity(0) {
    if (size > 0) {
        resize(size);
    }
}

AudioBuffer::~AudioBuffer() {
    if (m_data) {
        std::free(m_data);  // Required for aligned_alloc memory
    }
}

AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity) {
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_capacity = 0;
}

AudioBuffer& AudioBuffer::operator=(AudioBuffer&& other) noexcept {
    if (this != &other) {
        std::free(m_data);
        m_data = other.m_data;
        m_size = other.m_size;
        m_capacity = other.m_capacity;
        other.m_data = nullptr;
        other.m_size = 0;
        other.m_capacity = 0;
    }
    return *this;
}

void AudioBuffer::resize(size_t size) {
    if (size > m_capacity) {
        growCapacity(size);
    }
    m_size = size;
}

void AudioBuffer::ensureCapacity(size_t cap) {
    if (cap > m_capacity) {
        growCapacity(cap);
    }
}

void AudioBuffer::growCapacity(size_t needed) {
    // Grow by 1.5x to reduce future reallocations
    size_t newCapacity = std::max(needed, m_capacity + m_capacity / 2);
    // Round up to alignment boundary
    newCapacity = (newCapacity + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

    uint8_t* newData = static_cast<uint8_t*>(std::aligned_alloc(ALIGNMENT, newCapacity));
    if (!newData) {
        throw std::bad_alloc();
    }

    // Copy existing data
    if (m_data && m_size > 0) {
        std::memcpy(newData, m_data, m_size);
    }

    std::free(m_data);
    m_data = newData;
    m_capacity = newCapacity;
}
```

**Key behaviors:**
- `resize(size)`: If `size <= m_capacity`, just update `m_size` (no allocation)
- `growCapacity()`: Uses `aligned_alloc(64, ...)`, grows by 1.5x
- Destructor: Uses `free()` (required for `aligned_alloc` memory)

---

### Part 2: Timing Stability

**File:** `src/DirettaRenderer.cpp`

**New constants:**

```cpp
namespace AudioTiming {
    // Quantized chunk sizes (samples)
    constexpr size_t PCM_CHUNK_LOW = 2048;    // ≤48kHz
    constexpr size_t PCM_CHUNK_MID = 4096;    // 88.2-96kHz
    constexpr size_t PCM_CHUNK_HIGH = 8192;   // ≥176.4kHz
    constexpr size_t DSD_CHUNK = 32768;       // All DSD rates

    // Jitter buffer targets (milliseconds)
    constexpr int JITTER_TARGET_COMPRESSED = 200;   // FLAC, ALAC
    constexpr int JITTER_TARGET_UNCOMPRESSED = 100; // WAV, AIFF
}
```

**Chunk size selection:**

```cpp
size_t DirettaRenderer::selectChunkSize(uint32_t sampleRate, bool isDSD) const {
    if (isDSD) return AudioTiming::DSD_CHUNK;
    if (sampleRate <= 48000) return AudioTiming::PCM_CHUNK_LOW;
    if (sampleRate <= 96000) return AudioTiming::PCM_CHUNK_MID;
    return AudioTiming::PCM_CHUNK_HIGH;
}
```

**New audioThreadFunc():**

```cpp
void DirettaRenderer::audioThreadFunc() {
    DEBUG_LOG("[Audio Thread] Started");

    using Clock = std::chrono::steady_clock;

    Clock::time_point nextWake = Clock::now();
    size_t currentChunk = 0;
    uint32_t lastSampleRate = 0;
    std::chrono::microseconds period{0};

    while (m_running) {
        if (!m_audioEngine) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        auto state = m_audioEngine->getState();

        if (state != AudioEngine::State::PLAYING) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            nextWake = Clock::now();  // Reset on state change
            lastSampleRate = 0;
            continue;
        }

        const auto& trackInfo = m_audioEngine->getCurrentTrackInfo();
        uint32_t sampleRate = trackInfo.sampleRate;

        if (sampleRate == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        // Recalculate period only when format changes
        if (sampleRate != lastSampleRate) {
            currentChunk = selectChunkSize(sampleRate, trackInfo.isDSD);
            period = std::chrono::microseconds(
                (currentChunk * 1000000ULL) / sampleRate
            );
            lastSampleRate = sampleRate;
            nextWake = Clock::now();  // Reset cadence

            DEBUG_LOG("[Audio Thread] Format: " << sampleRate << "Hz "
                      << (trackInfo.isDSD ? "DSD" : "PCM")
                      << ", chunk=" << currentChunk
                      << ", period=" << period.count() << "µs");
        }

        // Steady cadence: process then sleep until next wake
        m_audioEngine->process(currentChunk);

        nextWake += period;
        auto now = Clock::now();
        if (nextWake > now) {
            std::this_thread::sleep_until(nextWake);
        } else {
            // Running late - skip sleep, catch up
            nextWake = now;
        }
    }

    DEBUG_LOG("[Audio Thread] Stopped");
}
```

**Removed:** `calculateAdaptiveChunkSize()` function (no longer needed).

---

### Part 3: Prefill Alignment

**File:** `src/DirettaSync.h`, `src/DirettaSync.cpp`

**New member:**

```cpp
// DirettaSync.h
size_t m_prefillTargetBuffers{0};  // Prefill in whole buffer count
```

**New helper:**

```cpp
// DirettaSync.cpp
size_t DirettaSync::calculateAlignedPrefill(size_t bytesPerSecond, bool isDSD,
                                             bool isCompressed, size_t bytesPerBuffer) {
    // Target fill based on format complexity
    int targetMs = isCompressed ? AudioTiming::JITTER_TARGET_COMPRESSED
                                : AudioTiming::JITTER_TARGET_UNCOMPRESSED;

    // DSD uses fixed 150ms (not affected by compression flag)
    if (isDSD) {
        targetMs = 150;
    }

    // Convert to bytes
    size_t targetBytes = (bytesPerSecond * targetMs) / 1000;

    // Align UP to whole buffer boundary
    size_t targetBuffers = (targetBytes + bytesPerBuffer - 1) / bytesPerBuffer;

    // Clamp to reasonable bounds (min 8 buffers, max 1/4 ring)
    size_t ringSize = m_ringBuffer.size();
    size_t maxBuffers = ringSize / (4 * bytesPerBuffer);
    targetBuffers = std::max(targetBuffers, size_t{8});
    targetBuffers = std::min(targetBuffers, maxBuffers);

    return targetBuffers;
}
```

**Updated configureRingPCM():**

```cpp
void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    // ... existing setup (unchanged) ...

    int bytesPerFrame = channels * direttaBps;
    int framesBase = rate / 1000;
    int framesRemainder = rate % 1000;
    m_bytesPerFrame.store(bytesPerFrame, std::memory_order_release);
    m_framesPerBufferRemainder.store(static_cast<uint32_t>(framesRemainder), std::memory_order_release);
    m_framesPerBufferAccumulator.store(0, std::memory_order_release);
    m_bytesPerBuffer.store(framesBase * bytesPerFrame, std::memory_order_release);

    size_t bytesPerSecond = static_cast<size_t>(rate) * channels * direttaBps;
    size_t bytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);

    // NEW: Aligned prefill calculation
    bool isCompressed = m_currentFormat.isCompressed;
    m_prefillTargetBuffers = calculateAlignedPrefill(
        bytesPerSecond, false, isCompressed, bytesPerBuffer);
    m_prefillTarget = m_prefillTargetBuffers * bytesPerBuffer;  // Exact multiple

    DIRETTA_LOG("Ring PCM: " << rate << "Hz " << channels << "ch "
                << direttaBps << "bps, buffer=" << ringSize
                << ", prefill=" << m_prefillTargetBuffers << " buffers ("
                << m_prefillTarget << " bytes, "
                << (isCompressed ? "compressed" : "uncompressed") << ")");

    // ... rest unchanged ...
}
```

**Updated configureRingDSD():**

```cpp
void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    // ... existing setup (unchanged) ...

    uint32_t bytesPerSecond = byteRate * channels;
    size_t bytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);

    // NEW: Aligned prefill calculation (DSD always uses 150ms)
    m_prefillTargetBuffers = calculateAlignedPrefill(
        bytesPerSecond, true, false, bytesPerBuffer);
    m_prefillTarget = m_prefillTargetBuffers * bytesPerBuffer;

    DIRETTA_LOG("Ring DSD: byteRate=" << byteRate << " ch=" << channels
                << " buffer=" << ringSize
                << " prefill=" << m_prefillTargetBuffers << " buffers ("
                << m_prefillTarget << " bytes)");

    // ... rest unchanged ...
}
```

---

### Part 4: PCM Jitter Buffer

**File:** `src/AudioEngine.h`, `src/AudioEngine.cpp`

**New members in AudioEngine:**

```cpp
// AudioEngine.h - private section
size_t m_jitterTargetSamples{0};    // Target fill level (samples)
size_t m_jitterMinSamples{0};       // Minimum before underrun warning
bool m_jitterBufferReady{false};    // True when target reached

// Pre-allocated output buffer (uses aligned AudioBuffer)
AudioBuffer m_outputBuffer;
```

**New method in AudioDecoder:**

```cpp
// AudioEngine.h - AudioDecoder class
AVAudioFifo* getFifo() { return m_pcmFifo; }
size_t fillFifo(size_t targetSamples);  // Decode until FIFO reaches target
```

**fillFifo() implementation:**

```cpp
// AudioEngine.cpp
size_t AudioDecoder::fillFifo(size_t targetSamples) {
    if (!m_codecContext || m_eof || !m_pcmFifo) {
        return 0;
    }

    size_t samplesAdded = 0;
    size_t currentLevel = av_audio_fifo_size(m_pcmFifo);

    // Decode frames until we reach target or run out of data
    while (currentLevel < targetSamples && !m_eof) {
        // Read and decode one packet
        if (!m_packet) m_packet = av_packet_alloc();
        if (!m_frame) m_frame = av_frame_alloc();

        int ret = av_read_frame(m_formatContext, m_packet);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                m_eof = true;
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

            // Convert frame to output format and add to FIFO
            // (reuse existing resampling logic from readSamples)
            if (m_bypassMode) {
                // Direct: add frame to FIFO
                uint8_t* data[1] = { m_frame->data[0] };
                av_audio_fifo_write(m_pcmFifo, (void**)data, m_frame->nb_samples);
                samplesAdded += m_frame->nb_samples;
            } else if (m_swrContext) {
                // Resample then add to FIFO
                // ... (reuse temp buffer path from readSamples)
            }

            av_frame_unref(m_frame);
        }

        currentLevel = av_audio_fifo_size(m_pcmFifo);
    }

    return samplesAdded;
}
```

**Updated process() - Fill-then-drain model:**

```cpp
bool AudioEngine::process(size_t samplesNeeded) {
    // ... existing async seek handling ...

    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_state.load() != State::PLAYING) {
        return false;
    }

    // ... existing pending next URI handling ...

    // Safety net: reopen if needed
    if (!m_currentDecoder) {
        if (!m_currentURI.empty()) {
            if (!openCurrentTrack()) {
                m_state = State::STOPPED;
                if (m_trackEndCallback) m_trackEndCallback();
                return false;
            }
        } else {
            return false;
        }
    }

    const TrackInfo& info = m_currentTrackInfo;
    uint32_t outputRate = info.sampleRate;
    uint32_t outputBits = info.bitDepth;
    uint32_t outputChannels = info.channels;

    // Calculate jitter buffer target (once per track)
    if (m_jitterTargetSamples == 0 && outputRate > 0) {
        int targetMs = info.isCompressed
            ? AudioTiming::JITTER_TARGET_COMPRESSED
            : AudioTiming::JITTER_TARGET_UNCOMPRESSED;
        m_jitterTargetSamples = (outputRate * targetMs) / 1000;
        m_jitterMinSamples = m_jitterTargetSamples / 4;  // 25% of target
        DEBUG_LOG("[AudioEngine] Jitter target: " << m_jitterTargetSamples
                  << " samples (" << targetMs << "ms)");
    }

    size_t bytesPerSample = (outputBits <= 16) ? 2 : 4;
    bytesPerSample *= outputChannels;

    AVAudioFifo* fifo = m_currentDecoder->getFifo();
    if (!fifo) {
        // DSD or no FIFO - use original path
        return processLegacy(samplesNeeded);
    }

    // Phase 1: FILL - Top up jitter buffer to target
    if (!m_jitterBufferReady) {
        while (!m_currentDecoder->isEOF()) {
            size_t fifoLevel = av_audio_fifo_size(fifo);
            if (fifoLevel >= m_jitterTargetSamples) {
                m_jitterBufferReady = true;
                DEBUG_LOG("[AudioEngine] Jitter buffer ready: " << fifoLevel << " samples");
                break;
            }
            m_currentDecoder->fillFifo(m_jitterTargetSamples);
        }

        if (!m_jitterBufferReady && !m_currentDecoder->isEOF()) {
            return false;  // Still filling
        }
        m_jitterBufferReady = true;  // EOF counts as ready
    }

    // Phase 2: MAINTAIN - Keep buffer topped up (non-blocking)
    if (!m_currentDecoder->isEOF()) {
        size_t fifoLevel = av_audio_fifo_size(fifo);
        if (fifoLevel < m_jitterTargetSamples) {
            m_currentDecoder->fillFifo(m_jitterTargetSamples);
        }
    }

    // Phase 3: DRAIN - Output exactly samplesNeeded
    size_t fifoLevel = av_audio_fifo_size(fifo);
    size_t toOutput = samplesNeeded;

    if (fifoLevel < samplesNeeded) {
        if (fifoLevel < m_jitterMinSamples && fifoLevel > 0) {
            DEBUG_LOG("[AudioEngine] Jitter buffer low: " << fifoLevel);
        }
        toOutput = fifoLevel;
    }

    if (toOutput == 0) {
        // Buffer empty - check for track transition
        if (m_currentDecoder->isEOF()) {
            // ... existing EOF/gapless handling ...
        }
        return false;
    }

    // Ensure output buffer capacity (grow-only)
    m_outputBuffer.ensureCapacity(toOutput * bytesPerSample);
    m_outputBuffer.resize(toOutput * bytesPerSample);

    // Drain from FIFO
    uint8_t* outPtr = m_outputBuffer.data();
    int read = av_audio_fifo_read(fifo, (void**)&outPtr, toOutput);

    if (read <= 0) {
        return false;
    }

    // Phase 4: SEND - Non-blocking
    if (m_audioCallback) {
        bool continuePlayback = m_audioCallback(
            m_outputBuffer, read, outputRate, outputBits, outputChannels);
        if (!continuePlayback) {
            m_state = State::STOPPED;
            return false;
        }
    }

    m_samplesPlayed += read;
    return true;
}
```

**Updated audio callback (non-blocking):**

```cpp
// DirettaRenderer.cpp - audio callback
m_audioEngine->setAudioCallback(
    [this](const AudioBuffer& buffer, size_t samples,
           uint32_t sampleRate, uint32_t bitDepth, uint32_t channels) -> bool {

        // ... existing guard and format setup ...

        // ... existing open/format change handling ...

        // CHANGED: Non-blocking send (no retry loops)
        const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();

        if (trackInfo.isDSD) {
            // DSD: Single send attempt (data stays in jitter buffer if full)
            m_direttaSync->sendAudio(buffer.data(), samples);
        } else {
            // PCM: Single send attempt
            size_t bytesPerSample = (bitDepth == 24 || bitDepth == 32)
                ? 4 * channels : (bitDepth / 8) * channels;
            m_direttaSync->sendAudio(buffer.data(), samples);
        }

        return true;
    }
);
```

---

## Integration

### Data Flow

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         AUDIO THREAD (steady cadence)                    │
│                                                                          │
│   selectChunkSize() ──► 4096 samples (quantized)                        │
│          │                                                               │
│          ▼                                                               │
│   ┌──────────────────────────────────────────────────────────────────┐  │
│   │  AudioEngine::process(4096)                                      │  │
│   │                                                                  │  │
│   │  ┌─────────────┐    ┌─────────────┐    ┌─────────────┐          │  │
│   │  │   DECODE    │───►│  AVAudioFifo │───►│   OUTPUT    │          │  │
│   │  │  (variable) │    │  (200ms buf) │    │  (aligned)  │          │  │
│   │  └─────────────┘    └─────────────┘    └─────────────┘          │  │
│   │                      Jitter Buffer      AudioBuffer              │  │
│   │                      absorbs spikes     64-byte aligned          │  │
│   └──────────────────────────────────────────────────────────────────┘  │
│          │                                                               │
│          ▼                                                               │
│   m_audioCallback() ──► DirettaSync::sendAudio() ──► Ring Buffer        │
│   (non-blocking)        (no retries)                                    │
│          │                                                               │
│          ▼                                                               │
│   sleep_until(nextWake) ──► steady ~93ms period @ 44.1kHz              │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────┐
│                      DIRETTA SDK THREAD (1ms callbacks)                  │
│                                                                          │
│   getNewStream() ──► Ring Buffer ──► m_bytesPerBuffer bytes             │
│                      (prefill aligned to N × m_bytesPerBuffer)          │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

### Startup Sequence

1. `open()` → `configureRingPCM()` → calculate aligned prefill (e.g., 200 buffers)
2. First `process()` calls → fill jitter buffer to 200ms target
3. `m_jitterBufferReady = true` → start outputting
4. `sendAudio()` fills ring buffer → prefill reaches 200 × `m_bytesPerBuffer`
5. `m_prefillComplete = true` → `getNewStream()` starts draining
6. Steady state: audio thread wakes every ~93ms, outputs 4096 samples, sleeps

### Error Handling

| Condition | Behavior |
|-----------|----------|
| Decode slow | Jitter buffer drains below target, `fillFifo()` catches up next cycle |
| Decode EOF | Drain remaining FIFO, then transition to next track or stop |
| Ring full | `sendAudio()` returns 0, samples stay in FIFO, retry next cycle |
| Running late | Skip sleep, process immediately, reset `nextWake` |

---

## State Reset

Track change and seek operations must reset jitter buffer state:

```cpp
// In openCurrentTrack() and seek()
m_jitterTargetSamples = 0;
m_jitterMinSamples = 0;
m_jitterBufferReady = false;
```

---

## Migration Checklist

### AudioBuffer (src/AudioEngine.h, src/AudioEngine.cpp)
- [ ] Add `m_capacity` member
- [ ] Add `ensureCapacity()` method
- [ ] Change `resize()` to grow-only
- [ ] Change allocation to `std::aligned_alloc(64, ...)`
- [ ] Change deallocation to `std::free()`
- [ ] Add `capacity()` getter

### Timing Stability (src/DirettaRenderer.cpp)
- [ ] Add `AudioTiming` namespace with constants
- [ ] Add `selectChunkSize()` method
- [ ] Rewrite `audioThreadFunc()` with `sleep_until` cadence
- [ ] Remove `calculateAdaptiveChunkSize()` function
- [ ] Remove buffer-level throttling logic

### Prefill Alignment (src/DirettaSync.h, src/DirettaSync.cpp)
- [ ] Add `m_prefillTargetBuffers` member
- [ ] Add `calculateAlignedPrefill()` method
- [ ] Update `configureRingPCM()` to use aligned prefill
- [ ] Update `configureRingDSD()` to use aligned prefill
- [ ] Plumb `isCompressed` flag to prefill calculation

### PCM Jitter Buffer (src/AudioEngine.h, src/AudioEngine.cpp)
- [ ] Add jitter buffer members to `AudioEngine`
- [ ] Add `getFifo()` method to `AudioDecoder`
- [ ] Add `fillFifo()` method to `AudioDecoder`
- [ ] Rewrite `process()` with fill-then-drain model
- [ ] Add `processLegacy()` for DSD path
- [ ] Update audio callback to be non-blocking
- [ ] Reset jitter state in `openCurrentTrack()` and `seek()`

### Testing
- [ ] Test PCM playback (44.1kHz, 96kHz, 192kHz, 384kHz)
- [ ] Test DSD playback (DSD64, DSD128, DSD256)
- [ ] Test compressed formats (FLAC, ALAC)
- [ ] Test uncompressed formats (WAV, AIFF)
- [ ] Test gapless transitions (same format)
- [ ] Test format changes (PCM↔DSD, sample rate changes)
- [ ] Test seek during playback
- [ ] Verify no heap allocations in steady-state (valgrind/heaptrack)
- [ ] Measure timing jitter before/after (instrumenting sleep_until)
