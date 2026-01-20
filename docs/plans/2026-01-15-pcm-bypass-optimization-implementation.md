# PCM Bypass Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Implement bit-perfect PCM playback with reduced CPU overhead by skipping unnecessary audio processing when source format matches output format.

**Architecture:** Add bypass detection in AudioDecoder, extend DirettaRingBuffer with S24 pack mode hints and deferred detection, wire adaptive chunk sizing in DirettaRenderer audio thread.

**Tech Stack:** C++17, FFmpeg (libavcodec, libswresample, libavutil), SIMD (AVX2)

---

## Task 1: Add Bypass Mode Flag and Initialization Guard to AudioDecoder

**Files:**
- Modify: `src/AudioEngine.h:119-150`
- Modify: `src/AudioEngine.cpp:72-87`

**Step 1: Write the test scaffold comment**

Add a comment describing expected bypass behavior (no dedicated test framework, manual verification):

```cpp
// In src/AudioEngine.h, add above private members (line ~119)

// PCM Bypass Mode
// When true, skip SwrContext creation and copy decoded frames directly.
// Enabled only for:
// - Integer formats (S16, S32) - NOT float (would corrupt audio)
// - Matching sample rate between source and output
// - Matching channel layout
// See: initResampler() for bypass logic
```

**Step 2: Add member variables to AudioDecoder**

In `src/AudioEngine.h`, add after line 148 (before `initResampler`):

```cpp
    // PCM bypass mode - skip resampler when formats match exactly
    bool m_bypassMode = false;
    bool m_resamplerInitialized = false;
```

**Step 3: Initialize new members in constructor**

In `src/AudioEngine.cpp`, AudioDecoder constructor (line ~73-86), add initialization:

```cpp
AudioDecoder::AudioDecoder()
    : m_formatContext(nullptr)
    , m_codecContext(nullptr)
    , m_swrContext(nullptr)
    , m_audioStreamIndex(-1)
    , m_eof(false)
    , m_rawDSD(false)
    , m_packet(nullptr)
    , m_frame(nullptr)
    , m_dsdRemainderCount(0)
    , m_pcmFifo(nullptr)
    , m_resampleBufferCapacity(0)
    , m_bypassMode(false)           // NEW
    , m_resamplerInitialized(false) // NEW
{
}
```

**Step 4: Verify build compiles**

Run: `make clean && make 2>&1 | head -50`
Expected: Build succeeds, no errors related to new members

**Step 5: Commit**

```bash
git add src/AudioEngine.h src/AudioEngine.cpp
git commit -m "feat(bypass): add m_bypassMode and m_resamplerInitialized flags to AudioDecoder"
```

---

## Task 2: Request Packed Format from Decoder BEFORE avcodec_open2()

**Files:**
- Modify: `src/AudioEngine.cpp:227-241`

**Step 1: Locate the codec opening code**

The target location is between `avcodec_parameters_to_context()` (line 228) and `avcodec_open2()` (line 236).

**Step 2: Add packed format request with capability check**

In `src/AudioEngine.cpp`, replace lines 235-241:

```cpp
    // Copy codec parameters
    if (avcodec_parameters_to_context(m_codecContext, codecpar) < 0) {
        std::cerr << "[AudioDecoder] Failed to copy codec parameters" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }

    // === NEW: Request packed output format BEFORE avcodec_open2() ===
    // This enables bypass mode for integer formats (S16P→S16, S32P→S32)
    // IMPORTANT: Float formats (FLT, FLTP) are NOT eligible - would corrupt audio
    AVSampleFormat srcFmt = m_codecContext->sample_fmt;
    AVSampleFormat preferredFormat = AV_SAMPLE_FMT_NONE;

    if (srcFmt == AV_SAMPLE_FMT_S16P)
        preferredFormat = AV_SAMPLE_FMT_S16;
    else if (srcFmt == AV_SAMPLE_FMT_S32P)
        preferredFormat = AV_SAMPLE_FMT_S32;
    // NOTE: Do NOT request FLT for FLTP - float bypass would corrupt audio

    // Check if decoder actually supports packed format before requesting
    // Some decoders only support planar output - requesting unsupported format
    // causes avcodec_open2() to fail with EINVAL
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
    // === END NEW ===

    // Open codec
    if (avcodec_open2(m_codecContext, codec, nullptr) < 0) {
        std::cerr << "[AudioDecoder] Failed to open codec" << std::endl;
        avcodec_free_context(&m_codecContext);
        avformat_close_input(&m_formatContext);
        return false;
    }
```

**Step 3: Verify build compiles**

Run: `make 2>&1 | head -30`
Expected: Build succeeds

**Step 4: Manual verification**

Run with verbose mode on a FLAC file:
```bash
sudo ./bin/DirettaRendererUPnP --verbose --port 4005
```
Expected log (when playing FLAC): `[AudioDecoder] Requesting packed format: s16` or `s32`

