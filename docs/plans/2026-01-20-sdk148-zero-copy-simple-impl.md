# SDK 148 Zero-Copy Migration Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Migrate DirettaSync to SDK 148's `getNewStream(diretta_stream&)` signature with zero-copy ring buffer access.

**Architecture:** Zero-copy path points SDK directly at ring buffer memory. Deferred read position advance ensures SDK finishes reading before buffer is reused. If data wraps, return silence and advance to realign.

**Tech Stack:** C++17, atomic operations, DIRETTA SDK 148

---

## Task 1: Add Ring Buffer Zero-Copy Methods

**Files:**
- Modify: `src/DirettaRingBuffer.h:17-21` (add constant)
- Modify: `src/DirettaRingBuffer.h:150-178` (add methods after getDirectWriteRegion)

**Step 1: Add RING_BUFFER_SIZE constant**

In `src/DirettaRingBuffer.h`, add after line 21 (after includes, before AlignedAllocator):

```cpp
// Maximum ring buffer size for zero-copy SDK 148 support
static constexpr size_t RING_BUFFER_SIZE = 1024 * 1024;  // 1MB
```

**Step 2: Add getDirectReadRegion method**

In `src/DirettaRingBuffer.h`, add after `commitDirectWrite()` (around line 188):

```cpp
    //=========================================================================
    // Direct Read API (zero-copy consumer path for SDK 148)
    //=========================================================================

    /**
     * @brief Get direct read pointer for zero-copy consumer access
     *
     * Returns a pointer to contiguous data in the ring buffer that the
     * consumer can read directly without copying.
     *
     * @param needed Minimum bytes required
     * @param region Output: pointer to contiguous read region
     * @param avail Output: contiguous bytes available (may exceed needed)
     * @return true if contiguous data >= needed is available, false if wrap or underrun
     */
    bool getDirectReadRegion(size_t needed, const uint8_t*& region, size_t& avail) const {
        if (size_ == 0) return false;

        size_t wp = writePos_.load(std::memory_order_acquire);
        size_t rp = readPos_.load(std::memory_order_relaxed);
        size_t total = (wp - rp) & mask_;

        if (total < needed) return false;  // Underrun

        size_t contiguous = std::min(size_ - rp, total);
        if (contiguous < needed) return false;  // Wrap

        region = buffer_.data() + rp;
        avail = contiguous;
        return true;
    }

    /**
     * @brief Advance read position after direct read completes
     *
     * Must be called after consumer finishes reading from the region
     * returned by getDirectReadRegion().
     *
     * @param bytes Number of bytes consumed
     */
    void advanceReadPos(size_t bytes) {
        size_t rp = readPos_.load(std::memory_order_relaxed);
        readPos_.store((rp + bytes) & mask_, std::memory_order_release);
    }
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors related to DirettaRingBuffer

**Step 4: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "feat(ring): add zero-copy read API for SDK 148

- Add RING_BUFFER_SIZE constant (1MB)
- Add getDirectReadRegion() for contiguous read access
- Add advanceReadPos() for deferred position update"
```

---

## Task 2: Add DirettaSync Member Variables

**Files:**
- Modify: `src/DirettaSync.h:317-325` (add members after m_workerActive)

**Step 1: Add new member variables**

In `src/DirettaSync.h`, add after line 323 (`std::atomic<uint32_t> m_underrunCount{0};`):

```cpp
    // SDK 148 zero-copy state
    std::atomic<size_t> m_pendingAdvance{0};  // Deferred read position advance

    // Silence buffer - pre-allocated on format change
    alignas(64) std::vector<uint8_t> m_silenceBuffer;
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaSync.h
git commit -m "feat(sync): add SDK 148 zero-copy member variables

- Add atomic m_pendingAdvance for deferred read position
- Add aligned m_silenceBuffer for silence output"
```

---

## Task 3: Add waitForPendingRelease Helper

**Files:**
- Modify: `src/DirettaSync.h:269-276` (add declaration after beginReconfigure/endReconfigure)
- Modify: `src/DirettaSync.cpp:1190-1199` (add implementation after endReconfigure)

