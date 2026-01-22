# SDK 148 Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate DirettaSync from SDK 147 to SDK 148 with hybrid zero-copy buffer design

**Architecture:** Zero-copy direct ring buffer reads (~90% of calls) with pre-allocated fallback buffer for wraparound cases. Deferred read position advance ensures SDK can safely read asynchronously after getNewStream() returns.

**Tech Stack:** C++17, DIRETTA SDK 148, AVX2 SIMD, lock-free atomics

---

## Task 1: Add getDirectReadRegion() to DirettaRingBuffer

**Files:**
- Modify: `src/DirettaRingBuffer.h:553-582` (after pop() method, in public section)
- Test: `src/test_ring_buffer_direct_read.cpp` (new file)

**Step 1: Write the failing test**

Create test file:

```cpp
// src/test_ring_buffer_direct_read.cpp
#include "DirettaRingBuffer.h"
#include <cassert>
#include <iostream>
#include <cstring>

void test_getDirectReadRegion_basic() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);

    // Push some data
    uint8_t input[100];
    for (int i = 0; i < 100; i++) input[i] = static_cast<uint8_t>(i);
    ring.push(input, 100);

    // Get direct read region
    const uint8_t* region;
    size_t available;
    bool success = ring.getDirectReadRegion(50, region, available);

    assert(success && "getDirectReadRegion should succeed with enough data");
    assert(available >= 50 && "Should have at least 50 bytes available");
    assert(region[0] == 0 && "First byte should be 0");
    assert(region[49] == 49 && "50th byte should be 49");

    std::cout << "test_getDirectReadRegion_basic PASSED" << std::endl;
}

void test_getDirectReadRegion_insufficient_data() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);

    uint8_t input[30];
    ring.push(input, 30);

    const uint8_t* region;
    size_t available;
    bool success = ring.getDirectReadRegion(50, region, available);

    assert(!success && "Should fail when not enough data");

    std::cout << "test_getDirectReadRegion_insufficient_data PASSED" << std::endl;
}

void test_getDirectReadRegion_wraparound_returns_false() {
    DirettaRingBuffer ring;
    ring.resize(128, 0x00);  // Small buffer to force wraparound

    // Fill most of buffer
    uint8_t input[100];
    ring.push(input, 100);

    // Pop most data to move read position near end
    uint8_t output[90];
    ring.pop(output, 90);

    // Push more data that wraps around
    ring.push(input, 50);

    // Now data wraps: 10 bytes at end + 40 bytes at start
    // Requesting 30 contiguous should fail (only 10 at end before wrap)
    const uint8_t* region;
    size_t available;
    bool success = ring.getDirectReadRegion(30, region, available);

    // Should return false because contiguous region < 30
    assert(!success && "Should fail when data wraps");

    std::cout << "test_getDirectReadRegion_wraparound_returns_false PASSED" << std::endl;
}

int main() {
    test_getDirectReadRegion_basic();
    test_getDirectReadRegion_insufficient_data();
    test_getDirectReadRegion_wraparound_returns_false();

    std::cout << "\nAll getDirectReadRegion tests PASSED!" << std::endl;
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -mavx2 -O2 -I src src/test_ring_buffer_direct_read.cpp -o test_direct_read && ./test_direct_read`
Expected: Compilation error - `getDirectReadRegion` not found

**Step 3: Implement getDirectReadRegion()**

Add to `src/DirettaRingBuffer.h` after the `pop()` method (around line 582), in public section:

```cpp
    //=========================================================================
    // Direct Read API (zero-copy consumer fast path)
    //=========================================================================

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
```

**Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -mavx2 -O2 -I src src/test_ring_buffer_direct_read.cpp -o test_direct_read && ./test_direct_read`
Expected: All 3 tests PASS

**Step 5: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_ring_buffer_direct_read.cpp
git commit -m "$(cat <<'EOF'
feat(ring): add getDirectReadRegion() for zero-copy consumer access

Part of SDK 148 migration - enables direct pointer access to ring buffer
for zero-copy reads when data is contiguous.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 2: Add advanceReadPos() to DirettaRingBuffer

**Files:**
- Modify: `src/DirettaRingBuffer.h` (after getDirectReadRegion)
- Modify: `src/test_ring_buffer_direct_read.cpp`

**Step 1: Write the failing test**

Add to `src/test_ring_buffer_direct_read.cpp`:

```cpp
void test_advanceReadPos_basic() {
    DirettaRingBuffer ring;
    ring.resize(1024, 0x00);

    uint8_t input[100];
    for (int i = 0; i < 100; i++) input[i] = static_cast<uint8_t>(i);
    ring.push(input, 100);

    assert(ring.getAvailable() == 100);

    // Get region but don't pop
    const uint8_t* region;
    size_t available;
    ring.getDirectReadRegion(50, region, available);

    // Manually advance read position
    ring.advanceReadPos(50);

    assert(ring.getAvailable() == 50 && "Should have 50 bytes left after advance");

    // Verify remaining data is correct
    uint8_t output[50];
    ring.pop(output, 50);
    assert(output[0] == 50 && "First remaining byte should be 50");

    std::cout << "test_advanceReadPos_basic PASSED" << std::endl;
}

void test_advanceReadPos_full_consume() {
    DirettaRingBuffer ring;
    ring.resize(256, 0x00);

    uint8_t input[64];
    ring.push(input, 64);

    const uint8_t* region;
    size_t available;
    ring.getDirectReadRegion(64, region, available);
    ring.advanceReadPos(64);

    assert(ring.getAvailable() == 0 && "Buffer should be empty");

    std::cout << "test_advanceReadPos_full_consume PASSED" << std::endl;
}
```

Update main():

```cpp
int main() {
    test_getDirectReadRegion_basic();
    test_getDirectReadRegion_insufficient_data();
    test_getDirectReadRegion_wraparound_returns_false();
    test_advanceReadPos_basic();
    test_advanceReadPos_full_consume();

    std::cout << "\nAll direct read tests PASSED!" << std::endl;
    return 0;
}
```

**Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -mavx2 -O2 -I src src/test_ring_buffer_direct_read.cpp -o test_direct_read && ./test_direct_read`
Expected: Compilation error - `advanceReadPos` not found

**Step 3: Implement advanceReadPos()**

Add to `src/DirettaRingBuffer.h` after getDirectReadRegion():

```cpp
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

**Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -mavx2 -O2 -I src src/test_ring_buffer_direct_read.cpp -o test_direct_read && ./test_direct_read`
Expected: All 5 tests PASS

**Step 5: Commit**

```bash
git add src/DirettaRingBuffer.h src/test_ring_buffer_direct_read.cpp
git commit -m "$(cat <<'EOF'
feat(ring): add advanceReadPos() for deferred position advance

Completes zero-copy read API. Consumer calls getDirectReadRegion(),
reads data, then calls advanceReadPos() when done. Enables deferred
advance pattern required by SDK 148's async consumption model.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 3: Add SDK 148 member variables to DirettaSync header

**Files:**
- Modify: `src/DirettaSync.h:317-377` (private member section)

**Step 1: Write the failing test**

This is a header-only change. We verify by checking compilation.

**Step 2: Verify current state compiles**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles successfully

**Step 3: Add new member variables**

Add to `src/DirettaSync.h` in the private section (after line 376, before the closing brace):

```cpp
    // ─────────────────────────────────────────────────────────────────────────
    // SDK 148 Zero-Copy Support
    // ─────────────────────────────────────────────────────────────────────────

    // Fallback buffer for wraparound cases (pre-allocated at format change)
    alignas(64) std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> m_outputBuffer;

    // Maximum bytes per buffer (set at format change, used to size fallback buffer)
    size_t m_maxBytesPerBuffer{0};

    // Tracks fallback buffer lifetime across callbacks
    std::atomic<bool> m_outputBufferInUse{false};

    // Guards m_outputBuffer resize vs silence fills during reconfigure
    std::mutex m_outputBufferMutex;

    // ─────────────────────────────────────────────────────────────────────────
    // Dedicated reconfigure silence buffer (IMMUTABLE during reconfigure)
    // ─────────────────────────────────────────────────────────────────────────
    alignas(64) std::vector<uint8_t, AlignedAllocator<uint8_t, 64>> m_reconfigureSilenceBuffer;

    // Maximum buffer size for supported formats (32KB covers all practical formats)
    static constexpr size_t RECONFIGURE_SILENCE_BUFFER_SIZE = 32768;

    // Tracks max seen bytes for dynamic sizing
    size_t m_maxSeenBytesPerBuffer{0};

    // ─────────────────────────────────────────────────────────────────────────
    // Deferred advance tracking (CRITICAL for async SDK consumption)
    // ─────────────────────────────────────────────────────────────────────────
    bool m_pendingZeroCopyAdvance{false};   // True if previous call used zero-copy
    size_t m_pendingAdvanceBytes{0};         // Bytes to advance on next call

    // Zero-copy lifetime guards
    std::atomic<bool> m_zeroCopyInUse{false};   // True between zero-copy return and next callback
    std::atomic<bool> m_zeroCopyBlocked{false}; // Block zero-copy during stop/reconfigure/clear

    // Reconfigure failure flag
    bool m_reconfigureFailed{false};