**Step 5: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "feat(bypass): request packed format from decoder before avcodec_open2()"
```

---

## Task 3: Extend TrackInfo with S24 Alignment Hint

**Files:**
- Modify: `src/AudioEngine.h:22-41`

**Step 1: Add S24Alignment enum to TrackInfo struct**

In `src/AudioEngine.h`, extend TrackInfo struct (after line 36, before constructor):

```cpp
struct TrackInfo {
    std::string uri;
    std::string metadata;
    uint32_t sampleRate;
    uint32_t bitDepth;
    uint32_t channels;
    std::string codec;
    uint64_t duration; // in samples
    bool isDSD;        // true if DSD format
    int dsdRate;       // DSD rate (64, 128, 256, 512, 1024)
    bool isCompressed; // true if format requires decoding (FLAC/ALAC), false for WAV/AIFF

    // DSD source format detection (for correct bit ordering)
    enum class DSDSourceFormat { Unknown, DSF, DFF };
    DSDSourceFormat dsdSourceFormat;

    // 24-bit alignment hint from FFmpeg (for S24_P32 packing)
    // This is a HINT only - sample-based detection takes priority
    enum class S24Alignment { Unknown, LsbAligned, MsbAligned };
    S24Alignment s24Alignment = S24Alignment::Unknown;

    TrackInfo() : sampleRate(0), bitDepth(0), channels(2), duration(0),
                  isDSD(false), dsdRate(0), isCompressed(true),
                  dsdSourceFormat(DSDSourceFormat::Unknown),
                  s24Alignment(S24Alignment::Unknown) {}
};
```

**Step 2: Verify build compiles**

Run: `make 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/AudioEngine.h
git commit -m "feat(s24): add S24Alignment enum to TrackInfo for pack mode hinting"
```

---

## Task 4: Detect S24 Alignment Hint in AudioDecoder::open()

**Files:**
- Modify: `src/AudioEngine.cpp:480-510` (after bitDepth is computed)

**Step 1: Add S24 alignment detection AFTER bit depth is computed**

**CRITICAL:** The S24 hint must be set AFTER `m_trackInfo.bitDepth` is computed (around line 492 in the existing code where bit depth is determined from codec parameters). At the proposed earlier location, `bitDepth==0` and the check would never trigger.

In `src/AudioEngine.cpp`, find where `m_trackInfo.bitDepth` is set (around line 492), and add the hint detection AFTER it:

```cpp
    // Existing bit depth detection code (around line 492):
    // m_trackInfo.bitDepth = ... (computed from codec params)

    // === NEW: Detect S24 alignment hint from FFmpeg ===
    // NOTE: bits_per_coded_sample indicates container size, NOT byte alignment
    // This is a HINT only - sample-based detection in DirettaRingBuffer takes priority
    // CRITICAL: Must be AFTER bitDepth is computed (not earlier where bitDepth==0)
    if (m_trackInfo.bitDepth == 24) {
        if (codecpar->bits_per_coded_sample == 32 || codecpar->bits_per_raw_sample == 24) {
            // Most 24-in-32 is LSB-aligned, but MSB-aligned exists (left-justified)
            m_trackInfo.s24Alignment = TrackInfo::S24Alignment::LsbAligned;
            DEBUG_LOG("[AudioDecoder] S24 hint: LsbAligned (from bits_per_coded_sample)");
        }
        // Note: This is a HINT only - sample detection will override if it sees non-zero data
    }
    // === END NEW ===
```

**Step 2: Verify build compiles**

Run: `make 2>&1 | head -20`
Expected: Build succeeds

**Step 3: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "feat(s24): detect S24 alignment hint from FFmpeg codecpar"
```

---

## Task 5: Extend DirettaRingBuffer with S24 Deferred Mode and Hint Support

**Files:**
- Modify: `src/DirettaRingBuffer.h:625-636`

**Step 1: Move S24PackMode enum to public section**

**CRITICAL:** The enum is currently private (line 625). DirettaSync and DirettaRenderer need access, so move it to public.

In `src/DirettaRingBuffer.h`, find the existing private enum and MOVE it to the public section (after line 93, before the push methods):

```cpp
public:
    // S24 pack mode for 24-bit audio (must be public for DirettaSync/DirettaRenderer access)
    enum class S24PackMode { Unknown, LsbAligned, MsbAligned, Deferred };
```