**Step 1: Add declaration in header**

In `src/DirettaSync.h`, add after line 270 (`void endReconfigure();`):

```cpp
    bool waitForPendingRelease(std::chrono::milliseconds timeout);
```

**Step 2: Add implementation**

In `src/DirettaSync.cpp`, add after `endReconfigure()` (after line 1199):

```cpp
bool DirettaSync::waitForPendingRelease(std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_pendingAdvance.load(std::memory_order_acquire) > 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;  // Timeout - SDK may still hold pointer
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 4: Commit**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp
git commit -m "feat(sync): add waitForPendingRelease helper

Spin-waits for SDK to release buffer (m_pendingAdvance == 0)
with configurable timeout for safe buffer clear/resize."
```

---

## Task 4: Pre-allocate Silence Buffer in configureRingPCM

**Files:**
- Modify: `src/DirettaSync.cpp:782-823` (configureRingPCM)

**Step 1: Add silence buffer allocation**

In `src/DirettaSync.cpp`, in `configureRingPCM()`, add after line 813 (`m_prefillComplete = false;`):

```cpp
    // Pre-allocate silence buffer for max possible bytes per callback
    size_t maxSilenceBytes = static_cast<size_t>((framesBase + 1) * bytesPerFrame);
    if (m_silenceBuffer.size() < maxSilenceBytes) {
        m_silenceBuffer.resize(maxSilenceBytes);
    }
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "feat(sync): pre-allocate silence buffer in configureRingPCM"
```

---

## Task 5: Pre-allocate Silence Buffer in configureRingDSD

**Files:**
- Modify: `src/DirettaSync.cpp:825-860` (configureRingDSD)

**Step 1: Add silence buffer allocation**

In `src/DirettaSync.cpp`, in `configureRingDSD()`, add after line 852 (`m_prefillComplete = false;`):

```cpp
    // Pre-allocate silence buffer for DSD
    size_t maxSilenceBytes = static_cast<size_t>(bytesPerBuffer) + 64;
    if (m_silenceBuffer.size() < maxSilenceBytes) {
        m_silenceBuffer.resize(maxSilenceBytes);
    }
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "feat(sync): pre-allocate silence buffer in configureRingDSD"
```

---

## Task 6: Add fillSilence Helper

**Files:**
- Modify: `src/DirettaSync.h:269-276` (add declaration)
- Modify: `src/DirettaSync.cpp` (add implementation before getNewStream)

**Step 1: Add declaration**

In `src/DirettaSync.h`, add after `waitForPendingRelease` declaration:

```cpp
    void fillSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte);
```

**Step 2: Add include for diretta_stream**

In `src/DirettaSync.h`, add after line 13 (`#include "DirettaRingBuffer.h"`):

```cpp
extern "C" {
    #include "diretta_stream.h"
}
```

**Step 3: Add implementation**

In `src/DirettaSync.cpp`, add before `getNewStream()` (before line 1051):

```cpp
void DirettaSync::fillSilence(diretta_stream& stream, size_t bytes, uint8_t silenceByte) {
    // m_silenceBuffer pre-allocated on format change, always large enough
    std::memset(m_silenceBuffer.data(), silenceByte, bytes);
    stream.Data.P = m_silenceBuffer.data();
    stream.Size = static_cast<unsigned long long>(bytes);
}
```

**Step 4: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 5: Commit**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp
git commit -m "feat(sync): add fillSilence helper for SDK 148

Uses pre-allocated m_silenceBuffer to provide silence output
without allocation in hot path."
```

---

## Task 7: Update getNewStream Signature and Implementation

**Files:**
- Modify: `src/DirettaSync.h:248` (change signature)
- Modify: `src/DirettaSync.cpp:1051-1156` (rewrite getNewStream)

**Step 1: Update header signature**

In `src/DirettaSync.h`, change line 248 from:

```cpp
    bool getNewStream(DIRETTA::Stream& stream) override;
```

to:

```cpp
    bool getNewStream(diretta_stream& stream) override;
