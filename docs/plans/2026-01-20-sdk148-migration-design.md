# SDK 148 Migration: Hybrid Zero-Copy Design

## Problem Statement

SDK 148 introduces breaking changes to the `getNewStream()` callback:

| Aspect | SDK 147 | SDK 148 |
|--------|---------|---------|
| Signature | `getNewStream(DIRETTA::Stream&)` | `getNewStream(diretta_stream&)` |
| Virtual | Non-pure virtual | **Pure virtual** |
| Buffer ownership | SDK manages via `Stream::resize()` | **Caller provides buffer** |
| Copy semantics | Copy allowed | **Move-only (copy deleted)** |
| Inheritance | `class Stream : private diretta_stream` | `class Stream : public diretta_stream` |

**Root cause of crashes:** After Stop→Play, calling `Stream` methods (`resize()`, `get_16()`) on the `diretta_stream&` parameter causes segfaults because these methods don't exist on the base struct.

## Design Goals

1. **Zero-copy when possible** - Point SDK directly at ring buffer (~90% of calls)
2. **Minimal jitter** - No allocations in hot path, predictable fallback
3. **Thread safety** - Maintain lock-free producer-consumer pattern
4. **Clean migration** - Isolated changes, easy to test/revert

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        getNewStream()                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                 │
│   ┌──────────────────┐     ┌─────────────────────────────────┐  │
│   │ Ring Buffer      │     │ Fallback Buffer                 │  │
│   │ (circular)       │     │ (pre-allocated, linear)         │  │
│   │                  │     │                                 │  │
│   │  ┌────────────┐  │     │  ┌─────────────────────────┐    │  │
│   │  │ contiguous │──┼──┐  │  │ m_fallbackBuffer        │    │  │
│   │  │ read region│  │  │  │  │ (64-byte aligned)       │    │  │
│   │  └────────────┘  │  │  │  └─────────────────────────┘    │  │
│   │                  │  │  │              ▲                  │  │
│   │  ┌────────────┐  │  │  │              │ pop() when       │  │
│   │  │ wrapped    │──┼──┼──┼──────────────┘ data wraps       │  │
│   │  │ data       │  │  │  │                                 │  │
│   │  └────────────┘  │  │  │                                 │  │
│   └──────────────────┘  │  └─────────────────────────────────┘  │
│                         │                                       │
│                         ▼                                       │
│              ┌──────────────────────┐                           │
│              │ diretta_stream       │                           │
│              │   .Data.P = ptr      │ ───────► SDK reads        │
│              │   .Size = bytes      │          (asynchronously) │
│              └──────────────────────┘                           │
│                                                                 │
└─────────────────────────────────────────────────────────────────┘
```

## Critical: Deferred Read Position Advance

**SDK Consumption Model:** The SDK reads the buffer **asynchronously** after `getNewStream()` returns.
Per documentation: *"The pointer must remain valid until it is dereferenced or this function is called again."*

**Problem with immediate advance:** If we advance the read position immediately after pointing
the SDK at the ring buffer, the producer sees that region as "free" and may overwrite it
while the SDK is still reading, causing audio corruption or crashes.

**Solution: Deferred advance pattern**

```
Call N:
  1. Complete pending advance from call N-1 (if any)
  2. Point SDK at ring buffer region [X, X+size)
  3. Save pendingAdvance = size (DON'T advance yet)
  4. Return - SDK reads asynchronously

Call N+1:
  1. Advance read position by pendingAdvance (SDK done with previous buffer)
  2. Point SDK at new region
  3. Save new pendingAdvance
  4. Return
```

**Why this is safe:**
- Ring buffer free space = `(readPos - writePos - 1) & mask`
- By NOT advancing readPos, producer sees less free space (conservative)
- Producer cannot write into region [readPos, readPos + pendingAdvance)
- SDK safely reads from protected region
- Next call advances, freeing the region for producer

**Contract requirement:** Zero-copy is only safe if the SDK guarantees the buffer
is not accessed after the next `getNewStream()` call. If that guarantee is not
available or cannot be enforced, zero-copy MUST be disabled and the fallback
copy path used unconditionally.

**Reconfigure safety:** While a zero-copy buffer is in use, the ring buffer must
not be resized or cleared. The design below pins the buffer between callbacks
and blocks reconfigure/clear until the next `getNewStream()` call releases it.

## Implementation Details

### Part 1: DirettaRingBuffer Changes

Add two new methods for zero-copy consumer access:

```cpp
// In DirettaRingBuffer class (public section)

/**
 * @brief Get direct read pointer for zero-copy consumer access
 *
 * Returns a pointer to contiguous data in the ring buffer that the
 * consumer can read directly without copying. This is the consumer-side
 * equivalent of getDirectWriteRegion().
 *
 * Thread safety: Safe to call from consumer thread while producer pushes.
 * The returned pointer remains valid until advanceReadPos() is called.
 *
 * @param needed Minimum bytes required
 * @param region Output: pointer to contiguous read region
 * @param available Output: total contiguous bytes available (may exceed needed)
 * @return true if contiguous data >= needed is available, false if data wraps
 */
bool getDirectReadRegion(size_t needed, const uint8_t*& region, size_t& available) const {
    if (size_ == 0) return false;

    size_t wp = writePos_.load(std::memory_order_acquire);
    size_t rp = readPos_.load(std::memory_order_acquire);
    size_t totalAvail = (wp - rp) & mask_;

    // Not enough data at all
    if (totalAvail < needed) return false;

    // Calculate contiguous region from read position
    // Either to write position (if no wrap) or to end of buffer
    size_t toEnd = size_ - rp;
    size_t contiguous = std::min(toEnd, totalAvail);

    if (contiguous >= needed) {
        region = buffer_.data() + rp;
        available = contiguous;
        return true;
    }

    // Data wraps around - caller must use pop() instead
    return false;
}

/**
 * @brief Advance read position after direct read completes
 *
 * Must be called after consumer finishes reading from the region
 * returned by getDirectReadRegion(). The bytes parameter must not
 * exceed what was available.
 *
 * @param bytes Number of bytes consumed
 */
void advanceReadPos(size_t bytes) {
    size_t rp = readPos_.load(std::memory_order_relaxed);
    readPos_.store((rp + bytes) & mask_, std::memory_order_release);
}
```

**Why this is safe:**
- `getDirectReadRegion()` only reads atomic positions (no mutation)
- Producer can continue writing to other parts of the buffer
- `advanceReadPos()` uses release semantics to ensure reads complete first
- Ring buffer's power-of-2 size ensures mask operations are correct

### Part 2: DirettaSync Changes

#### 2.1 New Member Variables

```cpp
// In DirettaSync class (private section)

// Fallback buffer for wraparound cases (pre-allocated at format change)
alignas(64) std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> m_outputBuffer;

// Maximum bytes per buffer (set at format change, used to size fallback buffer)
size_t m_maxBytesPerBuffer{0};

// Tracks fallback buffer lifetime across callbacks
std::atomic<bool> m_outputBufferInUse{false};

// Guards m_outputBuffer resize vs silence fills during reconfigure
std::mutex m_outputBufferMutex;

// ─────────────────────────────────────────────────────────────────
// Dedicated reconfigure silence buffer (IMMUTABLE during reconfigure)
// ─────────────────────────────────────────────────────────────────
// During reconfigure, callbacks that return silence must NOT use
// m_outputBuffer because it may be resized. This dedicated buffer
// is sized to the MAXIMUM possible buffer size for any supported format
// and is resized ONLY outside of reconfigure (in endReconfigure or open).
// Callbacks use this when m_reconfiguring is true.
alignas(64) std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> m_reconfigureSilenceBuffer;

// Maximum buffer size calculation for supported formats:
// - Max sample rate: 384kHz (or DSD512 = 24.576MHz / 16 channels)
// - Max channels: 8
// - Max bytes per sample: 4 (32-bit)
// - Buffer period: 1ms
// - Max bytes = 384 * 8 * 4 = 12,288 bytes/ms
// - Add safety margin: 16KB should cover all practical formats
// - For DSD512 8ch: 24576000 / 8 / 1000 * 8 = 24,576 bytes/ms → use 32KB
static constexpr size_t RECONFIGURE_SILENCE_BUFFER_SIZE = 32768;  // 32KB covers all formats

// Tracks if reconfigure silence buffer needs resize (done outside reconfigure)
size_t m_maxSeenBytesPerBuffer{0};

// ─────────────────────────────────────────────────────────────────
// Deferred advance tracking (CRITICAL for async SDK consumption)
// ─────────────────────────────────────────────────────────────────
// When zero-copy path is used, we defer advancing the read position
// until the NEXT getNewStream() call to ensure SDK has finished
// reading from the ring buffer region.

bool m_pendingZeroCopyAdvance{false};   // True if previous call used zero-copy
size_t m_pendingAdvanceBytes{0};         // Bytes to advance on next call

// Zero-copy lifetime guards
std::atomic<bool> m_zeroCopyInUse{false};   // True between zero-copy return and next callback
std::atomic<bool> m_zeroCopyBlocked{false}; // Block zero-copy during stop/reconfigure/clear

// Worker active flag (MUST be atomic for cross-thread visibility)
std::atomic<bool> m_workerActive{false};

// Reconfigure failure flag - set when beginReconfigure() times out waiting for buffer release
// Caller must check this and abort reconfigure if true
bool m_reconfigureFailed{false};

// Performance monitoring (optional, can be compile-time disabled)
#ifdef DIRETTA_PERF_STATS
std::atomic<uint64_t> m_zeroCopyCount{0};
std::atomic<uint64_t> m_fallbackCount{0};
#endif
```

#### 2.2 Signature Change

```cpp
// Old (SDK 147):
bool getNewStream(DIRETTA::Stream& stream) override;

// New (SDK 148):
bool getNewStream(diretta_stream& stream) override;
```

#### 2.3 Hot Path Implementation

```cpp
// ─────────────────────────────────────────────────────────────────
// RAII guard for m_workerActive (ensures cleanup on ALL exit paths)
// ─────────────────────────────────────────────────────────────────
struct WorkerActiveGuard {
    std::atomic<bool>& flag;
    WorkerActiveGuard(std::atomic<bool>& f) : flag(f) {
        flag.store(true, std::memory_order_release);
    }
    ~WorkerActiveGuard() {
        flag.store(false, std::memory_order_release);
    }
    WorkerActiveGuard(const WorkerActiveGuard&) = delete;
    WorkerActiveGuard& operator=(const WorkerActiveGuard&) = delete;
};

bool DirettaSync::getNewStream(diretta_stream& stream) {
    // RAII guard ensures m_workerActive is cleared on ALL paths
    WorkerActiveGuard workerGuard(m_workerActive);

    // Debug assertion: SDK must call this single-threaded
#ifdef DIRETTA_DEBUG
    static std::atomic_flag inCallback = ATOMIC_FLAG_INIT;
    assert(!inCallback.test_and_set() && "getNewStream() called concurrently!");
    struct CallbackGuard {
        ~CallbackGuard() { inCallback.clear(); }
    } cbGuard;
#endif

    // SDK consumption handshake for fallback buffer
    m_outputBufferInUse = false;

    // ─────────────────────────────────────────────────────────────
    // Generation counter optimization (unchanged from current code)
    // ─────────────────────────────────────────────────────────────
    uint32_t gen = m_consumerStateGen.load(std::memory_order_acquire);
    if (gen != m_cachedConsumerGen) {
        m_cachedBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
        m_cachedFramesRemainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
        m_cachedBytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
        m_cachedConsumerIsDsd = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedSilenceByte = m_ringBuffer.silenceByte();
        m_cachedConsumerGen = gen;
    }

    // Calculate current buffer size (handles fractional sample rates)
    int currentBytesPerBuffer = m_cachedBytesPerBuffer;
    uint32_t remainder = m_cachedFramesRemainder;
    if (remainder != 0) {
        int bytesPerFrame = m_cachedBytesPerFrame;
        uint32_t acc = m_framesPerBufferAccumulator.load(std::memory_order_relaxed);
        acc += remainder;
        if (acc >= 1000) {
            acc -= 1000;
            currentBytesPerBuffer += bytesPerFrame;
        }
        m_framesPerBufferAccumulator.store(acc, std::memory_order_relaxed);
    }
    uint8_t currentSilenceByte = m_cachedSilenceByte;

    // ─────────────────────────────────────────────────────────────
    // Ring access guard - MUST be acquired BEFORE any ring operations
    // ─────────────────────────────────────────────────────────────
    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) {
        // Reconfiguration in progress - ring buffer state is invalid.
        // Do not touch pending state here; config thread owns resets.
        // CRITICAL: Use dedicated reconfigure silence buffer, NOT m_outputBuffer!
        // m_outputBuffer may be resized during reconfigure.
        fillWithReconfigureSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;  // WorkerActiveGuard clears m_workerActive automatically
    }

    // ─────────────────────────────────────────────────────────────
    // STEP 1: Complete deferred advance from PREVIOUS call
    // ─────────────────────────────────────────────────────────────
    // Now safe to advance - we have the ring guard active.
    // SDK has finished reading the buffer from previous call
    // (next callback is the consumption handshake).
    if (m_pendingZeroCopyAdvance) {
        m_ringBuffer.advanceReadPos(m_pendingAdvanceBytes);
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
    }

    // ─────────────────────────────────────────────────────────────
    // Early-exit conditions (silence, not ready, etc.)
    // Note: These paths use fallback buffer, no deferred advance needed
    // ─────────────────────────────────────────────────────────────

    // Shutdown silence
    int silenceRemaining = m_silenceBuffersRemaining.load(std::memory_order_acquire);
    if (silenceRemaining > 0) {
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_silenceBuffersRemaining.fetch_sub(1, std::memory_order_acq_rel);
        return true;  // WorkerActiveGuard handles cleanup
    }

    // Stop requested
    if (m_stopRequested.load(std::memory_order_acquire)) {
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;  // WorkerActiveGuard handles cleanup
    }

    // Prefill not complete
    if (!m_prefillComplete.load(std::memory_order_acquire)) {
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;  // WorkerActiveGuard handles cleanup
    }

    // Post-online stabilization
    if (!m_postOnlineDelayDone.load(std::memory_order_acquire)) {
        int count = m_stabilizationCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= static_cast<int>(DirettaBuffer::POST_ONLINE_SILENCE_BUFFERS)) {
            m_postOnlineDelayDone = true;
            m_stabilizationCount = 0;
        }
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;  // WorkerActiveGuard handles cleanup
    }

    // ─────────────────────────────────────────────────────────────
    // Main audio path: Zero-copy with fallback
    // ─────────────────────────────────────────────────────────────

    size_t avail = m_ringBuffer.getAvailable();

    // Underrun check - ANY underrun uses fallback path (no partial zero-copy)
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;  // WorkerActiveGuard handles cleanup
    }

    // Try zero-copy direct read from ring buffer
    const uint8_t* directPtr;
    size_t contiguousAvail;

    if (!m_zeroCopyBlocked.load(std::memory_order_acquire) &&
        m_ringBuffer.getDirectReadRegion(
            static_cast<size_t>(currentBytesPerBuffer), directPtr, contiguousAvail)) {

        // ─────────────────────────────────────────────────────────
        // ZERO-COPY PATH: Point SDK directly at ring buffer
        // ─────────────────────────────────────────────────────────
        // CRITICAL: Do NOT advance read position now!
        // SDK reads asynchronously after we return.
        // Defer advance to NEXT getNewStream() call.
        // Pin ring buffer until next callback to block reconfigure/clear.
        stream.Data.P = const_cast<void*>(static_cast<const void*>(directPtr));
        stream.Size = static_cast<unsigned long long>(currentBytesPerBuffer);

        // Schedule deferred advance for next call
        m_pendingZeroCopyAdvance = true;
        m_pendingAdvanceBytes = static_cast<size_t>(currentBytesPerBuffer);
        m_zeroCopyInUse = true;

#ifdef DIRETTA_PERF_STATS
        m_zeroCopyCount.fetch_add(1, std::memory_order_relaxed);
#endif
    } else {
        // ─────────────────────────────────────────────────────────
        // FALLBACK PATH: Copy to pre-allocated buffer (data wraps)
        // ─────────────────────────────────────────────────────────
        // pop() copies data AND advances read position atomically.
        // Safe because SDK reads from our buffer, not the ring.
        m_ringBuffer.pop(m_outputBuffer.data(), static_cast<size_t>(currentBytesPerBuffer));
        stream.Data.P = m_outputBuffer.data();
        stream.Size = static_cast<unsigned long long>(currentBytesPerBuffer);
        m_outputBufferInUse = true;

        // No deferred advance needed - pop() already advanced
        // (m_pendingZeroCopyAdvance stays false)

#ifdef DIRETTA_PERF_STATS
        m_fallbackCount.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    // Verbose logging (unchanged)
    if (g_verbose) {
        int count = m_streamCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5 || count % 5000 == 0) {
            // ... existing logging ...
        }
    }

    return true;  // WorkerActiveGuard handles cleanup
}
```

#### 2.4 Helper for Silence (SDK 148 compatible)

```cpp
// New private helper method - used during NORMAL operation (not reconfigure)
void DirettaSync::fillWithSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte) {
    // Protect against concurrent resize; silence path is cold.
    std::lock_guard<std::mutex> lock(m_outputBufferMutex);
    if (m_outputBuffer.size() < bytes) {
        // This should never happen in hot path - buffer sized at format change
        // But handle gracefully just in case
        m_outputBuffer.resize(bytes);
    }
    std::memset(m_outputBuffer.data(), silenceByte, bytes);

    stream.Data.P = m_outputBuffer.data();
    stream.Size = static_cast<unsigned long long>(bytes);
    m_outputBufferInUse = true;
}