**Step 2: Replace existing m_s24PackMode (don't duplicate)**

**CRITICAL:** The plan must REPLACE the existing `m_s24PackMode` on line 635, not add a duplicate.

In the private section, REPLACE the existing line 635:

```cpp
    // OLD: S24PackMode m_s24PackMode = S24PackMode::Unknown;
    // REPLACED WITH:
    S24PackMode m_s24PackMode = S24PackMode::Unknown;

    // S24 metadata hint (from FFmpeg, non-authoritative)
    S24PackMode m_s24MetadataHint = S24PackMode::Unknown;

    // Sample rate for deferred timeout calculation
    uint32_t m_sampleRate = 0;
    size_t m_deferredSampleCount = 0;
```

**Step 3: Add public setter methods**

Add in public section (after resize/clear methods):

```cpp
public:
    /**
     * @brief Set S24 pack mode hint from upstream metadata
     * CRITICAL: Also resets m_s24PackMode to force re-detection
     * Without this, gapless transitions retain previous track's pack mode
     */
    void setS24PackHint(S24PackMode hint) {
        m_s24MetadataHint = hint;
        // CRITICAL: Reset pack mode to force re-detection on next push
        m_s24PackMode = S24PackMode::Unknown;
        m_deferredSampleCount = 0;
    }

    /**
     * @brief Set sample rate for deferred timeout calculation
     */
    void setSampleRate(uint32_t rate) {
        m_sampleRate = rate;
    }
```

**Step 4: Update detectS24PackMode() with hybrid approach**

Replace the existing `detectS24PackMode()` function (lines 626-634):

**CRITICAL BUG FIX:** The original plan's `allZero` check ignored `data[i*4+3]` (MSB byte), so MSB-aligned data with non-zero MSB would be incorrectly marked as "all zero". Include the MSB byte in the check.

```cpp
private:
    /**
     * Detect S24 pack mode using hybrid approach:
     * 1. Sample-based detection (authoritative) - if non-zero samples exist
     * 2. FFmpeg metadata hint (fallback) - only for all-zero data
     * 3. Deferred (last resort) - wait for non-zero audio data
     */
    S24PackMode detectS24PackMode(const uint8_t* data, size_t numSamples) {
        // Phase 1: ALWAYS try sample-based detection first (authoritative)
        size_t checkSamples = std::min<size_t>(numSamples, 64);
        bool hasNonZeroLsb = false;
        bool hasNonZeroMsb = false;
        bool allZero = true;

        for (size_t i = 0; i < checkSamples; i++) {
            uint8_t b0 = data[i * 4 + 0];  // LSB position
            uint8_t b3 = data[i * 4 + 3];  // MSB position

            if (b0 != 0x00) hasNonZeroLsb = true;
            if (b3 != 0x00) hasNonZeroMsb = true;
            // FIX: Include ALL 4 bytes in the "all zero" check, including MSB (b3)
            if (b0 != 0x00 || data[i*4+1] != 0x00 || data[i*4+2] != 0x00 || b3 != 0x00)
                allZero = false;
        }

        // Phase 2: If samples are conclusive, use them (ignore hint)
        if (hasNonZeroLsb) return S24PackMode::LsbAligned;
        if (hasNonZeroMsb && !hasNonZeroLsb) return S24PackMode::MsbAligned;  // Has non-zero MSB but zero LSB

        // Phase 3: Samples inconclusive (all zero) - use hint as fallback
        if (allZero && m_s24MetadataHint != S24PackMode::Unknown) {
            // DEBUG_LOG("[S24] All-zero samples, using metadata hint");
            return m_s24MetadataHint;
        }

        // Phase 4: No hint available - defer decision
        return S24PackMode::Deferred;
    }
```

**Step 5: Update push24BitPacked() to handle Deferred mode**

In `push24BitPacked()` (around line 189), update the detection logic:

**CRITICAL BUG FIX:** The original plan permanently locked `m_s24PackMode` to LSB after timeout, preventing later detection if non-zero audio appears after long silence. Keep `Deferred` as the stored mode and only use a temporary `effectiveMode` for the conversion.

```cpp
    size_t push24BitPacked(const uint8_t* data, size_t inputSize) {
        if (size_ == 0) return 0;
        size_t numSamples = inputSize / 4;
        if (numSamples == 0) return 0;

        size_t maxSamples = STAGING_SIZE / 3;
        size_t free = getFreeSpace();
        size_t maxSamplesByFree = free / 3;

        if (numSamples > maxSamples) numSamples = maxSamples;
        if (numSamples > maxSamplesByFree) numSamples = maxSamplesByFree;
        if (numSamples == 0) return 0;

        prefetch_audio_buffer(data, numSamples * 4);

        // Handle detection - re-detect if Unknown or Deferred
        if (m_s24PackMode == S24PackMode::Unknown || m_s24PackMode == S24PackMode::Deferred) {
            S24PackMode detected = detectS24PackMode(data, numSamples);

            // Only lock in the mode if detection was conclusive
            if (detected == S24PackMode::LsbAligned || detected == S24PackMode::MsbAligned) {
                m_s24PackMode = detected;
                m_deferredSampleCount = 0;
            } else {
                // Detection inconclusive - track sample count for timeout
                m_s24PackMode = S24PackMode::Deferred;
                m_deferredSampleCount += numSamples;
            }
        }

        // Determine effective mode for this conversion
        // FIX: Do NOT permanently lock m_s24PackMode - allow later detection
        S24PackMode effectiveMode;
        if (m_s24PackMode == S24PackMode::Deferred) {
            // Use timeout hint if available, else LSB default
            if (m_sampleRate > 0 && m_deferredSampleCount > m_sampleRate / 2) {
                // Timeout: use hint if available, else LSB default
                effectiveMode = (m_s24MetadataHint != S24PackMode::Unknown &&
                                 m_s24MetadataHint != S24PackMode::Deferred)
                    ? m_s24MetadataHint : S24PackMode::LsbAligned;
                // DEBUG_LOG("[S24] Timeout after 500ms silence, using effective mode");
            } else {
                // Before timeout, use hint or LSB
                effectiveMode = (m_s24MetadataHint != S24PackMode::Unknown &&
                                 m_s24MetadataHint != S24PackMode::Deferred)
                    ? m_s24MetadataHint : S24PackMode::LsbAligned;
            }
        } else {
            effectiveMode = m_s24PackMode;
        }

        size_t stagedBytes = (effectiveMode == S24PackMode::MsbAligned)
            ? convert24BitPackedShifted_AVX2(m_staging24BitPack, data, numSamples)
            : convert24BitPacked_AVX2(m_staging24BitPack, data, numSamples);
        size_t written = writeToRing(m_staging24BitPack, stagedBytes);
        size_t samplesWritten = written / 3;

        return samplesWritten * 4;
    }
```

**Step 6: Update resize() and clear() - preserve hint in BOTH**

**CRITICAL:** `resize()` is called from `configureRingPCM()` during `open()`. Since S24 hint is now set AFTER `open()` (Task 10), we don't need to preserve it across `resize()`. However, for safety and consistency, **preserve hint in both `resize()` and `clear()`** - the hint is only explicitly cleared via `setS24PackHint(Unknown)`.

```cpp
    void resize(size_t newSize, uint8_t silenceByte) {
        size_ = roundUpPow2(newSize);
        mask_ = size_ - 1;
        buffer_.resize(size_);
        silenceByte_.store(silenceByte, std::memory_order_release);
        clear();
        fillWithSilence();
        m_s24PackMode = S24PackMode::Unknown;
        // NOTE: m_s24MetadataHint is NOT cleared - preserved across resize
        // Hint is explicitly managed via setS24PackHint()
        m_deferredSampleCount = 0;
    }

    void clear() {
        writePos_.store(0, std::memory_order_release);
        readPos_.store(0, std::memory_order_release);
        m_s24PackMode = S24PackMode::Unknown;
        m_deferredSampleCount = 0;
        // NOTE: m_s24MetadataHint is NOT cleared - preserved across clear
        // Hint is explicitly managed via setS24PackHint()
    }
```

**Summary:** Neither `resize()` nor `clear()` clears `m_s24MetadataHint`. The hint is only modified by explicit calls to `setS24PackHint()`.

**Step 6: Verify build compiles**

Run: `make 2>&1 | head -30`
Expected: Build succeeds

**Step 7: Commit**

```bash
git add src/DirettaRingBuffer.h
git commit -m "feat(s24): add hybrid pack detection with hint support and deferred timeout"
```

---

## Task 6: Add S24 Hint Propagation to DirettaSync

**Files:**
- Modify: `src/DirettaSync.h:218-230`
- Modify: `src/DirettaSync.cpp` (in `open()` method)

**Step 1: Add setS24PackHint() method to DirettaSync**

In `src/DirettaSync.h`, after line 225 (after `getFormat()`):

```cpp
    const AudioFormat& getFormat() const { return m_currentFormat; }

    /**
     * @brief Set S24 pack mode hint for 24-bit audio
     * Propagates to ring buffer for hybrid detection
     */
    void setS24PackHint(DirettaRingBuffer::S24PackMode hint) {
        m_ringBuffer.setS24PackHint(hint);
    }
```

**Step 2: Call setSampleRate() in DirettaSync::open()**

Find the `open()` method in `src/DirettaSync.cpp` and add after ring buffer configuration:

```cpp
// In configureRingPCM() or open(), after ring buffer resize:
m_ringBuffer.setSampleRate(format.sampleRate);
```

**Step 3: Verify build compiles**

Run: `make 2>&1 | head -20`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/DirettaSync.h src/DirettaSync.cpp
git commit -m "feat(s24): add setS24PackHint() to DirettaSync for hint propagation"
```

---

## Task 7: Implement canBypass() Helper in AudioDecoder

**Files:**
- Modify: `src/AudioEngine.h:148-150`
- Modify: `src/AudioEngine.cpp` (add new method)

**Step 1: Declare canBypass() method**

In `src/AudioEngine.h`, after line 148 (before `initResampler`):

```cpp
    bool m_bypassMode = false;
    bool m_resamplerInitialized = false;

    // Check if bypass mode is possible for given output format
    bool canBypass(uint32_t outputRate, uint32_t outputBits) const;

    bool initResampler(uint32_t outputRate, uint32_t outputBits);
```

**Step 2: Implement canBypass() method**

In `src/AudioEngine.cpp`, add before `initResampler()`:

**CRITICAL BUG FIX:** 24-bit PCM is decoded as S32 (32 bytes/sample). The original plan compared `srcBits` (32) with `outputBits` (24), which would always mismatch and disable bypass for 24-bit audio. Allow S32 when outputBits==24.

```cpp
/**
 * Check if PCM bypass mode is possible
 * Bypass enabled only for:
 * - Integer formats (S16, S32) - NOT float (would corrupt audio)
 * - Matching sample rate
 * - Matching channel layout
 * - Compatible bit depth (S32 is valid for 24-bit output)
 */
bool AudioDecoder::canBypass(uint32_t outputRate, uint32_t outputBits) const {
    if (!m_codecContext) return false;

    AVSampleFormat srcFmt = m_codecContext->sample_fmt;

    // Float formats NEVER bypass - downstream expects signed integer
    if (srcFmt == AV_SAMPLE_FMT_FLT || srcFmt == AV_SAMPLE_FMT_FLTP ||
        srcFmt == AV_SAMPLE_FMT_DBL || srcFmt == AV_SAMPLE_FMT_DBLP) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (float format requires conversion)");
        return false;
    }

    // Check for packed integer format
    bool isPackedInteger = (srcFmt == AV_SAMPLE_FMT_S16 || srcFmt == AV_SAMPLE_FMT_S32);
    if (!isPackedInteger) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (planar format, swr needed)");
        return false;
    }

    // Check sample rate match
    if (static_cast<uint32_t>(m_codecContext->sample_rate) != outputRate) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (rate mismatch: "
                  << m_codecContext->sample_rate << " vs " << outputRate << ")");
        return false;
    }

    // Check channel count match
    if (m_codecContext->ch_layout.nb_channels != static_cast<int>(m_trackInfo.channels)) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (channel mismatch)");
        return false;
    }

    // Check bit depth compatibility
    // FIX: 24-bit audio is decoded as S32 (32 bits/sample), but outputBits is 24
    // Allow bypass for S32 when outputBits is 24 (24-in-32 format, handled by ring buffer packing)
    int srcBits = av_get_bytes_per_sample(srcFmt) * 8;
    bool bitsMatch = (static_cast<uint32_t>(srcBits) == outputBits);
    bool is24BitIn32 = (srcFmt == AV_SAMPLE_FMT_S32 && outputBits == 24);

    if (!bitsMatch && !is24BitIn32) {
        DEBUG_LOG("[AudioDecoder] canBypass: NO (bit depth mismatch: "
                  << srcBits << " vs " << outputBits << ")");
        return false;
    }

    DEBUG_LOG("[AudioDecoder] canBypass: YES (bit-perfect path enabled"
              << (is24BitIn32 ? ", 24-in-32 format" : "") << ")");
    return true;
}
```

**Step 3: Verify build compiles**

Run: `make 2>&1 | head -20`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/AudioEngine.h src/AudioEngine.cpp
git commit -m "feat(bypass): implement canBypass() helper for format matching"
```