#ifdef DIRETTA_PERF_STATS
    std::atomic<uint64_t> m_zeroCopyCount{0};
    std::atomic<uint64_t> m_fallbackCount{0};
#endif
```

**Step 4: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles successfully

**Step 5: Commit**

```bash
git add src/DirettaSync.h
git commit -m "$(cat <<'EOF'
feat(sync): add SDK 148 zero-copy member variables

Adds fallback buffer, reconfigure silence buffer, deferred advance
tracking, and zero-copy lifetime guards needed for SDK 148 migration.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 4: Add helper method declarations to DirettaSync header

**Files:**
- Modify: `src/DirettaSync.h:253-276` (private methods section)

**Step 1: Add method declarations**

Add after `void logSinkCapabilities();` (around line 276):

```cpp
    // SDK 148 helpers
    void fillWithSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte);
    void fillWithReconfigureSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte);

    enum class ZeroCopyWaitResult { Released, TimedOut };
    ZeroCopyWaitResult blockZeroCopyAndWait(std::chrono::milliseconds timeout);
    void clearRingBufferAndPendingState();
```

**Step 2: Update getNewStream signature**

Change line 248 from:

```cpp
    bool getNewStream(DIRETTA::Stream& stream) override;
```

To:

```cpp
    bool getNewStream(diretta_stream& stream) override;
```

**Step 3: Add include for diretta_stream**

Add after `#include <Stream.hpp>` (around line 17):

```cpp
// SDK 148: diretta_stream is the base struct
// Note: Adjust path based on actual SDK 148 header location
#include <diretta_stream.h>
```

**Step 4: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: May fail due to missing implementations - that's expected at this stage

**Step 5: Commit**

```bash
git add src/DirettaSync.h
git commit -m "$(cat <<'EOF'
feat(sync): add SDK 148 helper method declarations

Declares fillWithSilence, fillWithReconfigureSilence, blockZeroCopyAndWait,
and clearRingBufferAndPendingState. Changes getNewStream signature to
diretta_stream& for SDK 148 compatibility.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 5: Implement fillWithSilence() helper

**Files:**
- Modify: `src/DirettaSync.cpp` (add after logSinkCapabilities, around line 305)

**Step 1: Write the implementation**

Add after `logSinkCapabilities()`:

```cpp
//=============================================================================
// SDK 148 Helper Methods
//=============================================================================

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
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles (may have warnings about unused methods)

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): implement fillWithSilence() for SDK 148

Uses pre-allocated m_outputBuffer with mutex protection for thread safety.
Sets m_outputBufferInUse flag to track buffer lifetime.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Implement fillWithReconfigureSilence() helper

**Files:**
- Modify: `src/DirettaSync.cpp` (add after fillWithSilence)

**Step 1: Write the implementation**

```cpp
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

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): implement fillWithReconfigureSilence() for SDK 148

Uses dedicated immutable buffer that's never resized during reconfigure,
preventing UAF when callbacks occur during format changes.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 7: Implement blockZeroCopyAndWait() helper

**Files:**
- Modify: `src/DirettaSync.cpp` (add after fillWithReconfigureSilence)

**Step 1: Write the implementation**

```cpp
DirettaSync::ZeroCopyWaitResult DirettaSync::blockZeroCopyAndWait(std::chrono::milliseconds timeout) {
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
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): implement blockZeroCopyAndWait() for SDK 148