// Dedicated helper for reconfigure silence - uses IMMUTABLE buffer
// CRITICAL: This buffer is NEVER resized during reconfigure, so it's safe
// for SDK to read even while m_outputBuffer is being resized.
void DirettaSync::fillWithReconfigureSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte) {
    // m_reconfigureSilenceBuffer is sized to cover all supported formats.
    // If this assert fires, increase RECONFIGURE_SILENCE_BUFFER_SIZE or
    // ensure endReconfigure() grows the buffer based on m_maxSeenBytesPerBuffer.
    assert(bytes <= m_reconfigureSilenceBuffer.size() &&
           "Reconfigure silence buffer too small - increase RECONFIGURE_SILENCE_BUFFER_SIZE");

    std::memset(m_reconfigureSilenceBuffer.data(), silenceByte, bytes);

    stream.Data.P = m_reconfigureSilenceBuffer.data();
    stream.Size = static_cast<unsigned long long>(bytes);
    // Note: m_outputBufferInUse is NOT set - this is a separate buffer
}
```

**Initialization (in constructor):**
```cpp
DirettaSync::DirettaSync(...) {
    // ... existing init ...

    // Pre-allocate reconfigure silence buffer to max size
    m_reconfigureSilenceBuffer.resize(RECONFIGURE_SILENCE_BUFFER_SIZE);
    std::memset(m_reconfigureSilenceBuffer.data(), 0, m_reconfigureSilenceBuffer.size());
}
```

**Dynamic sizing (in endReconfigure - OUTSIDE reconfigure window):**
```cpp
void DirettaSync::endReconfigure() {
    // Grow reconfigure silence buffer if needed (safe - not in reconfigure)
    size_t maxBytes = m_maxBytesPerBuffer;
    if (maxBytes > m_maxSeenBytesPerBuffer) {
        m_maxSeenBytesPerBuffer = maxBytes;
        if (m_reconfigureSilenceBuffer.size() < maxBytes) {
            m_reconfigureSilenceBuffer.resize(maxBytes);
            // Pre-fault pages
            std::memset(m_reconfigureSilenceBuffer.data(), 0, m_reconfigureSilenceBuffer.size());
        }
    }

    m_reconfiguring.store(false, std::memory_order_release);
    m_zeroCopyBlocked = false;
}
```

#### 2.5 Buffer Pre-allocation (Cold Path)

```cpp
void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    // ─────────────────────────────────────────────────────────────────
    // CRITICAL: Check if beginReconfigure() timed out
    // ─────────────────────────────────────────────────────────────────
    // If SDK still holds a buffer pointer, we MUST NOT resize buffers.
    // Abort reconfigure and keep existing configuration.
    if (m_reconfigureFailed) {
        // endReconfigure() will be called by guard destructor
        // m_reconfiguring stays true until explicit recovery
        return;  // Abort - buffers not safe to resize
    }

    // ... existing configuration code ...

    int bytesPerFrame = channels * direttaBps;
    int framesBase = rate / 1000;
    int framesRemainder = rate % 1000;

    // Calculate maximum possible bytes per buffer
    // (base + 1 extra frame for remainder accumulation)
    int maxBytesPerBuffer = (framesBase + 1) * bytesPerFrame;

    // Pre-allocate fallback buffer (COLD PATH - ok to allocate here)
    // SAFE: beginReconfigure() confirmed no in-flight buffer usage
    {
        std::lock_guard<std::mutex> lock(m_outputBufferMutex);
        if (m_outputBuffer.size() < static_cast<size_t>(maxBytesPerBuffer)) {
            m_outputBuffer.resize(static_cast<size_t>(maxBytesPerBuffer));
            // Pre-fault pages to avoid page faults in hot path
            std::memset(m_outputBuffer.data(), 0, m_outputBuffer.size());
        }
    }
    m_maxBytesPerBuffer = static_cast<size_t>(maxBytesPerBuffer);

    // ... rest of existing code ...
}