```

**Step 2: Rewrite getNewStream implementation**

In `src/DirettaSync.cpp`, replace the entire `getNewStream` function (lines 1051-1156) with:

```cpp
bool DirettaSync::getNewStream(diretta_stream& stream) {
    m_workerActive = true;

    // Complete previous deferred advance (atomic load)
    size_t pending = m_pendingAdvance.load(std::memory_order_acquire);
    if (pending > 0) {
        m_ringBuffer.advanceReadPos(pending);
        m_pendingAdvance.store(0, std::memory_order_release);
    }

    // Generation counter optimization: single atomic load in common case
    uint32_t gen = m_consumerStateGen.load(std::memory_order_acquire);
    if (gen != m_cachedConsumerGen) {
        // Cold path: reload stable configuration (only on format change)
        m_cachedBytesPerBuffer = m_bytesPerBuffer.load(std::memory_order_acquire);
        m_cachedFramesRemainder = m_framesPerBufferRemainder.load(std::memory_order_acquire);
        m_cachedBytesPerFrame = m_bytesPerFrame.load(std::memory_order_acquire);
        m_cachedConsumerIsDsd = m_isDsdMode.load(std::memory_order_acquire);
        m_cachedSilenceByte = m_ringBuffer.silenceByte();
        m_cachedConsumerGen = gen;
    }

    // Hot path: use cached values
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

    RingAccessGuard ringGuard(m_ringUsers, m_reconfiguring);
    if (!ringGuard.active()) {
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_workerActive = false;
        return true;
    }

    // Shutdown silence
    int silenceRemaining = m_silenceBuffersRemaining.load(std::memory_order_acquire);
    if (silenceRemaining > 0) {
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_silenceBuffersRemaining.fetch_sub(1, std::memory_order_acq_rel);
        m_workerActive = false;
        return true;
    }

    // Stop requested
    if (m_stopRequested.load(std::memory_order_acquire)) {
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_workerActive = false;
        return true;
    }

    // Prefill not complete
    if (!m_prefillComplete.load(std::memory_order_acquire)) {
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_workerActive = false;
        return true;
    }

    // Post-online stabilization
    if (!m_postOnlineDelayDone.load(std::memory_order_acquire)) {
        int count = m_stabilizationCount.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (count >= static_cast<int>(DirettaBuffer::POST_ONLINE_SILENCE_BUFFERS)) {
            m_postOnlineDelayDone = true;
            m_stabilizationCount = 0;
            DIRETTA_LOG("Post-online stabilization complete");
        }
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_workerActive = false;
        return true;
    }

    int count = m_streamCount.fetch_add(1, std::memory_order_relaxed) + 1;

    // Check available data first (distinguishes underrun vs wrap)
    size_t avail = m_ringBuffer.getAvailable();
    if (avail < static_cast<size_t>(currentBytesPerBuffer)) {
        // True underrun - don't advance, just silence
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
        m_workerActive = false;
        return true;
    }

    if (g_verbose && (count <= 5 || count % 5000 == 0)) {
        size_t currentRingSize = m_ringBuffer.size();
        float fillPct = (currentRingSize > 0) ? (100.0f * avail / currentRingSize) : 0.0f;
        bool currentIsDsd = m_cachedConsumerIsDsd;
        DIRETTA_LOG("getNewStream #" << count << " bpb=" << currentBytesPerBuffer
                    << " avail=" << avail << " (" << std::fixed << std::setprecision(1)
                    << fillPct << "%) " << (currentIsDsd ? "[DSD]" : "[PCM]"));
    }

    // Zero-copy path
    const uint8_t* ptr;
    size_t contiguous;
    if (m_ringBuffer.getDirectReadRegion(static_cast<size_t>(currentBytesPerBuffer), ptr, contiguous)) {
        // Point SDK directly at ring buffer
        stream.Data.P = const_cast<void*>(static_cast<const void*>(ptr));
        stream.Size = static_cast<unsigned long long>(currentBytesPerBuffer);
        // Defer advance to next callback (SDK reads asynchronously)
        m_pendingAdvance.store(static_cast<size_t>(currentBytesPerBuffer), std::memory_order_release);
    } else {
        // Wrap (data available but not contiguous) - advance to realign
        m_ringBuffer.advanceReadPos(static_cast<size_t>(currentBytesPerBuffer));
        fillSilence(stream, currentBytesPerBuffer, currentSilenceByte);
    }

    m_workerActive = false;
    return true;
}
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 4: Commit**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp
git commit -m "feat(sync): implement SDK 148 zero-copy getNewStream

