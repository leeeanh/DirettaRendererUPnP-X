# Audio Timing Stability Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce timing jitter and improve audio pipeline predictability through aligned buffers, quantized chunk sizes, steady timing cadence, and a fill-then-drain jitter buffer model.

**Architecture:** Four interconnected changes: (1) 64-byte aligned grow-only AudioBuffer to eliminate hot-path allocations, (2) quantized chunk sizes with sleep_until cadence for steady timing, (3) whole-buffer prefill alignment to prevent mid-buffer underruns, (4) PCM jitter buffer using AVAudioFifo with non-blocking callbacks to absorb decode spikes.

**Tech Stack:** C++17, FFmpeg (AVAudioFifo), POSIX aligned_alloc, std::chrono::steady_clock

**Design Document:** `docs/plans/2026-01-22-audio-timing-stability-design.md`

---

## Part 1: Aligned AudioBuffer

### Task 1.1: Add capacity member and getter to AudioBuffer

**Files:**
- Modify: `src/AudioEngine.h:52-73`

**Step 1: Add m_capacity member**

In `src/AudioEngine.h`, find the AudioBuffer class private section (around line 70-72) and add the capacity member:

```cpp
private:
    uint8_t* m_data = nullptr;
    size_t m_size = 0;
    size_t m_capacity = 0;  // NEW: track allocated capacity

    static constexpr size_t ALIGNMENT = 64;  // AVX-512 cacheline
```

**Step 2: Add capacity() getter and ensureCapacity() method**

In the public section (around line 58-68), add:

```cpp
    void ensureCapacity(size_t cap);    // Pre-allocate without changing logical size
    size_t capacity() const { return m_capacity; }
```

**Step 3: Add private growCapacity() helper declaration**

In the private section:

```cpp
    void growCapacity(size_t needed);
```

**Step 4: Commit**

```bash
git add src/AudioEngine.h
git commit -m "$(cat <<'EOF'
feat(audio): add capacity tracking to AudioBuffer

Add m_capacity member and ALIGNMENT constant to support grow-only
allocation pattern. This prepares for eliminating hot-path heap
allocations.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 1.2: Implement grow-only resize and aligned allocation

**Files:**
- Modify: `src/AudioEngine.cpp:27-67`

**Step 1: Update constructor to initialize m_capacity**

Find the AudioBuffer constructor (lines 27-34) and update:

```cpp
AudioBuffer::AudioBuffer(size_t size) : m_data(nullptr), m_size(0), m_capacity(0) {
    if (size > 0) {
        resize(size);
    }
}
```

**Step 2: Update destructor to use std::free**

Find destructor (lines 36-40) and update:

```cpp
AudioBuffer::~AudioBuffer() {
    if (m_data) {
        std::free(m_data);  // Required for aligned_alloc memory
    }
}
```

**Step 3: Update move constructor to copy m_capacity**

Find move constructor (lines 42-48) and update:

```cpp
AudioBuffer::AudioBuffer(AudioBuffer&& other) noexcept
    : m_data(other.m_data), m_size(other.m_size), m_capacity(other.m_capacity) {
    other.m_data = nullptr;
    other.m_size = 0;
    other.m_capacity = 0;
}
```

**Step 4: Update move assignment to handle m_capacity and use std::free**

Find move assignment (lines 50-59) and update:

```cpp
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
```

**Step 5: Rewrite resize() to be grow-only**

Find resize() (lines 61-67) and replace:

```cpp
void AudioBuffer::resize(size_t size) {
    if (size > m_capacity) {
        growCapacity(size);
    }
    m_size = size;
}
```

**Step 6: Add ensureCapacity() method**

After resize():

```cpp
void AudioBuffer::ensureCapacity(size_t cap) {
    if (cap > m_capacity) {
        growCapacity(cap);
    }
}
```

**Step 7: Add growCapacity() implementation with aligned allocation**

After ensureCapacity():

```cpp
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

**Step 8: Add required include if not present**

At top of file, ensure `<cstdlib>` is included for aligned_alloc and free.

**Step 9: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors related to AudioBuffer

**Step 10: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
feat(audio): implement grow-only aligned AudioBuffer

- resize() now only grows capacity, never shrinks
- Uses std::aligned_alloc(64) for AVX-512 cache alignment
- Grows by 1.5x to amortize allocation cost
- Eliminates hot-path heap allocations in steady state

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Part 2: AudioTiming Header