void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);

    // CRITICAL: Check if beginReconfigure() timed out
    if (m_reconfigureFailed) {
        return;  // Abort - buffers not safe to resize
    }

    // ... existing configuration code ...

    // Pre-allocate fallback buffer for DSD
    // SAFE: beginReconfigure() confirmed no in-flight buffer usage
    size_t maxBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_relaxed) + 64;
    {
        std::lock_guard<std::mutex> lock(m_outputBufferMutex);
        if (m_outputBuffer.size() < maxBytesPerBuffer) {
            m_outputBuffer.resize(maxBytesPerBuffer);
            std::memset(m_outputBuffer.data(), 0x69, m_outputBuffer.size()); // DSD silence
        }
    }
    m_maxBytesPerBuffer = maxBytesPerBuffer;

    // ... rest of existing code ...
}
```

### Part 3: Header Changes

```cpp
// DirettaSync.h changes

// Include for diretta_stream (SDK 148)
#include <diretta_stream.h>  

class DirettaSync : public DIRETTA::Sync {
    // ...

protected:
    // SDK 148 signature
    bool getNewStream(diretta_stream& stream) override;

private:
    // New helper
    void fillWithSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte);

    // Fallback buffer (pre-allocated)
    alignas(64) std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> m_outputBuffer;
    size_t m_maxBytesPerBuffer{0};
    std::atomic<bool> m_outputBufferInUse{false};
    std::mutex m_outputBufferMutex;

    // Deferred advance state (for async SDK consumption)
    bool m_pendingZeroCopyAdvance{false};
    size_t m_pendingAdvanceBytes{0};

    // Zero-copy lifetime guards
    std::atomic<bool> m_zeroCopyInUse{false};
    std::atomic<bool> m_zeroCopyBlocked{false};