---

## Task 8: Modify readSamples() to Use Bypass Mode

**Files:**
- Modify: `src/AudioEngine.cpp` (readSamples method, around line 700-750)

**Step 1: Find the resampler initialization check in readSamples()**

Look for code like `if (!m_swrContext)` in readSamples().

**Step 2: Replace with m_resamplerInitialized flag check**

Replace the existing resampler init logic with:

```cpp
// At start of PCM decode path in readSamples():
if (!m_resamplerInitialized && !m_trackInfo.isDSD) {
    m_bypassMode = canBypass(outputRate, outputBits);

    if (!m_bypassMode) {
        if (!initResampler(outputRate, outputBits)) {
            std::cerr << "[AudioDecoder] Failed to init resampler" << std::endl;
            return 0;
        }
    } else {
        std::cout << "[AudioDecoder] PCM BYPASS enabled - bit-perfect path" << std::endl;
    }

    // Initialize FIFO (needed for both bypass and resampler modes)
    // Dynamic sizing based on output rate
    constexpr int64_t BASE_RATE = 44100;
    constexpr int64_t BASE_FRAME_SIZE = 4096;
    constexpr int MIN_FIFO_SAMPLES = 4096;
    constexpr int MAX_FIFO_SAMPLES = 32768;

    AVSampleFormat outFormat = m_bypassMode ? m_codecContext->sample_fmt :
        (outputBits == 16 ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_S32);

    // Use 64-bit arithmetic to avoid overflow at high sample rates
    // At 384kHz: 2 * 384000 * 4096 = 3,145,728,000 which exceeds INT_MAX
    int64_t fifoSamples64 = (2 * static_cast<int64_t>(outputRate) * BASE_FRAME_SIZE) / BASE_RATE;

    // Smaller FIFO for bypass mode (no resampling expansion)
    if (m_bypassMode) {
        fifoSamples64 = std::min(fifoSamples64, static_cast<int64_t>(8192));
    }

    int fifoSamples = static_cast<int>(std::clamp(fifoSamples64,
                                                   static_cast<int64_t>(MIN_FIFO_SAMPLES),
                                                   static_cast<int64_t>(MAX_FIFO_SAMPLES)));

    if (m_pcmFifo) {
        av_audio_fifo_free(m_pcmFifo);
    }
    m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, fifoSamples);

    // CRITICAL: Check for allocation failure
    if (!m_pcmFifo) {
        std::cerr << "[AudioDecoder] Failed to allocate PCM FIFO" << std::endl;
        return 0;  // Return 0 samples like initResampler failure path
    }

    DEBUG_LOG("[AudioDecoder] FIFO allocated: " << fifoSamples << " samples for " << outputRate << "Hz");

    m_resamplerInitialized = true;
}
```