- Change signature to diretta_stream&
- Deferred read position advance for async SDK consumption
- Zero-copy path points SDK directly at ring buffer
- Wrap case advances to realign and returns silence
- Underrun case returns silence without advancing"
```

---

## Task 8: Update beginReconfigure for Safe Buffer Pinning

**Files:**
- Modify: `src/DirettaSync.h:269` (change return type)
- Modify: `src/DirettaSync.cpp:1190-1195` (update implementation)

**Step 1: Update header declaration**

In `src/DirettaSync.h`, change `beginReconfigure` declaration to:

```cpp
    bool beginReconfigure();
```

**Step 2: Update implementation**

In `src/DirettaSync.cpp`, replace `beginReconfigure()` (around line 1190-1195) with:

```cpp
bool DirettaSync::beginReconfigure() {
    // Set m_reconfiguring FIRST to block new callbacks from using ring
    m_reconfiguring.store(true, std::memory_order_release);

    // Wait for ringUsers to drain (existing logic)
    while (m_ringUsers.load(std::memory_order_acquire) > 0) {
        std::this_thread::yield();
    }

    // Wait for SDK to release buffer
    if (!waitForPendingRelease(std::chrono::milliseconds(200))) {
        // Timeout: cannot safely resize
        // Clear m_reconfiguring so callbacks can proceed
        m_reconfiguring.store(false, std::memory_order_release);
        return false;  // Caller must abort reconfigure
    }

    return true;  // Safe to resize
}
```

**Step 3: Update ReconfigureGuard to handle bool return**

In `src/DirettaSync.h`, update ReconfigureGuard class (around line 278-287):

```cpp
    class ReconfigureGuard {
    public:
        explicit ReconfigureGuard(DirettaSync& sync) : sync_(sync), active_(sync_.beginReconfigure()) {}
        ~ReconfigureGuard() { if (active_) sync_.endReconfigure(); }
        ReconfigureGuard(const ReconfigureGuard&) = delete;
        ReconfigureGuard& operator=(const ReconfigureGuard&) = delete;
        bool active() const { return active_; }

    private:
        DirettaSync& sync_;
        bool active_;
    };
```

**Step 4: Update configureRingPCM to check ReconfigureGuard**

In `src/DirettaSync.cpp`, update `configureRingPCM` (around line 782-784):

```cpp
void DirettaSync::configureRingPCM(int rate, int channels, int direttaBps, int inputBps) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);
    if (!guard.active()) {
        DIRETTA_LOG("configureRingPCM aborted - SDK still holds buffer");
        return;
    }