#ifdef DIRETTA_PERF_STATS
    std::atomic<uint64_t> m_zeroCopyCount{0};
    std::atomic<uint64_t> m_fallbackCount{0};
#endif

    // ... rest unchanged ...
};
```

### Part 4: State Reset - Comprehensive Coverage

**Critical:** The deferred advance state MUST be reset whenever ring buffer positions become invalid.

#### 4.1 Invariant: Pending State ↔ Ring Buffer Positions

The `m_pendingAdvanceBytes` refers to a specific position in the ring buffer. If the ring
buffer is cleared or resized, that position becomes invalid. **Any code path that invalidates
ring buffer positions MUST also reset pending advance state.**

#### 4.2 Helper Method (Recommended)

To enforce this invariant and prevent future bugs, encapsulate the reset:

```cpp
enum class ZeroCopyWaitResult { Released, TimedOut };

// Block zero-copy and wait for any in-flight callback/buffers to be released.
// Caller is responsible for clearing m_zeroCopyBlocked when safe.
ZeroCopyWaitResult DirettaSync::blockZeroCopyAndWait(std::chrono::milliseconds timeout) {
    // Block new zero-copy attempts first to avoid races.
    m_zeroCopyBlocked.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        bool active = m_workerActive.load(std::memory_order_acquire);
        bool inUse = m_zeroCopyInUse.load(std::memory_order_acquire);
        bool outputInUse = m_outputBufferInUse.load(std::memory_order_acquire);
        if (!active && !inUse && !outputInUse) {
            return ZeroCopyWaitResult::Released;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return ZeroCopyWaitResult::TimedOut;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// Private helper - ensures pending state and ring buffer stay synchronized
void DirettaSync::clearRingBufferAndPendingState() {
    // MUST reset pending BEFORE clear - pending refers to old positions
    // Caller must have blocked zero-copy and waited for release first.
    m_pendingZeroCopyAdvance = false;
    m_pendingAdvanceBytes = 0;
    m_zeroCopyInUse = false;
    m_outputBufferInUse = false;
    m_ringBuffer.clear();
}
```

Note: The helper waits for any in-flight `getNewStream()` that may have observed
`m_zeroCopyBlocked == false` to finish before proceeding. This closes the race where
zero-copy or fallback usage could start after the block is set. For `stopPlayback()`/
`close()`, use a bounded timeout; if it expires (no further callbacks), do not
clear/resize. Instead use an explicit drain/flush (if available), disable zero-copy
for the session, and keep buffers alive until an explicit release point.

#### 4.3 All Reset Points

| Location | Trigger | Notes |
|----------|---------|-------|
| `beginReconfigure()` | Format change | Block zero-copy, wait for zero-copy and fallback release, then resize |
| `close()` | Session end | After waiting for `m_workerActive` |
| `fullReset()` | Full state reset | After waiting for `m_workerActive` |
| `stopPlayback()` | Playback stop | **MUST wait for `m_workerActive`** and buffer release (zero-copy + fallback) |
| `open()` quick resume | Same-format restart | Block zero-copy, wait, then clear |
| `resumePlayback()` | Resume from pause | Block zero-copy, wait, then clear |
| `getNewStream()` | Defensive | If `!ringGuard.active()`, return without touching pending state |

#### 4.4 Implementation

```cpp
void DirettaSync::beginReconfigure() {
    // ─────────────────────────────────────────────────────────────────
    // CRITICAL: Set m_reconfiguring FIRST to close the race window
    // ─────────────────────────────────────────────────────────────────
    // If we wait before setting m_reconfiguring, a callback can land in
    // the gap and hand the SDK a pointer to m_outputBuffer just before
    // we resize it. By setting m_reconfiguring first:
    // - New callbacks immediately see !ringGuard.active()
    // - They use m_reconfigureSilenceBuffer (immutable), not m_outputBuffer
    // - No risk of UAF on m_outputBuffer
    m_reconfiguring.store(true, std::memory_order_release);

    // Block new zero-copy attempts (redundant safety - reconfigure already blocks ring)
    m_zeroCopyBlocked.store(true, std::memory_order_release);

    // Wait for any in-flight callbacks to exit
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Wait for any in-flight zero-copy or fallback buffers to be released
    // (from callbacks that started before m_reconfiguring was set)
    bool released = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (std::chrono::steady_clock::now() < deadline) {
        bool inUse = m_zeroCopyInUse.load(std::memory_order_acquire);
        bool outputInUse = m_outputBufferInUse.load(std::memory_order_acquire);
        if (!inUse && !outputInUse) {
            released = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (!released) {
        // ─────────────────────────────────────────────────────────────────
        // TIMEOUT: SDK may still hold pointer - DO NOT clear flags or resize
        // ─────────────────────────────────────────────────────────────────
        // Keep m_reconfiguring true to block new callbacks from using
        // m_outputBuffer. The caller must handle this failure:
        // - Either abort the reconfigure entirely
        // - Or use a fallback strategy that doesn't resize buffers
        // The in-use flags are preserved to prevent subsequent resize/clear.
        m_reconfigureFailed = true;  // New flag for caller to check
        return;
    }

    m_reconfigureFailed = false;

    // SAFE: Buffers released - reset pending advance state
    m_pendingZeroCopyAdvance = false;
    m_pendingAdvanceBytes = 0;
    m_zeroCopyInUse = false;
    m_outputBufferInUse = false;
}

void DirettaSync::endReconfigure() {
    // Grow reconfigure silence buffer if needed (safe - not in reconfigure)
    // This ensures future reconfigurations have a large enough buffer
    size_t maxBytes = m_maxBytesPerBuffer;
    if (maxBytes > m_maxSeenBytesPerBuffer) {
        m_maxSeenBytesPerBuffer = maxBytes;
        if (m_reconfigureSilenceBuffer.size() < maxBytes) {
            m_reconfigureSilenceBuffer.resize(maxBytes);
            std::memset(m_reconfigureSilenceBuffer.data(), 0, m_reconfigureSilenceBuffer.size());
        }
    }

    m_reconfiguring.store(false, std::memory_order_release);
    m_zeroCopyBlocked = false;
}

void DirettaSync::close() {
    // ... existing close logic ...

    // Wait for worker to stop before resetting state
    while (m_workerActive.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Block zero-copy and wait for any in-flight buffer to be released
    const bool canReset = (blockZeroCopyAndWait(std::chrono::milliseconds(200)) ==
        ZeroCopyWaitResult::Released);
    if (canReset) {
        // Now safe to reset - no concurrent getNewStream()
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        // No further callbacks; keep buffers alive and skip clear/resize.
        m_zeroCopyBlocked = true;
    }

    // ... rest of close ...
}

void DirettaSync::fullReset() {
    // ... wait for m_workerActive (existing code) ...

    // Block zero-copy and wait for any in-flight buffer to be released
    const bool canReset = (blockZeroCopyAndWait(std::chrono::milliseconds(200)) ==
        ZeroCopyWaitResult::Released);
    if (canReset) {
        // Reset deferred advance state
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        // No further callbacks; keep buffers alive and skip clear/resize.
        m_zeroCopyBlocked = true;
    }

    // ... then clear ring buffer ...
}

void DirettaSync::stopPlayback(bool immediate) {
    if (!m_playing) return;

    // ... existing silence/stop logic ...

    // CRITICAL: Wait for worker before resetting non-atomic state
    // Without this, we race with getNewStream() on pending fields
    while (m_workerActive.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Block zero-copy and wait for any in-flight buffer to be released
    const bool canReset = (blockZeroCopyAndWait(std::chrono::milliseconds(200)) ==
        ZeroCopyWaitResult::Released);
    if (canReset) {
        // Now safe to reset
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        // No further callbacks; keep buffers alive and skip clear/resize.
        m_zeroCopyBlocked = true;
    }

    // ... rest of stop ...
}

// In open() - quick resume path (same format):
if (sameFormat) {
    // ... existing silence handling ...

    // MUST reset pending before clearing ring
    const bool canReuse = (blockZeroCopyAndWait(std::chrono::milliseconds(200)) ==
        ZeroCopyWaitResult::Released);
    if (canReuse) {
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_ringBuffer.clear();
        m_zeroCopyBlocked = false;
    } else {
        // Fall back to full open without reusing buffers.
    }

    // ... rest of quick resume ...
}

void DirettaSync::resumePlayback() {
    if (!m_paused) return;

    // ... existing logic ...

    // MUST reset pending before clearing ring
    const bool canReset = (blockZeroCopyAndWait(std::chrono::milliseconds(200)) ==
        ZeroCopyWaitResult::Released);
    if (canReset) {
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_ringBuffer.clear();
        m_zeroCopyBlocked = false;
    } else {
        // Cannot safely clear; leave buffers intact.
    }

    // ... rest of resume ...
}
```

#### 4.5 Why `stopPlayback()` Must Wait

```
Thread A (UPnP):              Thread B (SDK):
─────────────────             ─────────────────
stopPlayback() called
                              getNewStream() running
                              reads m_pendingZeroCopyAdvance (true)
m_pendingZeroCopyAdvance = false  ← RACE!
m_pendingAdvanceBytes = 0     ← RACE!
                              reads m_pendingAdvanceBytes (torn read?)
                              advanceReadPos(garbage)  ← CORRUPTION
```

**Fix:** Wait for `m_workerActive` to clear AND zero-copy release before modifying pending state.

#### 4.6 Defense in Depth Summary

| Layer | Mechanism | Coverage |
|-------|-----------|----------|
| **Encapsulation** | `clearRingBufferAndPendingState()` helper | Prevents forgetting reset |
| **Synchronization** | Wait for `m_workerActive` + buffer release | Prevents races |
| **Lifetime guard** | `m_zeroCopyInUse`/`m_outputBufferInUse` + `blockZeroCopyAndWait(timeout)` | Prevents resize/clear while SDK holds pointer |
| **Defensive** | Check in `getNewStream()` | Catches edge cases |
| **Review** | Audit all `clear()` calls | Ensures completeness |

## Thread Safety Analysis

| Thread | Operations | Safety |
|--------|------------|--------|
| Producer (Audio) | `push()`, `getAvailable()` | Lock-free atomics |
| Consumer (SDK) | `getDirectReadRegion()`, `advanceReadPos()`, `pop()` | Lock-free atomics |
| Config (UPnP) | `configureRing*()`, `close()`, `stopPlayback()` | Mutex + ReconfigureGuard + output buffer mutex |

### Lock Ordering Rule (CRITICAL - Prevents Deadlock)

**Problem:** `m_outputBufferMutex` vs `beginReconfigure()` wait can deadlock:
- `getNewStream()` enters (ringUsers incremented), hits fallback → tries to lock `m_outputBufferMutex`
- `beginReconfigure()` holds `m_outputBufferMutex` and waits for ringUsers == 0
- Deadlock: neither can proceed

**Rule:** `beginReconfigure()` must NEVER hold `m_outputBufferMutex` while waiting for `m_ringUsers`:
1. First: block zero-copy and wait for buffer release (`blockZeroCopyAndWait`)
2. Then: wait for `m_ringUsers == 0`
3. Finally: lock `m_outputBufferMutex` for resize (only after ringUsers is 0)

```cpp
void DirettaSync::beginReconfigure() {
    // Step 1: Block new zero-copy and wait for in-flight buffers
    if (blockZeroCopyAndWait(std::chrono::milliseconds(200)) ==
        ZeroCopyWaitResult::TimedOut) {
        return;  // Cannot safely proceed
    }

    // Step 2: Block ring operations and wait for ringUsers
    m_reconfiguring.store(true, std::memory_order_release);
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Step 3: NOW safe to take output buffer mutex (no ringUsers)
    // Reset pending state - buffer positions will be invalidated
    m_pendingZeroCopyAdvance = false;
    m_pendingAdvanceBytes = 0;
    m_zeroCopyInUse = false;
    m_outputBufferInUse = false;

    // Output buffer resize happens in configureRing*() under m_outputBufferMutex
    // AFTER this function returns
}
```

**Critical invariants maintained:**
1. Only consumer thread calls `advanceReadPos()` - no race on read position
2. Ring buffer power-of-2 size ensures mask arithmetic is atomic-safe
3. `ReconfigureGuard` blocks hot path during format changes
4. Fallback buffer is pinned between callbacks via `m_outputBufferInUse`
5. `m_pendingZeroCopyAdvance` and `m_pendingAdvanceBytes` are owned by consumer thread; other threads
   only touch them after waiting for `m_workerActive` AND buffer release
6. When `m_reconfiguring` is true, `getNewStream()` returns without touching pending state; only the
   config thread clears pending fields
7. `m_zeroCopyInUse` pins the ring buffer between callbacks; reconfigure/clear wait for release
8. `m_outputBuffer` resize/fill is serialized by `m_outputBufferMutex` to avoid UAF during reconfigure

**Deferred advance safety:**
- The pending advance state (`m_pendingZeroCopyAdvance`, `m_pendingAdvanceBytes`) is only
  accessed by the SDK's consumer thread within `getNewStream()`
- Reset happens in `close()`/`stopPlayback()` after waiting for `m_workerActive` and
  `m_zeroCopyInUse`/`m_outputBufferInUse` to clear; if the wait times out, the reset
  is deferred and buffers stay alive
- No synchronization needed for these variables in steady-state; cross-thread resets are
  sequenced by the waits above

## Performance Characteristics

### Zero-Copy Path (expected ~90% of calls)
- **Operations:** 2 atomic loads, pointer arithmetic, 1 atomic store
- **Estimated latency:** 10-30 nanoseconds
- **Memory:** No allocation, no copy

### Fallback Path (expected ~10% of calls)
- **Operations:** Same as zero-copy + `memcpy_audio()`
- **Estimated latency:** 500ns - 5μs depending on buffer size
- **Memory:** Copy to pre-allocated buffer (no allocation)

### When Fallback Occurs
1. Data wraps around ring buffer end
2. More likely at:
   - Low buffer fill levels
   - Just after track transitions
   - With very large buffer requests relative to ring size

## Testing Strategy

### Unit Tests
1. `getDirectReadRegion()` returns correct pointer and size
2. `getDirectReadRegion()` returns false when data wraps
3. `advanceReadPos()` correctly updates position
4. Fallback path produces identical output to direct `pop()`

### Integration Tests
1. Playback with SDK 148 - no crashes
2. Stop→Play→Stop cycles - no corruption
3. Format changes (PCM↔DSD, sample rate changes)
4. Gapless playback between tracks

### Performance Tests
1. Measure zero-copy hit rate (expect >85%)
2. Measure jitter histogram (expect <10μs p99)
3. Compare latency vs SDK 147 baseline

## Migration Checklist

### DirettaRingBuffer
- [ ] Add `getDirectReadRegion()` method
- [ ] Add `advanceReadPos()` method

### DirettaSync - Members
- [ ] Add `m_outputBuffer` (fallback buffer)
- [ ] Add `m_maxBytesPerBuffer`
- [ ] Add `m_pendingZeroCopyAdvance`
- [ ] Add `m_pendingAdvanceBytes`
- [ ] Add `m_zeroCopyInUse`
- [ ] Add `m_zeroCopyBlocked`
- [ ] Add `m_outputBufferInUse`
- [ ] Add `m_outputBufferMutex`
- [ ] Add `m_workerActive` (atomic bool)
- [ ] Add `m_reconfigureFailed` (set on timeout, checked by configureRing*)
- [ ] Add `m_reconfigureSilenceBuffer` (immutable during reconfigure)
- [ ] Add `RECONFIGURE_SILENCE_BUFFER_SIZE` constant (32KB default)
- [ ] Add `m_maxSeenBytesPerBuffer` for dynamic sizing
- [ ] (Optional) Add `m_zeroCopyCount`, `m_fallbackCount` stats

### DirettaSync - Methods
- [ ] Change `getNewStream()` signature to `diretta_stream&`
- [ ] Add `WorkerActiveGuard` RAII class for `m_workerActive`
- [ ] Add debug assertion for single-threaded callback (DIRETTA_DEBUG)
- [ ] Move RingAccessGuard BEFORE deferred advance in `getNewStream()`
- [ ] Do not clear pending state if `!ringGuard.active()`; config thread owns resets
- [ ] Implement deferred advance AFTER guard is active
- [ ] Implement hybrid zero-copy logic (defer advance, don't call immediately)
- [ ] Gate zero-copy on `!m_zeroCopyBlocked`
- [ ] Clear `m_zeroCopyInUse` when deferred advance completes
- [ ] Clear/set `m_outputBufferInUse` around fallback/silence buffers
- [ ] Lock `m_outputBufferMutex` around `fillWithSilence()` resize/fill (AFTER ringUsers == 0)
- [ ] Add `fillWithSilence()` helper (uses m_outputBuffer)
- [ ] Add `fillWithReconfigureSilence()` helper (uses immutable m_reconfigureSilenceBuffer, asserts on size)
- [ ] Update `endReconfigure()` to grow `m_reconfigureSilenceBuffer` if needed
- [ ] Initialize `m_reconfigureSilenceBuffer` in constructor
- [ ] Add `clearRingBufferAndPendingState()` helper (optional but recommended)
- [ ] Update `configureRingPCM()` to pre-allocate fallback buffer
- [ ] Update `configureRingDSD()` to pre-allocate fallback buffer
- [ ] Update destructor to wait for buffer release

### DirettaSync - Pending State Reset Points (ALL required)
- [ ] `beginReconfigure()` - set `m_reconfiguring` FIRST to close race window
- [ ] `beginReconfigure()` - wait for ringUsers AND buffer release AFTER setting m_reconfiguring
- [ ] `beginReconfigure()` - on timeout: set `m_reconfigureFailed`, preserve in-use flags, return early
- [ ] `beginReconfigure()` - on success: reset pending state after waits complete
- [ ] `configureRingPCM()`/`configureRingDSD()` - check `m_reconfigureFailed` and abort if true
- [ ] `close()` - reset AFTER waiting for `m_workerActive`
- [ ] `fullReset()` - reset AFTER waiting for `m_workerActive`
- [ ] `stopPlayback()` - **ADD wait for `m_workerActive`**, then wait for buffer release
- [ ] `open()` quick resume path - block zero-copy, wait, then `m_ringBuffer.clear()`
- [ ] `resumePlayback()` - block zero-copy, wait, then `m_ringBuffer.clear()`
- [ ] `getNewStream()` - return early without touching pending state if `!ringGuard.active()`

### Build/Integration
- [ ] Update header includes for SDK 148
- [ ] Verify SDK 148 header location (`diretta_stream.h`)
- [ ] Test compilation with SDK 148
- [ ] **CRITICAL:** Confirm SDK 148 `close()` guarantees no buffer dereferences afterward

### Testing
- [ ] Test basic playback (no crashes)
- [ ] Test Stop→Play→Stop cycles
- [ ] Test format changes (PCM↔DSD, sample rate)
- [ ] Test gapless playback
- [ ] Verify zero-copy hit rate (expect >85%)

## Shutdown Semantics (CRITICAL)

### Problem: Final Release Point

When `stopPlayback()` or `close()` is called and no further callbacks occur, the SDK may
still hold a pointer to ring buffer or fallback buffer memory. If we destroy `DirettaSync`
while `m_zeroCopyInUse == true`, the SDK may dereference freed memory → crash.

### Solution: Definitive Release Contract

**Requirement:** Confirm SDK 148's guarantee that after `Sync::close()` (or equivalent SDK stop)
returns, no outstanding buffer pointers will be dereferenced.

**Implementation:**

```cpp
DirettaSync::~DirettaSync() {
    // MUST ensure SDK has stopped before destruction
    // Option 1: If SDK guarantees close() releases all pointers
    if (m_zeroCopyInUse || m_outputBufferInUse) {
        // Force wait with longer timeout at destruction
        blockZeroCopyAndWait(std::chrono::seconds(5));
    }

    // Option 2: If SDK does NOT guarantee, must call SDK's explicit drain/stop first
    // (implementation depends on SDK 148 API)
}

void DirettaSync::close() {
    // ... existing close logic ...

    // Wait for worker and in-flight buffers
    while (m_workerActive.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto result = blockZeroCopyAndWait(std::chrono::milliseconds(500));
    if (result == ZeroCopyWaitResult::Released) {
        // Safe: SDK released all pointers
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        // Timeout: SDK may still hold pointer
        // Keep buffers alive, disable zero-copy for safety
        m_zeroCopyBlocked = true;
        // Log warning for debugging
        LOG_WARN("DirettaSync::close(): buffer release timeout, keeping buffers alive");
    }

    // ... rest of close ...
}
```

### Invariant

**After `close()` returns AND SDK confirms streaming stopped:**
- If `ZeroCopyWaitResult::Released`: All buffers are safe to free/resize
- If `ZeroCopyWaitResult::TimedOut`: Buffers must stay alive until explicit release

**At destruction:**
- MUST either wait for release OR confirm SDK has stopped via SDK API

## Rollback Plan

If issues arise:
1. The ring buffer changes are additive (no breaking changes)
2. Can revert `getNewStream()` to always use fallback path
3. Fallback path is functionally equivalent to SDK 147 behavior

## Open Questions

1. **SDK 148 header location:** Where is `diretta_stream.h` / `_diretta_stream` defined?
2. **Performance monitoring:** Enable `DIRETTA_PERF_STATS` by default or compile flag?
3. **SDK close guarantee:** Does `Sync::close()` guarantee no buffer dereferences afterward?
   - If YES: can force-clear `m_zeroCopyInUse` after close
   - If NO: must keep buffers alive until explicit SDK confirmation

## Resolved Questions

1. ~~**Buffer lifetime:** Does SDK read synchronously or asynchronously?~~
   **RESOLVED:** Asynchronous. Documentation states: *"The pointer must remain valid until
   it is dereferenced or this function is called again."* Design uses deferred advance +
   zero-copy pinning; if this contract cannot be enforced, zero-copy is disabled.