Blocks new zero-copy attempts and waits for any in-flight callbacks
and buffers to be released. Returns TimedOut if deadline exceeded.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 8: Implement clearRingBufferAndPendingState() helper

**Files:**
- Modify: `src/DirettaSync.cpp` (add after blockZeroCopyAndWait)

**Step 1: Write the implementation**

```cpp
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

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): implement clearRingBufferAndPendingState() for SDK 148

Encapsulates the invariant that pending advance state must be reset
before clearing ring buffer positions.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Initialize new buffers in constructor

**Files:**
- Modify: `src/DirettaSync.cpp:74-77` (constructor)

**Step 1: Update constructor**

Change from:

```cpp
DirettaSync::DirettaSync() {
    m_ringBuffer.resize(44100 * 2 * 4, 0x00);
    DIRETTA_LOG("Created");
}
```

To:

```cpp
DirettaSync::DirettaSync() {
    m_ringBuffer.resize(44100 * 2 * 4, 0x00);

    // Pre-allocate reconfigure silence buffer to max size
    m_reconfigureSilenceBuffer.resize(RECONFIGURE_SILENCE_BUFFER_SIZE);
    std::memset(m_reconfigureSilenceBuffer.data(), 0, m_reconfigureSilenceBuffer.size());

    DIRETTA_LOG("Created");
}
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): initialize reconfigure silence buffer in constructor

Pre-allocates 32KB buffer to cover all supported audio formats,
avoiding allocations during reconfigure.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 10: Add WorkerActiveGuard RAII class

**Files:**
- Modify: `src/DirettaSync.cpp:13-45` (namespace anonymous section)

**Step 1: Add RAII guard class**

Add after the RingAccessGuard class (around line 45):