```

**Step 5: Update configureRingDSD to check ReconfigureGuard**

In `src/DirettaSync.cpp`, update `configureRingDSD` (around line 825-827):

```cpp
void DirettaSync::configureRingDSD(uint32_t byteRate, int channels) {
    std::lock_guard<std::mutex> lock(m_configMutex);
    ReconfigureGuard guard(*this);
    if (!guard.active()) {
        DIRETTA_LOG("configureRingDSD aborted - SDK still holds buffer");
        return;
    }
```

**Step 6: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 7: Commit**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp
git commit -m "feat(sync): safe reconfigure with buffer pinning

- beginReconfigure() returns bool, waits for SDK buffer release
- ReconfigureGuard tracks active state and skips endReconfigure on failure
- configureRingPCM/DSD abort if SDK still holds buffer"
```

---

## Task 9: Add Reset Points in stopPlayback and close

**Files:**
- Modify: `src/DirettaSync.cpp:881-903` (stopPlayback)
- Modify: `src/DirettaSync.cpp:496-532` (close)

**Step 1: Update stopPlayback**

In `src/DirettaSync.cpp`, in `stopPlayback()`, add after line 900 (`stop();`) and before `m_playing = false;`:

```cpp
    // Wait for SDK to release buffer before any potential clear
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
```

**Step 2: Update close**

In `src/DirettaSync.cpp`, in `close()`, add after line 519 (`disconnect(true);`) and before the while loop:

```cpp
    // Wait for SDK to release buffer
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 4: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "feat(sync): add reset points in stopPlayback and close

Wait for SDK buffer release before clearing ring buffer
to prevent use-after-free."
```

---

## Task 10: Add Reset Points in fullReset and resumePlayback

**Files:**
- Modify: `src/DirettaSync.cpp:611-647` (fullReset)
- Modify: `src/DirettaSync.cpp:920-939` (resumePlayback)

**Step 1: Update fullReset**

In `src/DirettaSync.cpp`, in `fullReset()`, replace line 643 (`m_ringBuffer.clear();`) with:

```cpp
        // Wait for SDK to release buffer before clear
        if (waitForPendingRelease(std::chrono::milliseconds(100))) {
            m_ringBuffer.clear();
        }
```

**Step 2: Update resumePlayback**

In `src/DirettaSync.cpp`, in `resumePlayback()`, replace line 931 (`m_ringBuffer.clear();`) with:

```cpp
    // Wait for SDK to release buffer before clear
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
```

**Step 3: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 4: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "feat(sync): add reset points in fullReset and resumePlayback"
```

---

## Task 11: Add Reset Point in open() Same-Format Fast Path

**Files:**
- Modify: `src/DirettaSync.cpp:341-366` (open same-format path)

**Step 1: Update same-format fast path**

In `src/DirettaSync.cpp`, in `open()`, replace line 355 (`m_ringBuffer.clear();`) with:

```cpp
            // Wait for SDK to release buffer before clear
            if (waitForPendingRelease(std::chrono::milliseconds(100))) {
                m_ringBuffer.clear();
            }
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: No errors

**Step 3: Commit**

```bash
git add src/DirettaSync.cpp
git commit -m "feat(sync): add reset point in open() same-format path"
```

---

## Task 12: Update Makefile for SDK 148

**Files:**
- Modify: `Makefile` (update SDK include path)

**Step 1: Check current SDK path**

Run: `grep -n "DirettaHostSDK" Makefile`

**Step 2: Update SDK path to 148**

Update the include path from DirettaHostSDK_147 (or current) to DirettaHostSDK_148.

**Step 3: Verify compilation**

Run: `make clean && make -j4 2>&1 | tail -20`
Expected: Build succeeds with SDK 148

**Step 4: Commit**

```bash
git add Makefile
git commit -m "build: update to SDK 148"
```

---

## Task 13: Final Integration Test

**Step 1: Clean build**

Run: `make clean && make -j4`
Expected: Build succeeds

**Step 2: Run with verbose logging**

Run: `./diretta-renderer -v` (or appropriate test command)
Expected:
- Startup without crashes
- "getNewStream" logs show zero-copy working
- No underrun spam during normal playback

**Step 3: Test format change**

Play PCM, then switch to DSD (or vice versa)
Expected: Clean transition, no crashes

**Step 4: Test stop/play cycles**

Stop and restart playback multiple times
Expected: No crashes, clean state transitions

**Step 5: Final commit**

```bash
git add -A
git commit -m "test: verify SDK 148 zero-copy migration complete"
```

---

## Summary

| Task | Description |
|------|-------------|
| 1 | Add ring buffer zero-copy methods |
| 2 | Add DirettaSync member variables |
| 3 | Add waitForPendingRelease helper |
| 4 | Pre-allocate silence buffer in configureRingPCM |
| 5 | Pre-allocate silence buffer in configureRingDSD |
| 6 | Add fillSilence helper |
| 7 | Update getNewStream signature and implementation |
| 8 | Update beginReconfigure for safe buffer pinning |
| 9 | Add reset points in stopPlayback and close |
| 10 | Add reset points in fullReset and resumePlayback |
| 11 | Add reset point in open() same-format path |
| 12 | Update Makefile for SDK 148 |
| 13 | Final integration test |
