# PCM Latency and Jitter Optimization Design

**Date:** 2026-01-12
**Goal:** Minimize latency and jitter in PCM decode/playback path
**Target:** ~300ms low-latency mode, jitter reduction through allocation elimination and flow control

---

## 1. Architecture Overview

**Problem:** Current PCM path has three jitter sources:
1. Per-call heap allocations in decode loop
2. 10ms blocking sleeps on backpressure
3. Fixed 8192-sample chunks regardless of buffer state

**Solution:** A layered approach that eliminates allocations, adds adaptive flow control, and exposes CLI tuning.

```
┌─────────────────────────────────────────────────────────────┐
│  CLI Flags                                                  │
│  --low-latency (preset: 300ms buffer, 30ms prefill, 2048)   │
│  --pcm-buffer-ms=N  --pcm-prefill-ms=N  --chunk-size=N      │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  AudioEngine (modified)                                     │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Reusable members (allocated once):                 │    │
│  │  - m_packet (AVPacket*)                             │    │
│  │  - m_frame (AVFrame*)                               │    │
│  │  - m_resampleBuffer (capacity-tracked)              │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│  DirettaRenderer audio callback                             │
│  ┌─────────────────────────────────────────────────────┐    │
│  │  Hybrid flow control:                               │    │
│  │  - Normal: 500µs sleep, 20ms cap                    │    │
│  │  - Critical (<10% buffer): early-return             │    │
│  └─────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────┘
```

**Key principle:** Hot path executes identical code regardless of buffer state or data size.

---

## 2. Per-Call Allocation Elimination

**Current problem** (`src/AudioEngine.cpp:746-747, 860-862`):

```cpp
// Called every readSamples() - heap allocation each time
AVPacket* packet = av_packet_alloc();
AVFrame* frame = av_frame_alloc();

// Called every resampled frame - heap allocation each time
AudioBuffer tempBuffer(tempBufferSize);
```

**Solution:** Move to class members with lazy initialization and capacity tracking.

```cpp
class AudioDecoder {
private:
    // Reusable FFmpeg structures (allocated once on first use)
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;

    // Reusable resample buffer with capacity tracking
    AudioBuffer m_resampleBuffer;
    size_t m_resampleBufferCapacity = 0;
};
```

**Lifecycle:**
- **Initialization:** Allocate in `open()` or lazily on first `readSamples()` call
- **Reuse:** `av_packet_unref()` / `av_frame_unref()` between uses (resets without realloc)
- **Cleanup:** Free in destructor and `close()`

**Resample buffer growth policy:**

```cpp
// Only reallocate if capacity insufficient
if (tempBufferSize > m_resampleBufferCapacity) {
    m_resampleBuffer.resize(tempBufferSize * 1.5);  // 50% headroom
    m_resampleBufferCapacity = m_resampleBuffer.size();
}
uint8_t* tempPtr = m_resampleBuffer.data();
```

**Expected impact:** Eliminates 2-4 heap allocations per `readSamples()` call.

---

## 3. Hybrid Flow Control

**Current problem** (`src/DirettaRenderer.cpp:257-269`):

```cpp
while (remainingSamples > 0 && retryCount < maxRetries) {
    size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);
    if (sent > 0) {
        retryCount = 0;
    } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));  // 10ms stall
        retryCount++;
    }
}
// maxRetries=50 → up to 500ms blocking
```

**Solution:** Hybrid approach with two modes based on buffer level.

```cpp
constexpr int MICROSLEEP_US = 500;
constexpr int MAX_WAIT_MS = 20;
constexpr int MAX_RETRIES = MAX_WAIT_MS * 1000 / MICROSLEEP_US;  // 40 retries
constexpr float CRITICAL_BUFFER_LEVEL = 0.10f;

float bufferLevel = m_direttaSync->getBufferLevel();
bool criticalMode = (bufferLevel < CRITICAL_BUFFER_LEVEL);

while (remainingSamples > 0 && retryCount < MAX_RETRIES) {
    size_t sent = m_direttaSync->sendAudio(audioData, remainingSamples);

    if (sent > 0) {
        remainingSamples -= sent / bytesPerSample;
        audioData += sent;
        retryCount = 0;
    } else {
        if (criticalMode) {
            DEBUG_LOG("[Audio] Early-return, buffer critical: " << bufferLevel);
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(MICROSLEEP_US));
        retryCount++;
    }
}
```

**Behavior summary:**