### Task 2.1: Create AudioTiming.h with shared constants

**Files:**
- Create: `src/AudioTiming.h`

**Step 1: Create the header file**

```cpp
#ifndef AUDIO_TIMING_H
#define AUDIO_TIMING_H

#include <cstddef>

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

#endif // AUDIO_TIMING_H
```

**Step 2: Commit**

```bash
git add src/AudioTiming.h
git commit -m "$(cat <<'EOF'
feat(audio): add AudioTiming constants header

Shared namespace for quantized chunk sizes and jitter buffer targets.
Eliminates magic numbers and ensures consistency across components.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 2.2: Include AudioTiming.h in relevant files

**Files:**
- Modify: `src/DirettaRenderer.cpp`
- Modify: `src/AudioEngine.cpp`
- Modify: `src/DirettaSync.cpp`

**Step 1: Add include to DirettaRenderer.cpp**

Near top includes section:

```cpp
#include "AudioTiming.h"
```

**Step 2: Add include to AudioEngine.cpp**

Near top includes section:

```cpp
#include "AudioTiming.h"
```

**Step 3: Add include to DirettaSync.cpp**

Near top includes section:

```cpp
#include "AudioTiming.h"
```

**Step 4: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 5: Commit**

```bash
git add src/DirettaRenderer.cpp src/AudioEngine.cpp src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
chore: include AudioTiming.h in audio pipeline files

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Part 3: Timing Stability

### Task 3.1: Add selectChunkSize() method to DirettaRenderer

**Files:**
- Modify: `src/DirettaRenderer.h:85` (near calculateAdaptiveChunkSize)
- Modify: `src/DirettaRenderer.cpp`

**Step 1: Add declaration to header**

In `src/DirettaRenderer.h`, in the private section near line 85 (near calculateAdaptiveChunkSize), add:

```cpp
    size_t selectChunkSize(uint32_t sampleRate, bool isDSD) const;
```

**Step 2: Add implementation to cpp**

In `src/DirettaRenderer.cpp`, after calculateAdaptiveChunkSize() (around line 565), add:

```cpp
size_t DirettaRenderer::selectChunkSize(uint32_t sampleRate, bool isDSD) const {
    if (isDSD) return AudioTiming::DSD_CHUNK;
    if (sampleRate <= 48000) return AudioTiming::PCM_CHUNK_LOW;
    if (sampleRate <= 96000) return AudioTiming::PCM_CHUNK_MID;
    return AudioTiming::PCM_CHUNK_HIGH;
}
```

**Step 3: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 4: Commit**

```bash
git add src/DirettaRenderer.h src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
feat(audio): add selectChunkSize() for quantized timing

Returns fixed chunk sizes based on sample rate tier:
- ≤48kHz: 2048 samples
- 88.2-96kHz: 4096 samples
- ≥176.4kHz: 8192 samples
- DSD: 32768 samples

This replaces continuous scaling with discrete, predictable values.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3.2: Rewrite audioThreadFunc() with sleep_until cadence

**Files:**
- Modify: `src/DirettaRenderer.cpp:567-640`

**Step 1: Replace audioThreadFunc() implementation**

Find the current audioThreadFunc() (lines 567-640) and replace the entire function body:

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

**Step 2: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
feat(audio): rewrite audioThreadFunc with sleep_until cadence

Replace variable sleep_for() with steady sleep_until() timing:
- Fixed chunk sizes based on sample rate tier
- Periodic cadence computed once per format change
- Late processing catches up without oscillation
- Removes buffer-level throttling logic

This provides predictable, jitter-free audio thread timing.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 3.3: Remove calculateAdaptiveChunkSize() function

**Files:**
- Modify: `src/DirettaRenderer.h:85`
- Modify: `src/DirettaRenderer.cpp:545-565`

**Step 1: Remove declaration from header**

In `src/DirettaRenderer.h`, find and remove the declaration (around line 85):

```cpp
    size_t calculateAdaptiveChunkSize(size_t baseSamples, double bufferLevel);  // REMOVE THIS LINE