**Step 3: Add bypass path in decode loop**

**CRITICAL BUG FIX:** The original plan wrote only to FIFO but never output samples, causing `readSamples()` to stall. The existing code at line ~930 has a "no resample needed" direct-copy branch that copies directly to the output buffer. The bypass path must ALSO copy to output.

**MANDATORY: Use Option A below. Option B is provided only as fallback if Option A structure doesn't match existing code.**

**Option A (REQUIRED): Reuse existing direct-copy path - copies to output buffer**

Find the existing direct-copy branch (around line 930) and modify its condition to include bypass mode:

```cpp
// The existing code structure in readSamples() (around line 900-950):
//
// bool needsResample = (src_rate != dst_rate || src_fmt != dst_fmt || ...);
// 
// if (!needsResample) {
//     // Direct copy to output buffer
//     memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
//     outputPtr += bytesToCopy;
//     samplesProduced += frameSamples;
// } else {
//     // Resample path via swr_convert
// }

// MODIFY the condition to include bypass mode:
if (m_bypassMode || !needsResample) {
    // Direct copy to output buffer - bit-perfect path
    int frameSamples = m_frame->nb_samples;
    int bytesPerSample = av_get_bytes_per_sample(m_codecContext->sample_fmt);
    size_t bytesToCopy = frameSamples * bytesPerSample * m_trackInfo.channels;

    // Ensure output buffer has space
    if (samplesProduced + frameSamples > numSamples) {
        // Write excess to FIFO for next call
        int excessSamples = (samplesProduced + frameSamples) - numSamples;
        int fitSamples = frameSamples - excessSamples;

        // Copy what fits to output
        size_t fitBytes = fitSamples * bytesPerSample * m_trackInfo.channels;
        memcpy_audio(buffer.data() + (samplesProduced * bytesPerSample * m_trackInfo.channels),
                     m_frame->data[0], fitBytes);
        samplesProduced += fitSamples;

        // Write excess to FIFO
        uint8_t* excessPtr = m_frame->data[0] + fitBytes;
        av_audio_fifo_write(m_pcmFifo, reinterpret_cast<void**>(&excessPtr), excessSamples);
    } else {
        // All fits - direct copy
        memcpy_audio(buffer.data() + (samplesProduced * bytesPerSample * m_trackInfo.channels),
                     m_frame->data[0], bytesToCopy);
        samplesProduced += frameSamples;
    }
} else {
    // Existing resampler path via swr_convert
    // ...
}
```