```cpp
// RAII guard for m_workerActive (ensures cleanup on ALL exit paths)
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
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): add WorkerActiveGuard RAII class

Ensures m_workerActive is properly cleared on all exit paths from
getNewStream(), preventing worker detection races.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Rewrite getNewStream() for SDK 148

**Files:**
- Modify: `src/DirettaSync.cpp:1051-1156` (getNewStream method)

**Step 1: Replace entire getNewStream implementation**

Replace the existing `getNewStream` method with:

```cpp
bool DirettaSync::getNewStream(diretta_stream& stream) {
    // RAII guard ensures m_workerActive is cleared on ALL paths
    WorkerActiveGuard workerGuard(m_workerActive);

    // SDK consumption handshake for fallback buffer
    m_outputBufferInUse = false;

    // ─────────────────────────────────────────────────────────────────────────
    // Generation counter optimization (unchanged from current code)
    // ─────────────────────────────────────────────────────────────────────────
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

    // ─────────────────────────────────────────────────────────────────────────
    // Ring access guard - MUST be acquired BEFORE any ring operations
    // ─────────────────────────────────────────────────────────────────────────
    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) {
        // Reconfiguration in progress - ring buffer state is invalid.
        // Do not touch pending state here; config thread owns resets.
        // CRITICAL: Use dedicated reconfigure silence buffer, NOT m_outputBuffer!
        fillWithReconfigureSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // STEP 1: Complete deferred advance from PREVIOUS call
    // ─────────────────────────────────────────────────────────────────────────
    if (m_pendingZeroCopyAdvance) {
        m_ringBuffer.advanceReadPos(m_pendingAdvanceBytes);
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Early-exit conditions (silence, not ready, etc.)
    // ─────────────────────────────────────────────────────────────────────────

    // Shutdown silence
    int silenceRemaining = m_silenceBuffersRemaining.load(std::memory_order_acquire);
    if (silenceRemaining > 0) {
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_silenceBuffersRemaining.fetch_sub(1, std::memory_order_acq_rel);
        return true;
    }

    // Stop requested
    if (m_stopRequested.load(std::memory_order_acquire)) {
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;
    }

    // Prefill not complete
    if (!m_prefillComplete.load(std::memory_order_acquire)) {
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;
    }

    // Post-online stabilization
    if (!m_postOnlineDelayDone.load(std::memory_order_acquire)) {
        int count = m_stabilizationCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= static_cast<int>(DirettaBuffer::POST_ONLINE_SILENCE_BUFFERS)) {
            m_postOnlineDelayDone = true;
            m_stabilizationCount = 0;
        }
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Main audio path: Zero-copy with fallback
    // ─────────────────────────────────────────────────────────────────────────

    size_t avail = m_ringBuffer.getAvailable();

    // Underrun check
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        fillWithSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        return true;
    }

    // Try zero-copy direct read from ring buffer
    const uint8_t* directPtr;
    size_t contiguousAvail;

    if (!m_zeroCopyBlocked.load(std::memory_order_acquire) &&
        m_ringBuffer.getDirectReadRegion(
            static_cast<size_t>(currentBytesPerBuffer), directPtr, contiguousAvail)) {

        // ─────────────────────────────────────────────────────────────────────
        // ZERO-COPY PATH: Point SDK directly at ring buffer
        // ─────────────────────────────────────────────────────────────────────
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
        // ─────────────────────────────────────────────────────────────────────
        // FALLBACK PATH: Copy to pre-allocated buffer (data wraps)
        // ─────────────────────────────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lock(m_outputBufferMutex);
            if (m_outputBuffer.size() < static_cast<size_t>(currentBytesPerBuffer)) {
                m_outputBuffer.resize(static_cast<size_t>(currentBytesPerBuffer));
            }
        }
        m_ringBuffer.pop(m_outputBuffer.data(), static_cast<size_t>(currentBytesPerBuffer));
        stream.Data.P = m_outputBuffer.data();
        stream.Size = static_cast<unsigned long long>(currentBytesPerBuffer);
        m_outputBufferInUse = true;

#ifdef DIRETTA_PERF_STATS
        m_fallbackCount.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    // Verbose logging
    if (g_verbose) {
        int count = m_streamCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 5 || count % 5000 == 0) {
            size_t ringSize = m_ringBuffer.size();
            float fillPct = (ringSize > 0) ? (100.0f * avail / ringSize) : 0.0f;
            DIRETTA_LOG("getNewStream #" << count << " bpb=" << currentBytesPerBuffer
                        << " avail=" << avail << " (" << std::fixed << std::setprecision(1)
                        << fillPct << "%) " << (m_cachedConsumerIsDsd ? "[DSD]" : "[PCM]"));
        }
    }

    return true;
}
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
feat(sync): rewrite getNewStream() for SDK 148 zero-copy

- Changes signature from DIRETTA::Stream& to diretta_stream&
- Implements hybrid zero-copy with deferred read position advance
- Uses WorkerActiveGuard for proper cleanup on all exit paths
- Falls back to pre-allocated buffer when data wraps
- Uses dedicated silence buffer during reconfigure

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 12: Update beginReconfigure() for buffer safety

**Files:**
- Modify: `src/DirettaSync.cpp:1190-1195` (beginReconfigure method)

**Step 1: Replace beginReconfigure implementation**

Replace:

```cpp
void DirettaSync::beginReconfigure() {
    m_reconfiguring.store(true, std::memory_order_release);
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }
}
```

With:

```cpp
void DirettaSync::beginReconfigure() {
    // ─────────────────────────────────────────────────────────────────────────
    // CRITICAL: Set m_reconfiguring FIRST to close the race window
    // ─────────────────────────────────────────────────────────────────────────
    m_reconfiguring.store(true, std::memory_order_release);

    // Block new zero-copy attempts
    m_zeroCopyBlocked.store(true, std::memory_order_release);

    // Wait for any in-flight callbacks to exit
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Wait for any in-flight zero-copy or fallback buffers to be released
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
        // Timeout: SDK may still hold pointer - DO NOT clear flags or resize
        m_reconfigureFailed = true;
        return;
    }

    m_reconfigureFailed = false;

    // SAFE: Buffers released - reset pending advance state
    m_pendingZeroCopyAdvance = false;
    m_pendingAdvanceBytes = 0;
    m_zeroCopyInUse = false;
    m_outputBufferInUse = false;
}
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update beginReconfigure() for SDK 148 buffer safety

- Blocks zero-copy and waits for buffer release before resize
- Sets m_reconfigureFailed on timeout to prevent unsafe operations
- Resets pending advance state only when buffers are released

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 13: Update endReconfigure() to grow silence buffer

**Files:**
- Modify: `src/DirettaSync.cpp:1197-1199` (endReconfigure method)

**Step 1: Replace endReconfigure implementation**

Replace:

```cpp
void DirettaSync::endReconfigure() {
    m_reconfiguring.store(false, std::memory_order_release);
}
```

With:

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

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update endReconfigure() to grow silence buffer and unblock

Dynamically grows reconfigure silence buffer based on max seen format,
and clears m_zeroCopyBlocked to re-enable zero-copy path.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 14: Update configureRingPCM() to pre-allocate fallback buffer

**Files:**
- Modify: `src/DirettaSync.cpp:782-823` (configureRingPCM method)

**Step 1: Add reconfigureFailed check and buffer pre-allocation**

After `ReconfigureGuard guard(*this);` (around line 784), add:

```cpp
    // CRITICAL: Check if beginReconfigure() timed out
    if (m_reconfigureFailed) {
        return;  // Abort - buffers not safe to resize
    }
```

Before the line `DIRETTA_LOG("Ring PCM: ..."`, add:

```cpp
    // Pre-allocate fallback buffer (COLD PATH - ok to allocate here)
    int maxBytesPerBuffer = (framesBase + 1) * bytesPerFrame;
    {
        std::lock_guard<std::mutex> lock(m_outputBufferMutex);
        if (m_outputBuffer.size() < static_cast<size_t>(maxBytesPerBuffer)) {
            m_outputBuffer.resize(static_cast<size_t>(maxBytesPerBuffer));
            // Pre-fault pages to avoid page faults in hot path
            std::memset(m_outputBuffer.data(), 0, m_outputBuffer.size());
        }
    }
    m_maxBytesPerBuffer = static_cast<size_t>(maxBytesPerBuffer);
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): pre-allocate fallback buffer in configureRingPCM()

Adds reconfigureFailed check and pre-allocates m_outputBuffer for
the maximum possible buffer size to avoid hot path allocations.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 15: Update configureRingDSD() to pre-allocate fallback buffer

**Files:**
- Modify: `src/DirettaSync.cpp:825-860` (configureRingDSD method)

**Step 1: Add reconfigureFailed check and buffer pre-allocation**

After `ReconfigureGuard guard(*this);` (around line 827), add:

```cpp
    // CRITICAL: Check if beginReconfigure() timed out
    if (m_reconfigureFailed) {
        return;  // Abort - buffers not safe to resize
    }
```

Before the line `DIRETTA_LOG("Ring DSD: ..."`, add:

```cpp
    // Pre-allocate fallback buffer for DSD
    size_t maxBytesPerBuffer = bytesPerBuffer + 64;  // Add safety margin
    {
        std::lock_guard<std::mutex> lock(m_outputBufferMutex);
        if (m_outputBuffer.size() < maxBytesPerBuffer) {
            m_outputBuffer.resize(maxBytesPerBuffer);
            std::memset(m_outputBuffer.data(), 0x69, m_outputBuffer.size()); // DSD silence
        }
    }
    m_maxBytesPerBuffer = maxBytesPerBuffer;
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): pre-allocate fallback buffer in configureRingDSD()

Adds reconfigureFailed check and pre-allocates m_outputBuffer with
DSD silence (0x69) to avoid hot path allocations.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 16: Update close() with buffer wait

**Files:**
- Modify: `src/DirettaSync.cpp:496-532` (close method)

**Step 1: Add buffer wait before state reset**

Replace the wait loop section with enhanced version. After `disconnect(true);` add:

```cpp
    // Wait for worker and in-flight buffers
    int waitCount = 0;
    while (m_workerActive.load() && waitCount < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        waitCount++;
    }

    // Block zero-copy and wait for buffer release
    auto result = blockZeroCopyAndWait(std::chrono::milliseconds(200));
    if (result == ZeroCopyWaitResult::Released) {
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        // Timeout: keep buffers alive
        m_zeroCopyBlocked = true;
        DIRETTA_LOG("close(): buffer release timeout, keeping buffers alive");
    }
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update close() to wait for buffer release