```

**Step 2: Remove implementation from cpp**

In `src/DirettaRenderer.cpp`, find and remove the entire function (lines 545-565):

```cpp
// REMOVE: entire calculateAdaptiveChunkSize function
size_t DirettaRenderer::calculateAdaptiveChunkSize(size_t baseSamples, double bufferLevel) {
    // ... entire function body ...
}
```

**Step 3: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors (function is no longer called)

**Step 4: Commit**

```bash
git add src/DirettaRenderer.h src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
refactor(audio): remove calculateAdaptiveChunkSize

No longer needed - replaced by selectChunkSize() with fixed tiers.
Continuous 0.25x-1.5x scaling caused thousands of unique chunk sizes
and buffer churn.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Part 4: Prefill Alignment

### Task 4.1: Add prefill alignment members to DirettaSync

**Files:**
- Modify: `src/DirettaSync.h`

**Step 1: Add m_prefillTargetBuffers member**

In `src/DirettaSync.h`, find the private section with prefill-related members (search for `m_prefillTarget`) and add nearby:

```cpp
    size_t m_prefillTargetBuffers{0};  // Prefill in whole buffer count
```

**Step 2: Add calculateAlignedPrefill() declaration**

In the private section, add:

```cpp
    size_t calculateAlignedPrefill(size_t bytesPerSecond, bool isDSD,
                                   bool isCompressed, size_t bytesPerBuffer);
```

**Step 3: Commit**

```bash
git add src/DirettaSync.h
git commit -m "$(cat <<'EOF'
feat(sync): add prefill alignment members to DirettaSync

Add m_prefillTargetBuffers for whole-buffer prefill tracking and
calculateAlignedPrefill() for computing aligned targets.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.2: Implement calculateAlignedPrefill()

**Files:**
- Modify: `src/DirettaSync.cpp`

**Step 1: Add calculateAlignedPrefill() implementation**

Add this function near configureRingPCM():

```cpp
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

**Step 2: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): implement calculateAlignedPrefill

Computes prefill target aligned to whole buffer boundaries:
- 200ms for compressed (FLAC, ALAC)
- 100ms for uncompressed (WAV, AIFF)
- 150ms for DSD
- Clamped between 8 buffers and 1/4 ring capacity

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.3: Update configureRingPCM() signature and implementation

**Files:**
- Modify: `src/DirettaSync.h`
- Modify: `src/DirettaSync.cpp:935-976`

**Step 1: Update declaration in header**

Find configureRingPCM declaration and add isCompressed parameter:

```cpp
    void configureRingPCM(int rate, int channels, int direttaBps, int inputBps, bool isCompressed);
```

**Step 2: Update implementation**

In `src/DirettaSync.cpp`, find configureRingPCM (line 935) and update:

1. Update function signature:
```cpp
void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps, bool isCompressed) {
```

2. After the existing frame calculation code (around lines 956-962), replace the prefill target calculation with:

```cpp
    size_t bytesPerSecond = static_cast<size_t>(rate) * channels * direttaBps;
    size_t bytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);

    // Aligned prefill calculation
    m_prefillTargetBuffers = calculateAlignedPrefill(
        bytesPerSecond, false, isCompressed, bytesPerBuffer);

    // Handle fractional frame remainder for non-standard rates
    int framesRemainder = rate % 1000;
    if (framesRemainder == 0) {
        m_prefillTarget = m_prefillTargetBuffers * bytesPerBuffer;
    } else {
        // Compute the sum of the next N callback sizes to stay on true boundaries
        size_t totalBytes = 0;
        uint32_t acc = 0;
        int bytesPerFrame = channels * direttaBps;
        for (size_t i = 0; i < m_prefillTargetBuffers; ++i) {
            size_t bytesThis = bytesPerBuffer;
            acc += static_cast<uint32_t>(framesRemainder);
            if (acc >= 1000) {
                acc -= 1000;
                bytesThis += static_cast<size_t>(bytesPerFrame);
            }
            totalBytes += bytesThis;
        }
        m_prefillTarget = totalBytes;
    }

    DIRETTA_LOG("Ring PCM: " << rate << "Hz " << channels << "ch "
                << direttaBps << "bps, buffer=" << ringSize
                << ", prefill=" << m_prefillTargetBuffers << " buffers ("
                << m_prefillTarget << " bytes, "
                << (isCompressed ? "compressed" : "uncompressed") << ")");
```

**Step 3: Update all call sites of configureRingPCM**