**Option B (FALLBACK ONLY): Write to FIFO, then read immediately to output**

Only if Option A's structure doesn't match existing code. This option ALSO copies to output buffer:

```cpp
if (m_bypassMode) {
    // Write decoded frame to FIFO
    av_audio_fifo_write(m_pcmFifo, reinterpret_cast<void**>(m_frame->data), m_frame->nb_samples);

    // Immediately read from FIFO to output (critical - don't leave in FIFO!)
    int bytesPerSample = av_get_bytes_per_sample(m_codecContext->sample_fmt);
    int samplesToRead = std::min(av_audio_fifo_size(m_pcmFifo),
                                  static_cast<int>(numSamples - samplesProduced));
    if (samplesToRead > 0) {
        uint8_t* outPtr = buffer.data() + (samplesProduced * bytesPerSample * m_trackInfo.channels);
        int readSamples = av_audio_fifo_read(m_pcmFifo, reinterpret_cast<void**>(&outPtr), samplesToRead);
        samplesProduced += readSamples;
    }
}
```

**IMPORTANT:** Whichever option is used, the bypass path MUST populate the output buffer before returning. Verify `samplesProduced` is incremented and returned correctly.

**Step 4: Verify build compiles**

Run: `make 2>&1 | head -30`
Expected: Build succeeds

**Step 5: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "feat(bypass): add bypass mode path in readSamples() with dynamic FIFO sizing"
```

---

## Task 9: Reset Bypass State on Seek and Track Change

**Files:**
- Modify: `src/AudioEngine.cpp` (seek method, close method)

**Step 1: Reset flags in close()**

In `AudioDecoder::close()`, add:

```cpp
void AudioDecoder::close() {
    // ... existing cleanup ...

    // Reset bypass state for next track
    m_bypassMode = false;
    m_resamplerInitialized = false;
}
```

**Step 2: Verify m_resamplerInitialized is preserved on seek**

In `AudioDecoder::seek()`, the FIFO is reset but bypass state should be preserved. Verify existing code only does:

```cpp
    // Reset PCM FIFO (clear stale samples)
    if (m_pcmFifo) {
        av_audio_fifo_reset(m_pcmFifo);
    }
    // m_bypassMode and m_resamplerInitialized should NOT be reset here
```

**Step 3: Verify build compiles**

Run: `make 2>&1 | head -20`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/AudioEngine.cpp
git commit -m "feat(bypass): reset bypass state in close(), preserve on seek"
```

---

## Task 10: Propagate S24 Hint in DirettaRenderer Audio Callback

**Files:**
- Modify: `src/DirettaRenderer.cpp:150-235`
- Modify: `src/DirettaRenderer.h` (add track URI tracking)

**Step 1: Add track change detection for gapless transitions**

**CRITICAL BUG FIX:** The original plan set S24 hint only inside `needsOpen`. Gapless same-format transitions don't call `open()`, so the hint would never update between tracks. Detect track changes by comparing URIs.

In `src/DirettaRenderer.h`, add a member to track current URI:

```cpp
private:
    // ... existing members ...
    std::string m_lastProcessedURI;  // Track URI for S24 hint change detection
```

**Step 2: Extract S24 hint propagation into a helper**

In `src/DirettaRenderer.cpp`, add a helper method before the audio callback:

```cpp
/**
 * Propagate S24 alignment hint to DirettaSync
 * Called on open() and on track changes (gapless)
 */
void DirettaRenderer::propagateS24Hint(const TrackInfo& trackInfo) {
    if (trackInfo.bitDepth == 24 && !trackInfo.isDSD && m_direttaSync) {
        DirettaRingBuffer::S24PackMode hint = DirettaRingBuffer::S24PackMode::Unknown;
        if (trackInfo.s24Alignment == TrackInfo::S24Alignment::LsbAligned) {
            hint = DirettaRingBuffer::S24PackMode::LsbAligned;
        } else if (trackInfo.s24Alignment == TrackInfo::S24Alignment::MsbAligned) {
            hint = DirettaRingBuffer::S24PackMode::MsbAligned;
        }
        m_direttaSync->setS24PackHint(hint);
        DEBUG_LOG("[Callback] S24 hint propagated: "
                  << (hint == DirettaRingBuffer::S24PackMode::LsbAligned ? "LsbAligned" :
                      hint == DirettaRingBuffer::S24PackMode::MsbAligned ? "MsbAligned" : "Unknown"));
    }
}
```

**Step 3: Call helper after open() AND on track change (CRITICAL for gapless)**

**CRITICAL:** The S24 hint must be propagated in TWO places:
1. After `open()` completes (for format changes)
2. On track change WITHOUT `open()` (for gapless same-format transitions)

The `else if (trackChanged)` branch handles case #2 - this is OUTSIDE the `needsOpen` block.

In the audio callback:

```cpp
                const TrackInfo& trackInfo = m_audioEngine->getCurrentTrackInfo();

                // Detect track change (for gapless S24 hint updates)
                // CRITICAL: This detects gapless transitions where open() is NOT called
                bool trackChanged = (trackInfo.uri != m_lastProcessedURI);

                // Build format
                AudioFormat format(sampleRate, bitDepth, channels);
                format.isDSD = trackInfo.isDSD;
                format.isCompressed = trackInfo.isCompressed;

                // ... existing format setup ...

                // Open/resume connection if needed
                if (needsOpen) {
                    if (!m_direttaSync->open(format)) {
                        std::cerr << "[Callback] Failed to open DirettaSync" << std::endl;
                        return false;
                    }

                    // Case 1: Propagate S24 hint after open()
                    propagateS24Hint(trackInfo);
                    m_lastProcessedURI = trackInfo.uri;
                }
                // Case 2: Handle gapless track changes (same format, open() NOT called)
                // CRITICAL: This else-if is OUTSIDE needsOpen - handles gapless transitions
                else if (trackChanged) {
                    propagateS24Hint(trackInfo);
                    m_lastProcessedURI = trackInfo.uri;
                    DEBUG_LOG("[Callback] Gapless track change, S24 hint updated");
                }
```

**Verification:** The `else if (trackChanged)` block executes when:
- `needsOpen == false` (already playing, same format)
- `trackChanged == true` (new track URI detected)

This ensures S24 hint updates on gapless album playback where tracks have different S24 alignment.

**Step 4: Add include for DirettaRingBuffer**

At top of `src/DirettaRenderer.cpp`, ensure include is present:

```cpp
#include "DirettaRingBuffer.h"  // For S24PackMode enum
```

**Step 3: Verify build compiles**

Run: `make 2>&1 | head -30`
Expected: Build succeeds

**Step 4: Commit**

```bash
git add src/DirettaRenderer.cpp
git commit -m "feat(s24): propagate S24 alignment hint from TrackInfo to DirettaSync"
```

---

## Task 11: Add Adaptive Chunk Sizing to Audio Thread

**Files:**
- Modify: `src/DirettaRenderer.cpp:525-595`
- Modify: `src/DirettaRenderer.h` (add helper method)

**Step 1: Add missing include for std::min/std::max**

**CRITICAL:** The adaptive chunk sizing uses `std::min` and `std::max`. Ensure `<algorithm>` is included.

In `src/DirettaRenderer.cpp`, verify or add at top (around line 8):

```cpp
#include <algorithm>  // For std::min, std::max
```

**Step 2: Add helper method declaration**

In `src/DirettaRenderer.h`, add private method:

```cpp
private:
    // ... existing members ...

    // Adaptive chunk sizing based on buffer level
    size_t calculateAdaptiveChunkSize(size_t baseSize, float bufferLevel) const;
```

**Step 3: Implement calculateAdaptiveChunkSize()**

In `src/DirettaRenderer.cpp`, add before `audioThreadFunc()`:

```cpp
/**
 * Calculate adaptive chunk size based on buffer level
 * Targets ~50% buffer fill for smooth flow control
 */
size_t DirettaRenderer::calculateAdaptiveChunkSize(size_t baseSize, float bufferLevel) const {
    constexpr float TARGET_LEVEL = 0.50f;
    constexpr float DEADBAND = 0.10f;
    constexpr float MIN_SCALE = 0.25f;
    constexpr float MAX_SCALE = 1.50f;

    float scale = 1.0f;
    float deviation = bufferLevel - TARGET_LEVEL;

    if (deviation > DEADBAND) {
        // Buffer too full - reduce chunk size
        scale = 1.0f - ((deviation - DEADBAND) / (1.0f - TARGET_LEVEL - DEADBAND));
        scale = std::max(scale, MIN_SCALE);
    } else if (deviation < -DEADBAND) {
        // Buffer too empty - increase chunk size
        scale = 1.0f + ((-deviation - DEADBAND) / (TARGET_LEVEL - DEADBAND)) * 0.5f;
        scale = std::min(scale, MAX_SCALE);
    }

    return static_cast<size_t>(baseSize * scale);
}
```