Waits for zero-copy and fallback buffers to be released before
clearing state. Keeps buffers alive on timeout for safety.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 17: Update fullReset() with buffer wait

**Files:**
- Modify: `src/DirettaSync.cpp:611-647` (fullReset method)

**Step 1: Add buffer wait before ring clear**

After the worker wait loop, add:

```cpp
    // Block zero-copy and wait for buffer release
    auto result = blockZeroCopyAndWait(std::chrono::milliseconds(200));
    if (result == ZeroCopyWaitResult::Released) {
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        m_zeroCopyBlocked = true;
    }
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update fullReset() to wait for buffer release

Resets pending advance state only after confirming buffers are released.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 18: Update stopPlayback() with buffer wait

**Files:**
- Modify: `src/DirettaSync.cpp:881-903` (stopPlayback method)

**Step 1: Add buffer wait before state reset**

After `stop();` and before setting `m_playing = false;`, add:

```cpp
    // CRITICAL: Wait for worker and buffers before resetting state
    while (m_workerActive.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto result = blockZeroCopyAndWait(std::chrono::milliseconds(200));
    if (result == ZeroCopyWaitResult::Released) {
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_zeroCopyBlocked = false;
    } else {
        m_zeroCopyBlocked = true;
    }
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update stopPlayback() to wait for buffer release

Prevents race between stopPlayback() and getNewStream() by waiting
for worker exit and buffer release before modifying pending state.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 19: Update resumePlayback() with buffer wait

**Files:**
- Modify: `src/DirettaSync.cpp:920-939` (resumePlayback method)

**Step 1: Add buffer wait before ring clear**

Before `m_ringBuffer.clear();`, add:

```cpp
    // Block zero-copy and wait for buffer release before clearing
    auto result = blockZeroCopyAndWait(std::chrono::milliseconds(200));
    if (result == ZeroCopyWaitResult::Released) {
        m_pendingZeroCopyAdvance = false;
        m_pendingAdvanceBytes = 0;
        m_zeroCopyInUse = false;
        m_outputBufferInUse = false;
        m_ringBuffer.clear();
        m_zeroCopyBlocked = false;
    } else {
        // Cannot safely clear; leave buffers intact
        DIRETTA_LOG("resumePlayback: buffer release timeout, skipping clear");
    }
```

And remove the original `m_ringBuffer.clear();` line.

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update resumePlayback() to wait for buffer release

Resets pending state and clears ring only after confirming buffers
are released to prevent race conditions.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 20: Update open() quick resume path with buffer wait

**Files:**
- Modify: `src/DirettaSync.cpp:341-366` (open method, sameFormat block)

**Step 1: Add buffer wait before ring clear**

Replace the quick resume block with:

```cpp
        if (sameFormat) {
            std::cout << "[DirettaSync] Same format - quick resume (no setSink)" << std::endl;

            // Send silence before transition to flush Diretta pipeline
            if (m_isDsdMode.load(std::memory_order_acquire)) {
                requestShutdownSilence(30);
                auto start = std::chrono::steady_clock::now();
                while (m_silenceBuffersRemaining.load(std::memory_order_acquire) > 0) {
                    if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(100)) break;
                    std::this_thread::yield();
                }
            }

            // Block zero-copy and wait for buffer release before clearing
            auto result = blockZeroCopyAndWait(std::chrono::milliseconds(200));
            if (result == ZeroCopyWaitResult::Released) {
                m_pendingZeroCopyAdvance = false;
                m_pendingAdvanceBytes = 0;
                m_zeroCopyInUse = false;
                m_outputBufferInUse = false;
                m_ringBuffer.clear();
                m_zeroCopyBlocked = false;
            } else {
                // Fall back to full open without reusing buffers
                std::cout << "[DirettaSync] Buffer release timeout, falling back to full open" << std::endl;
                // Continue to full open path below
                goto full_open;
            }

            m_prefillComplete = false;
            m_postOnlineDelayDone = false;
            m_stabilizationCount = 0;
            m_stopRequested = false;
            m_draining = false;
            m_silenceBuffersRemaining = 0;
            play();
            m_playing = true;
            m_paused = false;
            std::cout << "[DirettaSync] ========== OPEN COMPLETE (quick) ==========" << std::endl;
            return true;
        }
```

Add a label before fullReset() call:

```cpp
full_open:
    // Full reset for first open or after format change reopen
    if (needFullConnect) {
        fullReset();
    }
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update open() quick resume to wait for buffer release

Falls back to full open path if buffer release times out, preventing
UAF during same-format track transitions.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 21: Update destructor to wait for buffer release

**Files:**
- Modify: `src/DirettaSync.cpp:79-82` (destructor)

**Step 1: Add buffer wait in destructor**

Replace:

```cpp
DirettaSync::~DirettaSync() {
    disable();
    DIRETTA_LOG("Destroyed");
}
```

With:

```cpp
DirettaSync::~DirettaSync() {
    disable();

    // Ensure SDK has released all buffer pointers before destruction
    if (m_zeroCopyInUse.load(std::memory_order_acquire) ||
        m_outputBufferInUse.load(std::memory_order_acquire)) {
        blockZeroCopyAndWait(std::chrono::seconds(5));
    }

    DIRETTA_LOG("Destroyed");
}
```

**Step 2: Verify compilation**

Run: `g++ -std=c++17 -mavx2 -O2 -c src/DirettaSync.cpp -I src -I /path/to/diretta/sdk`
Expected: Compiles

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
fix(sync): update destructor to wait for buffer release

Prevents UAF by waiting up to 5 seconds for SDK to release all
buffer pointers before destroying DirettaSync object.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 22: Full build and basic test

**Files:**
- All modified files

**Step 1: Full compilation**

Run: `make clean && make`
Expected: Build succeeds with no errors

**Step 2: Run basic test**

Run: `./diretta_renderer --verbose` (or equivalent)
Expected: Application starts without crash

**Step 3: Final commit**

```bash
git add -A
git commit -m "$(cat <<'EOF'
feat: complete SDK 148 migration with hybrid zero-copy

Summary:
- Added getDirectReadRegion() and advanceReadPos() to DirettaRingBuffer
- Changed getNewStream() signature to diretta_stream& for SDK 148
- Implemented hybrid zero-copy with deferred read position advance
- Added pre-allocated fallback buffer for wraparound cases
- Added dedicated reconfigure silence buffer
- Updated all state reset points to wait for buffer release
- Added buffer lifetime guards (m_zeroCopyInUse, m_outputBufferInUse)

Expected performance: ~90% zero-copy, <10us p99 jitter

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 23: Integration testing

**Files:**
- None (testing only)

**Step 1: Test basic PCM playback**

Play a PCM file (44.1kHz/16-bit or 192kHz/24-bit)
Expected: Audio plays without crashes or glitches

**Step 2: Test Stop→Play→Stop cycles**

Rapidly start/stop playback 10 times
Expected: No crashes, no audio corruption

**Step 3: Test format changes**

Play PCM, then DSD, then PCM again
Expected: Clean transitions without crashes

**Step 4: Test gapless playback**

Queue multiple tracks of same format
Expected: Seamless playback without gaps

**Step 5: Commit test results**

```bash
git add -A
git commit -m "$(cat <<'EOF'
test: verify SDK 148 migration integration

Tested:
- Basic PCM playback (44.1/16, 192/24)
- Stop/Play cycles (10x)
- Format changes (PCM↔DSD)
- Gapless playback

All tests passed.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Summary

This plan implements the SDK 148 migration in 23 tasks:

| Task | Description | Risk |
|------|-------------|------|
| 1-2 | Ring buffer direct read API | Low |
| 3-4 | Header declarations | Low |
| 5-9 | Helper methods | Low |
| 10 | WorkerActiveGuard RAII | Low |
| 11 | getNewStream() rewrite | **High** |
| 12-13 | Reconfigure safety | Medium |
| 14-15 | Buffer pre-allocation | Low |
| 16-21 | State reset points | Medium |
| 22-23 | Build and test | Low |

**Critical path:** Task 11 (getNewStream rewrite) is the highest risk change. If issues arise, the fallback path provides identical behavior to SDK 147.

**Rollback:** Revert to always using fallback path by setting `m_zeroCopyBlocked = true` in constructor.