Search for all calls to configureRingPCM and add the isCompressed parameter. Likely locations:
- In DirettaRenderer.cpp audio callback
- Use `grep -rn "configureRingPCM" src/` to find all call sites

**Step 4: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 5: Commit**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
feat(sync): update configureRingPCM with aligned prefill

- Add isCompressed parameter for format-aware prefill targets
- Use calculateAlignedPrefill() for whole-buffer boundaries
- Handle fractional frame remainder for non-standard sample rates
- Log compression status for debugging

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 4.4: Update configureRingDSD() with aligned prefill

**Files:**
- Modify: `src/DirettaSync.cpp:978-1013`

**Step 1: Update configureRingDSD implementation**

Find configureRingDSD (line 978) and update the prefill calculation section:

```cpp
    uint32_t bytesPerSecond = byteRate * channels;
    size_t bytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);

    // Aligned prefill calculation (DSD always uses 150ms)
    m_prefillTargetBuffers = calculateAlignedPrefill(
        bytesPerSecond, true, false, bytesPerBuffer);
    m_prefillTarget = m_prefillTargetBuffers * bytesPerBuffer;

    DIRETTA_LOG("Ring DSD: byteRate=" << byteRate << " ch=" << channels
                << " buffer=" << ringSize
                << " prefill=" << m_prefillTargetBuffers << " buffers ("
                << m_prefillTarget << " bytes)");
```

**Step 2: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): update configureRingDSD with aligned prefill

Use calculateAlignedPrefill() with 150ms fixed target for DSD.
Ensures prefill completes on whole-buffer boundaries.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Part 5: PCM Jitter Buffer

### Task 5.1: Add AudioCallbackStatus and related types

**Files:**
- Modify: `src/AudioEngine.h`

**Step 1: Add callback types before AudioEngine class**

Find the area before the AudioEngine class definition and add:

```cpp
enum class AudioCallbackStatus { Sent, Backpressure, Stop };

struct AudioCallbackResult {
    AudioCallbackStatus status;
    size_t bytesConsumed;  // 0..payload.bytes
};

struct AudioCallbackPayload {
    const uint8_t* data;
    size_t bytes;
    size_t samples;
};

using AudioCallback = std::function<AudioCallbackResult(
    const AudioCallbackPayload&, uint32_t, uint32_t, uint32_t)>;
```

**Step 2: Commit**

```bash
git add src/AudioEngine.h
git commit -m "$(cat <<'EOF'
feat(audio): add backpressure-aware callback types

AudioCallbackResult returns status (Sent/Backpressure/Stop) and
bytesConsumed to support partial sends and backpressure handling.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.2: Add jitter buffer members to AudioEngine

**Files:**
- Modify: `src/AudioEngine.h`

**Step 1: Add jitter buffer members to AudioEngine private section**

Find the AudioEngine private section and add:

```cpp
    // Jitter buffer state
    size_t m_jitterTargetSamples{0};    // Target fill level (samples)
    size_t m_jitterMinSamples{0};       // Minimum before underrun warning
    bool m_jitterBufferReady{false};    // True when target reached

    // Pending output state (backpressure)
    size_t m_pendingBytes{0};           // Pending output bytes
    size_t m_pendingByteOffset{0};      // Byte offset into m_outputBuffer
    size_t m_pendingSamples{0};         // Pending samples

    // Pre-allocated output buffer (uses aligned AudioBuffer)
    AudioBuffer m_outputBuffer;
```

**Step 2: Commit**

```bash
git add src/AudioEngine.h
git commit -m "$(cat <<'EOF'
feat(audio): add jitter buffer state to AudioEngine

Track jitter buffer target/min levels, ready state, and pending
output for backpressure handling. Uses aligned AudioBuffer for output.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.3: Add FIFO access methods to AudioDecoder

**Files:**
- Modify: `src/AudioEngine.h:78-162`

**Step 1: Add getFifo() and ensureFifo() declarations**

In the AudioDecoder class public section, add:

```cpp
    AVAudioFifo* getFifo() { return m_pcmFifo; }
    bool ensureFifo(uint32_t outputRate, uint32_t outputBits);
    size_t fillFifo(size_t targetSamples, uint32_t outputRate, uint32_t outputBits);
```

**Step 2: Add FIFO spill buffer members**