**Step 4: Wire adaptive sizing in audioThreadFunc()**

In `audioThreadFunc()` (around line 554), replace fixed chunk size:

```cpp
            // Adjust samples per call based on format
            size_t baseSamplesPerCall = isDSD ? 32768 : 8192;

            // Apply adaptive sizing based on buffer level
            float bufferLevel = 0.0f;
            if (m_direttaSync && m_direttaSync->isPlaying()) {
                bufferLevel = m_direttaSync->getBufferLevel();
            }
            size_t samplesPerCall = calculateAdaptiveChunkSize(baseSamplesPerCall, bufferLevel);
```

**Step 5: Verify build compiles**

Run: `make 2>&1 | head -30`
Expected: Build succeeds

**Step 6: Commit**

```bash
git add src/DirettaRenderer.h src/DirettaRenderer.cpp
git commit -m "feat(adaptive): add buffer-level adaptive chunk sizing in audio thread"
```

---

## Task 12: Final Build and Integration Test

**Files:**
- All modified files

**Step 1: Clean build**

Run: `make clean && make 2>&1`
Expected: Build succeeds with no errors or warnings related to new code

**Step 2: Test with PCM file (bypass path)**

```bash
sudo ./bin/DirettaRendererUPnP --verbose --port 4005
# Play a 16-bit/44.1kHz FLAC file from control point
```

Expected logs:
- `[AudioDecoder] Requesting packed format: s16`
- `[AudioDecoder] canBypass: YES (bit-perfect path enabled)`
- `[AudioDecoder] PCM BYPASS enabled - bit-perfect path`

**Step 3: Test with lossy file (resampler path)**

```bash
# Play an AAC or MP3 file from control point
```

Expected logs:
- `[AudioDecoder] canBypass: NO (float format requires conversion)`
- No "PCM BYPASS enabled" message

**Step 4: Test with 24-bit file (S24 hint)**

```bash
# Play a 24-bit FLAC file from control point
```

Expected logs:
- `[AudioDecoder] S24 hint: LsbAligned`
- `[Callback] S24 hint propagated: LsbAligned`

**Step 5: Test 384kHz file (dynamic FIFO sizing)**

```bash
# Play a high-resolution 384kHz file
```

Expected logs:
- `[AudioDecoder] FIFO allocated: 32768 samples for 384000Hz`

**Step 6: Commit all changes**

```bash
git add .
git commit -m "feat(bypass): complete PCM bypass optimization implementation

- Request packed format from decoder before avcodec_open2()
- Add bypass mode detection in canBypass()
- Implement bypass path in readSamples()
- Add S24 alignment hint propagation
- Extend DirettaRingBuffer with hybrid S24 detection
- Add adaptive chunk sizing in audio thread
- Add dynamic FIFO sizing based on sample rate"
```

---

## Verification Checklist

| Test Case | Expected Behavior |
|-----------|-------------------|
| 16-bit FLAC | Bypass enabled, no SwrContext |
| 24-bit FLAC | S24 hint propagated, sample detection |
| AAC/MP3 | Bypass disabled, SwrContext used |
| 384kHz PCM | FIFO = 32768, no overflow |
| Gapless 24-bit | S24 mode reset on track change |
| Seek during playback | FIFO reset, bypass preserved |
| Silence at track start | Deferred detection, 500ms timeout |

---

## Rollback Plan

If issues occur, each commit is atomic and can be reverted:

```bash
git log --oneline -10  # Find commit to revert
git revert <commit-hash>
```

Critical changes in order of risk:
1. `push24BitPacked()` changes (Task 5) - affects all 24-bit audio
2. `readSamples()` bypass path (Task 8) - affects all PCM playback
3. Packed format request (Task 2) - could break some decoders

---

## Critical Fixes Applied (from code review)

| Issue | Fix |
|-------|-----|
| S24 hint detection at bitDepth==0 | Moved to AFTER bitDepth is computed (Task 4) |
| canBypass fails for 24-bit (S32 vs 24) | Added `is24BitIn32` check (Task 7) |
| Bypass path never outputs samples | Two options: reuse direct-copy or read FIFO immediately (Task 8) |
| FIFO alloc failure unchecked | Added null check and bail (Task 8) |
| S24PackMode is private | Moved enum to public section (Task 5) |
| Duplicate m_s24PackMode | REPLACE existing, don't add second (Task 5) |
| detectS24PackMode ignores MSB byte | Include b3 in allZero check (Task 5) |
| Deferred timeout locks mode permanently | Keep Deferred, use temporary effectiveMode (Task 5) |
| Ambiguous resize/clear hint handling | Clarified: preserve hint in BOTH (Task 5) |
| S24 hint not set on gapless transitions | Added track change detection via URI comparison (Task 10) |
| S24 hint wiped by open()/resize() | Set hint AFTER open() completes (Task 10) |
| Missing `<algorithm>` include | Added for std::min/max (Task 11) |

---