| Buffer Level | Backpressure Response | Max Stall |
|--------------|----------------------|-----------|
| ≥10% | Micro-sleep 500µs, up to 40 retries | 20ms |
| <10% | Immediate early-return | 0ms |

**Why 10% threshold?** At 300ms buffer, 10% = 30ms of audio remaining. Early-return ensures `audioThreadFunc` can refill before underrun.

---

## 4. CLI Interface

**New flags:**

```
--low-latency           Enable low-latency mode (preset: 300ms buffer, 30ms prefill, 2048 chunk)
--pcm-buffer-ms=N       PCM buffer size in milliseconds (default: 1000)
--pcm-prefill-ms=N      PCM prefill before playback starts (default: 50)
--chunk-size=N          Samples per process() call (default: 8192)
```

**Precedence:** Individual flags override `--low-latency` preset.

```bash
# Use preset
diretta-renderer --low-latency

# Use preset but override chunk size
diretta-renderer --low-latency --chunk-size=4096

# Fully custom
diretta-renderer --pcm-buffer-ms=500 --pcm-prefill-ms=40 --chunk-size=2048
```

**Configuration struct:**

```cpp
struct BufferConfig {
    size_t pcmBufferMs = 1000;
    size_t pcmPrefillMs = 50;
    size_t chunkSize = 8192;

    static BufferConfig lowLatency() {
        return { .pcmBufferMs = 300, .pcmPrefillMs = 30, .chunkSize = 2048 };
    }
};
```

**Validation:**
- `pcmBufferMs`: min 100, max 5000
- `pcmPrefillMs`: min 10, max pcmBufferMs/2
- `chunkSize`: must be power of 2, range 512-16384

---

## 5. Logging Gate

**Solution:** First-N unthrottled + rate-limited after.

```cpp
class RateLimitedLogger {
public:
    bool shouldLog() {
        if (m_count < FIRST_N_UNTHROTTLED) {
            m_count++;
            return true;
        }

        auto now = std::chrono::steady_clock::now();
        if (now - m_lastLog >= THROTTLE_INTERVAL) {
            m_lastLog = now;
            return true;
        }
        return false;
    }

    void reset() {
        m_count = 0;
        m_lastLog = std::chrono::steady_clock::time_point{};
    }

private:
    static constexpr int FIRST_N_UNTHROTTLED = 5;
    static constexpr auto THROTTLE_INTERVAL = std::chrono::seconds(1);

    int m_count = 0;
    std::chrono::steady_clock::time_point m_lastLog{};
};
```

**Behavior:**
- First 5 occurrences: always log (startup diagnosis)
- After that: max 1 log per second
- Resets on track change

---

## 6. Implementation Plan

**Files to modify:**

| File | Changes |
|------|---------|
| `src/AudioEngine.h` | Add `m_packet`, `m_frame`, `m_resampleBuffer` members; add `RateLimitedLogger` |
| `src/AudioEngine.cpp` | Refactor `readSamples()` to use reusable members; add logging gate |
| `src/DirettaRenderer.h` | Add `BufferConfig` struct |
| `src/DirettaRenderer.cpp` | Hybrid flow control in audio callback; use config for chunk size |
| `src/DirettaSync.h` | Make buffer constants non-constexpr, accept config |
| `src/DirettaSync.cpp` | Use configurable buffer/prefill values |
| `src/main.cpp` | Add CLI flag parsing, create `BufferConfig` |

**Implementation order:**

| Step | Task | Risk |
|------|------|------|
| 1 | CLI parsing + `BufferConfig` struct | Low |
| 2 | Wire config to `DirettaSync` buffer sizing | Low |
| 3 | Per-call allocation elimination in `AudioEngine` | Medium |
| 4 | Hybrid flow control in audio callback | Medium |
| 5 | Logging gate | Low |
| 6 | Integration test with `--low-latency` | Validation |

**Testing strategy:**
- Unit: Verify `BufferConfig` validation, `RateLimitedLogger` behavior
- Integration: Playback test with `--low-latency`, verify no underruns
- Optional benchmark: Measure jitter variance before/after

---

## Scope

**In scope:**
- Per-call allocation elimination
- Hybrid send loop (micro-sleep + early-return on critical)
- CLI flags: `--low-latency`, `--pcm-buffer-ms`, `--pcm-prefill-ms`, `--chunk-size`
- Logging gate (first-N + rate-limit)

**Out of scope:**
- AVX2 micro-optimizations (zero hoist)
- `memcpy_audio_fixed` for PCM ring writes (covered in memory optimization design)
- Config file changes
- Runtime toggle (UPnP/D-Bus)