In the AudioDecoder class private section, add:

```cpp
    // FIFO spill buffer for partial writes
    AudioBuffer m_fifoPendingBuffer;
    size_t m_fifoPendingBytes = 0;
    size_t m_fifoPendingOffset = 0;

    void resetFifoPending();  // Clear spill state on seek/track change
```

**Step 3: Commit**

```bash
git add src/AudioEngine.h
git commit -m "$(cat <<'EOF'
feat(audio): add FIFO access methods to AudioDecoder

- getFifo(): direct access for jitter buffer queries
- ensureFifo(): lazy initialization wrapper
- fillFifo(): decode until FIFO reaches target
- Spill buffer for partial av_audio_fifo_write results

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.4: Implement AudioDecoder FIFO methods

**Files:**
- Modify: `src/AudioEngine.cpp`

**Step 1: Implement ensureFifo()**

Add after existing AudioDecoder methods:

```cpp
bool AudioDecoder::ensureFifo(uint32_t outputRate, uint32_t outputBits) {
    if (m_resamplerInitialized) {
        return true;
    }
    return initResampler(outputRate, outputBits);
}
```

**Step 2: Implement resetFifoPending()**

```cpp
void AudioDecoder::resetFifoPending() {
    m_fifoPendingBytes = 0;
    m_fifoPendingOffset = 0;
}
```

**Step 3: Implement fillFifo()**

This is a larger function - add it after ensureFifo():

```cpp
size_t AudioDecoder::fillFifo(size_t targetSamples, uint32_t outputRate, uint32_t outputBits) {
    if (!m_codecContext || m_eof) {
        return 0;
    }

    size_t samplesAdded = 0;
    size_t bytesPerSample = (outputBits <= 16) ? 2 : 4;
    bytesPerSample *= m_trackInfo.channels;

    // Ensure resampler/bypass is initialized before decoding into FIFO
    if (!ensureFifo(outputRate, outputBits)) {
        return 0;
    }

    if (!m_pcmFifo) {
        return 0;
    }

    size_t currentLevel = av_audio_fifo_size(m_pcmFifo);

    // Flush any pending samples from prior partial FIFO write
    if (m_fifoPendingBytes > 0) {
        int space = av_audio_fifo_space(m_pcmFifo);
        size_t pendingSamples = m_fifoPendingBytes / bytesPerSample;
        size_t toWrite = std::min<size_t>(pendingSamples, static_cast<size_t>(space));
        if (toWrite > 0) {
            uint8_t* data[1] = { m_fifoPendingBuffer.data() + m_fifoPendingOffset };
            int written = av_audio_fifo_write(m_pcmFifo, (void**)data, toWrite);
            if (written > 0) {
                size_t bytesWritten = static_cast<size_t>(written) * bytesPerSample;
                m_fifoPendingOffset += bytesWritten;
                m_fifoPendingBytes -= bytesWritten;
                samplesAdded += written;
                if (m_fifoPendingBytes == 0) {
                    m_fifoPendingOffset = 0;
                }
            }
        }

        // If FIFO is still full, defer decoding until next call
        if (m_fifoPendingBytes > 0) {
            return samplesAdded;
        }
    }

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
            if (m_bypassMode) {
                // Direct: add frame to FIFO
                int space = av_audio_fifo_space(m_pcmFifo);
                size_t frameSamples = static_cast<size_t>(m_frame->nb_samples);
                size_t toWrite = std::min<size_t>(frameSamples, static_cast<size_t>(space));

                if (toWrite > 0) {
                    uint8_t* data[1] = { m_frame->data[0] };
                    int written = av_audio_fifo_write(m_pcmFifo, (void**)data, toWrite);
                    if (written > 0) {
                        samplesAdded += written;
                    }
                    toWrite = static_cast<size_t>(written);
                }

                // Preserve remainder when FIFO is nearly full
                if (toWrite < frameSamples) {
                    size_t remainingSamples = frameSamples - toWrite;
                    size_t remainingBytes = remainingSamples * bytesPerSample;
                    m_fifoPendingBuffer.resize(remainingBytes);
                    std::memcpy(m_fifoPendingBuffer.data(),
                                m_frame->data[0] + toWrite * bytesPerSample,
                                remainingBytes);
                    m_fifoPendingBytes = remainingBytes;
                    m_fifoPendingOffset = 0;
                    av_frame_unref(m_frame);
                    break;
                }
            } else if (m_swrContext) {
                // Resample then add to FIFO
                // Use existing resampling logic pattern from readSamples()
                int outSamples = swr_get_out_samples(m_swrContext, m_frame->nb_samples);
                m_resampleBuffer.resize(outSamples * bytesPerSample);

                uint8_t* outData[1] = { m_resampleBuffer.data() };
                int converted = swr_convert(m_swrContext, outData, outSamples,
                                           (const uint8_t**)m_frame->data, m_frame->nb_samples);

                if (converted > 0) {
                    int space = av_audio_fifo_space(m_pcmFifo);
                    size_t toWrite = std::min<size_t>(static_cast<size_t>(converted),
                                                       static_cast<size_t>(space));

                    if (toWrite > 0) {
                        uint8_t* data[1] = { m_resampleBuffer.data() };
                        int written = av_audio_fifo_write(m_pcmFifo, (void**)data, toWrite);
                        if (written > 0) {
                            samplesAdded += written;
                        }
                        toWrite = static_cast<size_t>(written);
                    }

                    // Preserve remainder
                    if (toWrite < static_cast<size_t>(converted)) {
                        size_t remainingSamples = converted - toWrite;
                        size_t remainingBytes = remainingSamples * bytesPerSample;
                        m_fifoPendingBuffer.resize(remainingBytes);
                        std::memcpy(m_fifoPendingBuffer.data(),
                                    m_resampleBuffer.data() + toWrite * bytesPerSample,
                                    remainingBytes);
                        m_fifoPendingBytes = remainingBytes;
                        m_fifoPendingOffset = 0;
                        av_frame_unref(m_frame);
                        break;
                    }
                }
            }

            av_frame_unref(m_frame);
        }

        currentLevel = av_audio_fifo_size(m_pcmFifo);
    }

    return samplesAdded;
}
```

**Step 4: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 5: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
feat(audio): implement AudioDecoder FIFO methods

- ensureFifo(): lazy FIFO initialization
- resetFifoPending(): clear spill state
- fillFifo(): decode until FIFO reaches target samples
  - Handles bypass mode and resampler paths
  - Preserves partial frames in spill buffer when FIFO full

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.5: Add jitter buffer reset helper to AudioEngine

**Files:**
- Modify: `src/AudioEngine.h`
- Modify: `src/AudioEngine.cpp`

**Step 1: Add declaration**

In AudioEngine private section:

```cpp
    void resetJitterState();
