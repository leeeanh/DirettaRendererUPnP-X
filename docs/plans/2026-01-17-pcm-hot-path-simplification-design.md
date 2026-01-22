# PCM Hot Path Simplification Design

**Date:** 2026-01-17
**Status:** Ready for implementation
**Scope:** PCM playback only (DSD optimizations deferred)

## Objective

Reduce timing variance in the audio callback hot path for PCM playback by eliminating syscalls, unnecessary branches, and I/O operations. The hypothesis is that simpler, more predictable code execution leads to better subjective audio quality through reduced jitter.

## Optimizations

| ID | Optimization | File(s) | Impact |
|----|--------------|---------|--------|
| C0 | Remove Mutex/Notify from callback | DirettaRenderer.h/.cpp | Very High |
| C1 | Fix modulo → bitmask in writeToRing | DirettaRingBuffer.h | High |
| C4 | Remove dual memcpy dispatch | DirettaRingBuffer.h | Medium |
| C6 | Remove I/O on underrun | DirettaSync.h/.cpp | Medium |

### Out of Scope (Deferred)

- C7: LUT consolidation (DSD-specific)
- S2: Legacy pushDSDPlanar removal (DSD-specific)
- S1: Remove disabled code blocks (code doesn't exist in current codebase)

---

## Phase 1: C0 - Mutex/Notify Removal

### Problem

The audio callback acquires a mutex and calls `notify_all()` on every frame. At 44.1kHz, that's ~44,100 syscalls per second for synchronization only needed during shutdown.

### Changes

**DirettaRenderer.h** - Replace mutex/CV with atomics:

```cpp
// BEFORE
mutable std::mutex m_callbackMutex;
std::condition_variable m_callbackCV;
bool m_callbackRunning{false};

// AFTER
std::atomic<bool> m_callbackRunning{false};
std::atomic<bool> m_shutdownRequested{false};
```

**DirettaRenderer.cpp** - Callback entry:

```cpp
// CRITICAL: Set running flag FIRST, then check shutdown.
// This order prevents a race where stopper checks m_callbackRunning
// before we set it, but after checking m_shutdownRequested.
// Use seq_cst to ensure total ordering across both flags.
m_callbackRunning.store(true, std::memory_order_seq_cst);

if (m_shutdownRequested.load(std::memory_order_seq_cst)) {
    m_callbackRunning.store(false, std::memory_order_release);
    return false;
}

struct Guard {
    std::atomic<bool>& flag;
    ~Guard() { flag.store(false, std::memory_order_release); }
} guard{m_callbackRunning};
```

**DirettaRenderer.cpp** - waitForCallbackComplete():

```cpp
void DirettaRenderer::waitForCallbackComplete() {
    // Use seq_cst to pair with callback's seq_cst operations
    m_shutdownRequested.store(true, std::memory_order_seq_cst);

    auto start = std::chrono::steady_clock::now();
    while (m_callbackRunning.load(std::memory_order_seq_cst)) {
        std::this_thread::yield();
        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > std::chrono::seconds(5)) {
            std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
            // Reset flag to avoid permanent "callback running" state
            m_callbackRunning.store(false, std::memory_order_release);
            break;
        }
    }

    m_shutdownRequested.store(false, std::memory_order_release);
}
```

**DirettaRenderer.cpp** - Replace mutex in onSetURI/onStop:

The current code uses `m_callbackMutex` to serialize `m_audioEngine->stop()` against callback entry:

```cpp
// BEFORE (onSetURI line 363, onStop line 442)
{
    std::lock_guard<std::mutex> cbLock(m_callbackMutex);
    m_audioEngine->stop();
}
waitForCallbackComplete();
```

Replace with:

```cpp
// AFTER - waitForCallbackComplete() handles the shutdown flag
m_audioEngine->stop();
waitForCallbackComplete();
```

Why this works:
- `waitForCallbackComplete()` sets `m_shutdownRequested` with seq_cst
- The callback checks this flag AFTER setting `m_callbackRunning`
- The seq_cst ordering ensures: either the stopper sees `m_callbackRunning=true` and waits, or the callback sees `m_shutdownRequested=true` and exits
- No mutex needed because atomic seq_cst provides the total ordering

### Testing

- Start/stop playback multiple times
- Verify clean shutdown (no hangs)
- PCM playback at 44.1k, 96k, 192k

---

## Phase 2: C1 + C4 - Ring Buffer Optimizations

### C1: Modulo → Bitmask

**Problem:** Division instruction has variable latency (20-100 cycles).

**DirettaRingBuffer.h:**

```cpp
// BEFORE
size_t newWritePos = (writePos + len) % size;

// AFTER
size_t newWritePos = (writePos + len) & mask_;
```

Single AND instruction (1 cycle). `mask_` already exists as `size_ - 1`.

### C4: Remove Dual Memcpy Dispatch

**Problem:** Branch misprediction costs ~15-20 cycles.

**DirettaRingBuffer.h:**

```cpp
// BEFORE
if (firstChunk >= 32) {
    memcpy_audio_fixed(ring + writePos, staged, firstChunk);
} else if (firstChunk > 0) {
    std::memcpy(ring + writePos, staged, firstChunk);
}

// AFTER
if (firstChunk > 0) {
    memcpy_audio_fixed(ring + writePos, staged, firstChunk);
}
```

`memcpy_audio_fixed()` already handles small sizes correctly.

### Testing

- PCM playback at all sample rates
- Gapless playback test (verify no buffer corruption)

---

## Phase 3: C6 - Remove I/O on Underrun

### Problem

`std::cerr` during underrun can block for milliseconds, causing cascading underruns.

### Changes

**DirettaSync.h** - Add counter:

```cpp
std::atomic<uint32_t> m_underrunCount{0};
```

**DirettaSync.cpp** - In getNewStream():

```cpp
// BEFORE
if (avail < currentBytesPerBuffer) {
    std::cerr << "[DirettaSync] UNDERRUN..." << std::endl;
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
}

// AFTER
if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
    m_underrunCount.fetch_add(1, std::memory_order_relaxed);
    std::memset(dest, currentSilenceByte, currentBytesPerBuffer);
}
```

**DirettaSync.cpp** - In stopPlayback():

```cpp
uint32_t underruns = m_underrunCount.exchange(0, std::memory_order_relaxed);
if (underruns > 0) {
    std::cerr << "[DirettaSync] Session had " << underruns << " underrun(s)" << std::endl;
}
```

### Testing

- Normal playback (no underrun message during play)
- Stress test with CPU load, verify count reported at stop

---

## Summary

### Files Modified

| File | Changes |
|------|---------|
| DirettaRenderer.h | Replace mutex/CV with 2 atomics |
| DirettaRenderer.cpp | Rewrite callback guard, waitForCallbackComplete(), onSetURI/onStop mutex removal |
| DirettaRingBuffer.h | Fix modulo, simplify memcpy dispatch |
| DirettaSync.h | Add m_underrunCount atomic |
| DirettaSync.cpp | Defer underrun logging |

### Net Result

- Syscalls eliminated from hot path (mutex lock, notify_all)
- Division replaced with bitmask (20-100 cycles → 1 cycle)
- Branch removed from memcpy dispatch
- Underrun logging moved to cold path

### Verification

**Functional (after each phase):**
- [ ] PCM 16-bit/44.1kHz
- [ ] PCM 24-bit/96kHz
- [ ] PCM 24-bit/192kHz
- [ ] Start/stop cycles
- [ ] Track transitions

**Listening (after all phases):**
- [ ] A/B comparison on test device
- [ ] Focus: clarity, background noise, timing/rhythm
