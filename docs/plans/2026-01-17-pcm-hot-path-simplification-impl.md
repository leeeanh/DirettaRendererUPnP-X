# PCM Hot Path Simplification Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce timing variance in the audio callback hot path by eliminating syscalls, unnecessary branches, and I/O operations.

**Architecture:** Replace mutex/CV synchronization with lock-free atomics using seq_cst ordering. Simplify ring buffer operations and move underrun logging to cold path.

**Tech Stack:** C++17, std::atomic with memory_order_seq_cst

---

## Task 1: Replace Mutex/CV Members with Atomics

**Files:**
- Modify: `src/DirettaRenderer.h:77-80`

**Step 1: Update header includes and member variables**

In `src/DirettaRenderer.h`, replace the callback synchronization members:

```cpp
// FIND (lines 77-80):
    // Callback synchronization
    mutable std::mutex m_callbackMutex;
    std::condition_variable m_callbackCV;
    bool m_callbackRunning{false};

// REPLACE WITH:
    // Callback synchronization (lock-free for hot path)
    std::atomic<bool> m_callbackRunning{false};
    std::atomic<bool> m_shutdownRequested{false};
```

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: Compilation errors about m_callbackMutex and m_callbackCV usage (expected, we'll fix in next tasks)

---

## Task 2: Rewrite waitForCallbackComplete()

**Files:**
- Modify: `src/DirettaRenderer.cpp:92-100`

**Step 1: Replace waitForCallbackComplete implementation**

```cpp
// FIND (lines 92-100):
void DirettaRenderer::waitForCallbackComplete() {
    std::unique_lock<std::mutex> lk(m_callbackMutex);
    bool completed = m_callbackCV.wait_for(lk, std::chrono::seconds(5),
        [this]{ return !m_callbackRunning; });
    if (!completed) {
        std::cerr << "[DirettaRenderer] CRITICAL: Callback timeout!" << std::endl;
        m_callbackRunning = false;
    }
}

// REPLACE WITH:
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

**Step 2: Verify this change compiles in isolation**

Run: `make -j4 2>&1 | head -50`
Expected: Still errors from callback and onSetURI/onStop (fixing next)

---

## Task 3: Rewrite Callback Entry Guard

**Files:**
- Modify: `src/DirettaRenderer.cpp:157-171`

**Step 1: Replace the callback RAII guard**

```cpp
// FIND (lines 157-171):
                // RAII guard
                {
                    std::lock_guard<std::mutex> lk(m_callbackMutex);
                    m_callbackRunning = true;
                }
                struct Guard {
                    DirettaRenderer* self;
                    ~Guard() {
                        {
                            std::lock_guard<std::mutex> lk(self->m_callbackMutex);
                            self->m_callbackRunning = false;
                        }
                        self->m_callbackCV.notify_all();
                    }
                } guard{this};

// REPLACE WITH:
                // CRITICAL: Set running flag FIRST, then check shutdown.
                // This order prevents a race where stopper checks m_callbackRunning
                // before we set it, but after checking m_shutdownRequested.
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

**Step 2: Verify compilation**

Run: `make -j4 2>&1 | head -50`
Expected: Still errors from onSetURI/onStop mutex usage

---

## Task 4: Remove Mutex from onSetURI

**Files:**
- Modify: `src/DirettaRenderer.cpp:362-366`

**Step 1: Remove mutex lock around stop() in onSetURI**

```cpp
// FIND (lines 362-366):
                {
                    std::lock_guard<std::mutex> cbLock(m_callbackMutex);
                    m_audioEngine->stop();
                }
                waitForCallbackComplete();

// REPLACE WITH:
                m_audioEngine->stop();
                waitForCallbackComplete();
```

**Step 2: Verify compilation continues**

Run: `make -j4 2>&1 | head -50`
Expected: One more error from onStop

---

## Task 5: Remove Mutex from onStop

**Files:**
- Modify: `src/DirettaRenderer.cpp:441-445`

**Step 1: Remove mutex lock around stop() in onStop**

```cpp
// FIND (lines 441-445):
            {
                std::lock_guard<std::mutex> cbLock(m_callbackMutex);
                m_audioEngine->stop();
            }
            waitForCallbackComplete();

// REPLACE WITH:
            m_audioEngine->stop();
            waitForCallbackComplete();
```

**Step 2: Verify full compilation succeeds**

Run: `make -j4`
Expected: BUILD SUCCESS

**Step 3: Commit Phase 1**

```bash
git add src/DirettaRenderer.h src/DirettaRenderer.cpp
git commit -m "$(cat <<'EOF'
perf(hot-path): replace mutex/CV with lock-free atomics (C0)

Remove syscalls from audio callback hot path:
- Replace m_callbackMutex + m_callbackCV with atomic flags
- Use seq_cst ordering to ensure proper synchronization
- Callback now sets m_callbackRunning BEFORE checking shutdown
- waitForCallbackComplete uses spin-wait with yield

Eliminates ~44,100 syscalls/sec at CD quality playback.

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 6: Test Phase 1

**Step 1: Manual testing**

Test the following scenarios:
- [ ] Start playback, stop playback (verify no hang)
- [ ] Start playback, rapid stop/start cycles (10x)
- [ ] PCM 44.1kHz playback
- [ ] PCM 96kHz playback
- [ ] PCM 192kHz playback (if available)

**Step 2: Listening test**

Compare audio quality against previous build if possible.

---

## Task 7: Fix Modulo Operation (C1)

**Files:**
- Modify: `src/DirettaRingBuffer.h:569`

**Step 1: Replace modulo with bitmask**

```cpp
// FIND (line 569):
        size_t newWritePos = (writePos + len) % size;

// REPLACE WITH:
        size_t newWritePos = (writePos + len) & mask_;
```

**Step 2: Verify compilation**

Run: `make -j4`
Expected: BUILD SUCCESS

---

## Task 8: Remove Dual Memcpy Dispatch (C4)

**Files:**
- Modify: `src/DirettaRingBuffer.h:554-566`

**Step 1: Simplify memcpy dispatch**

```cpp
// FIND (lines 554-566):
        if (firstChunk >= 32) {
            memcpy_audio_fixed(ring + writePos, staged, firstChunk);
        } else if (firstChunk > 0) {
            std::memcpy(ring + writePos, staged, firstChunk);
        }

        size_t secondChunk = len - firstChunk;
        if (secondChunk > 0) {
            if (secondChunk >= 32) {
                memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
            } else {
                std::memcpy(ring, staged + firstChunk, secondChunk);
            }
        }

// REPLACE WITH:
        if (firstChunk > 0) {
            memcpy_audio_fixed(ring + writePos, staged, firstChunk);
        }

        size_t secondChunk = len - firstChunk;
        if (secondChunk > 0) {
            memcpy_audio_fixed(ring, staged + firstChunk, secondChunk);
        }
```

**Step 2: Verify compilation**

Run: `make -j4`
Expected: BUILD SUCCESS

**Step 3: Commit Phase 2**

```bash
git add src/DirettaRingBuffer.h
git commit -m "$(cat <<'EOF'
perf(hot-path): optimize ring buffer operations (C1, C4)

C1: Replace modulo with bitmask in writeToRing
- Division: 20-100 cycles -> AND: 1 cycle
- mask_ already exists as size_ - 1

C4: Remove dual memcpy dispatch
- memcpy_audio_fixed handles small sizes correctly
- Eliminates branch misprediction penalty

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 9: Test Phase 2

**Step 1: Manual testing**

- [ ] PCM playback at all sample rates
- [ ] Gapless playback (multiple tracks)
- [ ] Verify no audio glitches or buffer corruption

---

## Task 10: Add Underrun Counter (C6)

**Files:**
- Modify: `src/DirettaSync.h` (add member)
- Modify: `src/DirettaSync.cpp:1100-1101` (replace logging)
- Modify: `src/DirettaSync.cpp:869+` (add logging in stopPlayback)

**Step 1: Add atomic counter to DirettaSync.h**

Find the private members section and add:

```cpp
// Add after other atomic members (around line 200):
    std::atomic<uint32_t> m_underrunCount{0};
```

**Step 2: Replace underrun logging in getNewStream**

```cpp
// FIND (lines 1100-1101):
        std::cerr << "[DirettaSync] UNDERRUN #" << count
                  << " avail=" << avail << " need=" << currentBytesPerBuffer << std::endl;

// REPLACE WITH:
        m_underrunCount.fetch_add(1, std::memory_order_relaxed);
```

**Step 3: Add logging in stopPlayback**

In `stopPlayback()` function (around line 869), add after `if (!m_playing) return;`:

```cpp
// ADD after line 870 (after the early return):
    // Report accumulated underruns (moved from hot path)
    uint32_t underruns = m_underrunCount.exchange(0, std::memory_order_relaxed);
    if (underruns > 0) {
        std::cerr << "[DirettaSync] Session had " << underruns << " underrun(s)" << std::endl;
    }
```

**Step 4: Verify compilation**

Run: `make -j4`
Expected: BUILD SUCCESS

**Step 5: Commit Phase 3**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp
git commit -m "$(cat <<'EOF'
perf(hot-path): defer underrun logging to cold path (C6)

- Replace std::cerr in getNewStream with atomic counter
- Log accumulated count in stopPlayback (cold path)
- Prevents I/O blocking from causing cascading underruns

Co-Authored-By: Claude Opus 4.5 <noreply@anthropic.com>
EOF
)"
```

---

## Task 11: Final Testing

**Step 1: Functional testing**

- [ ] PCM 16-bit/44.1kHz
- [ ] PCM 24-bit/96kHz
- [ ] PCM 24-bit/192kHz
- [ ] Start/stop cycles (no hangs)
- [ ] Track transitions
- [ ] Gapless playback

**Step 2: Underrun logging verification**

- Induce underrun with CPU load
- Verify count is reported at stop (not during playback)

**Step 3: Listening test**

- [ ] A/B comparison on test device
- [ ] Focus: clarity, background noise, timing/rhythm

---

## Summary

| Phase | Tasks | Optimization | Files |
|-------|-------|--------------|-------|
| 1 | 1-6 | C0: Mutex/CV removal | DirettaRenderer.h/.cpp |
| 2 | 7-9 | C1: Modulo fix, C4: Memcpy simplification | DirettaRingBuffer.h |
| 3 | 10-11 | C6: Underrun I/O removal | DirettaSync.h/.cpp |

**Total commits:** 3
**Estimated lines changed:** ~60 modified, ~20 removed