```

**Step 2: Implement resetJitterState()**

```cpp
void AudioEngine::resetJitterState() {
    m_jitterTargetSamples = 0;
    m_jitterMinSamples = 0;
    m_jitterBufferReady = false;
    m_pendingBytes = 0;
    m_pendingByteOffset = 0;
    m_pendingSamples = 0;
    if (m_currentDecoder) {
        m_currentDecoder->resetFifoPending();
    }
}
```

**Step 3: Call resetJitterState() in appropriate places**

Add calls to resetJitterState() in:
- `openCurrentTrack()` - after successfully opening
- `seek()` - after seeking
- `transitionToNextTrack()` - during gapless transition
- Any place where track or format changes

**Step 4: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 5: Commit**

```bash
git add src/AudioEngine.h src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
feat(audio): add resetJitterState helper

Centralized reset for jitter buffer and pending output state.
Called on track change, seek, and gapless transitions.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.6: Add processLegacy() for DSD path

**Files:**
- Modify: `src/AudioEngine.h`
- Modify: `src/AudioEngine.cpp`

**Step 1: Add declaration**

In AudioEngine private section:

```cpp
    bool processLegacy(size_t samplesNeeded);  // Original path for DSD
```

**Step 2: Implement processLegacy()**

Extract the current process() implementation into processLegacy() before modifying process(). This preserves the existing DSD handling:

```cpp
bool AudioEngine::processLegacy(size_t samplesNeeded) {
    // [Copy existing process() implementation here]
    // This is the fallback for DSD and cases without FIFO
}
```

**Step 3: Commit**

