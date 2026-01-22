# SDK 148 Migration: Simple Zero-Copy Design

## Problem

SDK 148 changes `getNewStream()` signature from `DIRETTA::Stream&` to `diretta_stream&`. The SDK reads the buffer asynchronously after we return.

## Solution

Zero-copy only. Point SDK directly at ring buffer. If data wraps, return silence.

1MB ring buffer makes wrap rare (<0.1% of callbacks).

## Implementation

### DirettaRingBuffer

```cpp
// New constant
static constexpr size_t RING_BUFFER_SIZE = 1024 * 1024;  // 1MB

// New methods
bool getDirectReadRegion(size_t needed, const uint8_t*& region, size_t& avail) const {
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

void advanceReadPos(size_t bytes) {
    size_t rp = readPos_.load(std::memory_order_relaxed);
    readPos_.store((rp + bytes) & mask_, std::memory_order_release);
}
```

### DirettaSync

**New members:**
```cpp
std::atomic<size_t> m_pendingAdvance{0};

// Silence buffer - pre-allocated on format change, never resized in callback
alignas(64) std::vector<uint8_t> m_silenceBuffer;
```

**Silence buffer allocation (on format change):**
```cpp
void DirettaSync::configureRingPCM(...) {
    // ... existing config ...

    // Pre-allocate silence buffer for max possible bytes per callback
    // (base frames + 1 extra for remainder accumulation) * bytesPerFrame
    size_t maxBytes = (framesBase + 1) * bytesPerFrame;
    if (m_silenceBuffer.size() < maxBytes) {
        m_silenceBuffer.resize(maxBytes);
    }
}

void DirettaSync::configureRingDSD(...) {
    // ... existing config ...

    size_t maxBytes = m_bytesPerBuffer + 64;  // DSD buffer + margin
    if (m_silenceBuffer.size() < maxBytes) {
        m_silenceBuffer.resize(maxBytes);
    }
}
```

**Wait helper (for safe reset):**
```cpp
// Wait for SDK to release buffer (next callback clears m_pendingAdvance)
// Returns true if released, false if timeout
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

**Updated getNewStream:**
```cpp
bool DirettaSync::getNewStream(diretta_stream& stream) {
    // Complete previous deferred advance (atomic load)
    size_t pending = m_pendingAdvance.load(std::memory_order_acquire);
    if (pending > 0) {
        m_ringBuffer.advanceReadPos(pending);
        m_pendingAdvance.store(0, std::memory_order_release);
    }

    // ... existing generation counter / bytesPerBuffer calculation ...

    // ... existing early exits (reconfiguring, stop, prefill, etc.) ...
    // All early exits call fillSilence() - unchanged

    // Check available data first (distinguishes underrun vs wrap)
    size_t avail = m_ringBuffer.getAvailable();
    if (avail < currentBytesPerBuffer) {
        // True underrun - don't advance, just silence
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
        fillSilence(stream, currentBytesPerBuffer);
        return true;
    }

    // Zero-copy path
    const uint8_t* ptr;
    size_t contiguous;
    if (m_ringBuffer.getDirectReadRegion(currentBytesPerBuffer, ptr, contiguous)) {
        stream.Data.P = const_cast<void*>(static_cast<const void*>(ptr));
        stream.Size = currentBytesPerBuffer;
        m_pendingAdvance.store(currentBytesPerBuffer, std::memory_order_release);
    } else {
        // Wrap (data available but not contiguous) - advance to realign
        m_ringBuffer.advanceReadPos(currentBytesPerBuffer);
        fillSilence(stream, currentBytesPerBuffer);
    }

    return true;
}

void DirettaSync::fillSilence(diretta_stream& stream, size_t bytes) {
    // m_silenceBuffer pre-allocated on format change, always large enough
    std::memset(m_silenceBuffer.data(), m_cachedSilenceByte, bytes);
    stream.Data.P = m_silenceBuffer.data();
    stream.Size = bytes;
}
```

**Reset points (all paths that clear/resize ring buffer):**

Wait for SDK to release buffer before clearing. If timeout, skip clear (buffer stays valid).

```cpp
void DirettaSync::stopPlayback() {
    // ... existing stop logic ...
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
    // If timeout: skip clear, buffer stays valid for late SDK read
}

void DirettaSync::close() {
    // ... existing close logic ...
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
}

void DirettaSync::fullReset() {
    // ... existing logic ...
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
}

void DirettaSync::resumePlayback() {
    // ... existing logic ...
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
}

// In open() same-format fast path:
if (sameFormat) {
    if (waitForPendingRelease(std::chrono::milliseconds(100))) {
        m_ringBuffer.clear();
    }
    // ...
}

// Returns false if reconfigure should be aborted
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

// Caller pattern:
void DirettaSync::configureRingPCM(...) {
    if (!beginReconfigure()) {
        return;  // Abort - SDK still holds buffer
    }
    // ... resize buffers ...
    endReconfigure();
}
```

## Checklist

### DirettaRingBuffer
- [ ] Add `RING_BUFFER_SIZE` constexpr (1MB)
- [ ] Add `getDirectReadRegion()` method
- [ ] Add `advanceReadPos()` method
- [ ] Update buffer allocation to use new size

### DirettaSync
- [ ] Add `std::atomic<size_t> m_pendingAdvance` member
- [ ] Add `m_silenceBuffer` member (aligned)
- [ ] Add `waitForPendingRelease()` helper
- [ ] Change `getNewStream()` signature to `diretta_stream&`
- [ ] Implement deferred advance at start of `getNewStream()` (atomic)
- [ ] Check `getAvailable()` first to distinguish underrun vs wrap
- [ ] Implement zero-copy path with `getDirectReadRegion()`
- [ ] Add `fillSilence()` helper using `m_silenceBuffer`
- [ ] Pre-allocate `m_silenceBuffer` in `configureRingPCM()`
- [ ] Pre-allocate `m_silenceBuffer` in `configureRingDSD()`
- [ ] Advance on wrap only (not underrun)
- [ ] Wait + clear in `stopPlayback()`
- [ ] Wait + clear in `close()`
- [ ] Wait + clear in `fullReset()`
- [ ] Wait + clear in `resumePlayback()`
- [ ] Wait + clear in `open()` same-format path
- [ ] Change `beginReconfigure()` to return bool
- [ ] Clear `m_reconfiguring` and return false on timeout
- [ ] Check `beginReconfigure()` return in `configureRingPCM()`
- [ ] Check `beginReconfigure()` return in `configureRingDSD()`

### Build
- [ ] Update includes for SDK 148
- [ ] Test compilation
- [ ] Test playback