```bash
git add src/AudioEngine.h src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
refactor(audio): extract processLegacy for DSD path

Move existing process() logic to processLegacy() to preserve
DSD handling. New jitter buffer logic will be added to process().

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.7: Rewrite process() with fill-then-drain model

**Files:**
- Modify: `src/AudioEngine.cpp`

**Step 1: Rewrite process() function**

Replace the process() implementation with the fill-then-drain model from the design document. The key phases are:

1. **FILL** - Top up jitter buffer to target (blocking on first fill)
2. **MAINTAIN** - Keep buffer topped up (non-blocking)
3. **RETRY** - Handle pending output from backpressure
4. **DRAIN** - Output exactly samplesNeeded from FIFO
5. **SEND** - Non-blocking callback with backpressure tracking

See design document `docs/plans/2026-01-22-audio-timing-stability-design.md` Part 4 for complete implementation.

**Step 2: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "$(cat <<'EOF'
feat(audio): rewrite process() with fill-then-drain jitter buffer

Four-phase processing:
1. FILL: Block until jitter buffer reaches target (first fill only)
2. MAINTAIN: Non-blocking top-up each cycle
3. RETRY: Handle pending output from backpressure
4. DRAIN/SEND: Output from FIFO with backpressure awareness

Absorbs decode time variance, provides steady output timing.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 5.8: Update audio callback to be non-blocking

**Files:**
- Modify: `src/DirettaRenderer.cpp`

**Step 1: Update the audio callback**

Find the setAudioCallback lambda in DirettaRenderer and update to:
- Accept `AudioCallbackPayload` parameter
- Return `AudioCallbackResult` with status and bytesConsumed
- Remove retry loops
- Single send attempt, return Backpressure on partial/zero

See design document Part 4 "Updated audio callback" section.

**Step 2: Verify compilation**

Run: `make -j$(nproc) 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
feat(audio): update callback to non-blocking backpressure model

- Accept AudioCallbackPayload, return AudioCallbackResult
- Single send attempt, no retry loops
- Return Backpressure status for partial/zero sends
- Decouples decode timing from send timing

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Part 6: Integration Testing

### Task 6.1: Manual testing checklist

**Test each scenario manually:**

1. **PCM playback at various sample rates**
   - 44.1kHz (FLAC) - verify 2048 chunk, ~46ms period
   - 96kHz (WAV) - verify 4096 chunk, ~43ms period
   - 192kHz (FLAC) - verify 8192 chunk, ~43ms period
   - 384kHz - verify 8192 chunk, ~21ms period

2. **DSD playback**
   - DSD64 - verify 32768 chunk
   - DSD128 - verify 32768 chunk
   - DSD256 - verify 32768 chunk

3. **Format transitions**
   - PCM to PCM (same rate) - verify gapless
   - PCM to PCM (different rate) - verify clean transition
   - PCM to DSD - verify format switch
   - DSD to PCM - verify format switch

4. **Stress tests**
   - Seek during playback - verify no glitches
   - Rapid track skip - verify no crashes
   - Long playback (1+ hour) - verify memory stable

**Step 1: Run each test manually**

Document results in a test log.

**Step 2: Commit test results**

```bash
git add docs/test-results/
git commit -m "$(cat <<'EOF'
test: audio timing stability manual test results

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

### Task 6.2: Verify no steady-state heap allocations

**Step 1: Run with heaptrack or valgrind**

```bash
heaptrack ./DirettaRenderer &
# Play audio for 30+ seconds
# Stop and analyze
heaptrack --analyze heaptrack.DirettaRenderer.*.gz
```

**Step 2: Check for hot-path allocations**

Look for allocations in:
- `AudioBuffer::resize()` - should only allocate on first use
- `audioThreadFunc()` - should have zero allocations
- `AudioEngine::process()` - should have zero allocations in steady state

**Step 3: Document findings**

---

## Summary

This plan implements four interconnected optimizations:

| Part | Component | Key Change |
|------|-----------|------------|
| 1 | AudioBuffer | 64-byte aligned, grow-only allocation |
| 2 | AudioTiming | Shared constants header |
| 3 | audioThreadFunc | Quantized chunks, sleep_until cadence |
| 4 | DirettaSync | Whole-buffer prefill alignment |
| 5 | AudioEngine | Fill-then-drain jitter buffer with backpressure |
| 6 | Testing | Manual verification and heap profiling |

Total tasks: ~20 bite-sized steps
Estimated commits: 15-18
