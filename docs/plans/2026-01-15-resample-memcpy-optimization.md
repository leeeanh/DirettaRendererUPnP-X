# Resample Path Memory Copy Optimization Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate unnecessary memory copies in PCM resample path using AVAudioFifo and direct write optimization.

**Architecture:** Replace manual O(n) memmove buffer management with O(1) circular FIFO for PCM overflow. Add direct write path when output buffer has sufficient space. Keep DSD buffer separate with renamed members for clarity.

**Tech Stack:** FFmpeg libavutil (AVAudioFifo), C++17

---

## Task 1: Add AVAudioFifo Include

**Files:**
- Modify: `src/AudioEngine.h:12-16`

**Step 1: Add the include**

In `src/AudioEngine.h`, add `#include <libavutil/audio_fifo.h>` after line 16:

```cpp
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>
}
```

**Step 2: Build to verify include works**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X clean && make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X`
Expected: Build succeeds with no errors

**Step 3: Commit**

```bash
git add src/AudioEngine.h
git commit -m "feat: add AVAudioFifo include for PCM buffer optimization

```

---

## Task 2: Rename DSD Buffer Members in Header

**Files:**
- Modify: `src/AudioEngine.h:134-135`

**Step 1: Rename members in AudioDecoder class**

Change lines 134-135 from:

```cpp
    AudioBuffer m_remainingSamples;
    size_t m_remainingCount;
```

To:

```cpp
    // DSD remainder buffer (byte-level L/R channel buffering)
    AudioBuffer m_dsdRemainderBuffer;
    size_t m_dsdRemainderCount;
```

**Step 2: Build to see all compilation errors (expected)**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Compilation errors referencing old member names in AudioEngine.cpp

**Step 3: Note errors for next task**

List files/lines that need updating (should all be in AudioEngine.cpp).

---

## Task 3: Add PCM FIFO Member to Header

**Files:**
- Modify: `src/AudioEngine.h:134-138`

**Step 1: Add m_pcmFifo member after DSD buffer**

After the DSD buffer members (from Task 2), add:

```cpp
    // DSD remainder buffer (byte-level L/R channel buffering)
    AudioBuffer m_dsdRemainderBuffer;
    size_t m_dsdRemainderCount;

    // PCM FIFO for sample overflow (O(1) circular buffer)
    AVAudioFifo* m_pcmFifo = nullptr;
```

**Step 2: Build (still expecting errors from rename)**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Same errors as Task 2 (member rename not yet fixed in .cpp)

**Step 3: Commit header changes**

```bash
git add src/AudioEngine.h
git commit -m "feat: add PCM FIFO member and rename DSD buffer for clarity

- Rename m_remainingSamples -> m_dsdRemainderBuffer
- Rename m_remainingCount -> m_dsdRemainderCount
- Add m_pcmFifo for O(1) PCM overflow handling

```

---

## Task 4: Update Constructor Initialization

**Files:**
- Modify: `src/AudioEngine.cpp:73-85`

**Step 1: Update AudioDecoder constructor**

Change the initialization list and body. Replace:

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
    , m_remainingCount(0)
    , m_resampleBufferCapacity(0)
{
}
```

With:

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
{
}
```

**Step 2: Build (still expecting errors in readSamples)**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Errors in readSamples() and other functions using old names

---

## Task 5: Update close() for FIFO Cleanup

**Files:**
- Modify: `src/AudioEngine.cpp:514-534`

**Step 1: Add FIFO cleanup to close()**

After line 526 (`av_packet_free(&m_packet);`), before line 527 (`if (m_formatContext)`), add FIFO cleanup:

```cpp
void AudioDecoder::close() {
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }
    if (m_codecContext) {
        avcodec_free_context(&m_codecContext);
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    // Free PCM FIFO
    if (m_pcmFifo) {
        av_audio_fifo_free(m_pcmFifo);
        m_pcmFifo = nullptr;
    }
    if (m_formatContext) {
        avformat_close_input(&m_formatContext);
    }
    m_audioStreamIndex = -1;
    m_eof = false;
    m_rawDSD = false;
    m_resampleBufferCapacity = 0;
}
```

**Step 2: Build (still expecting errors)**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Still errors in readSamples() and seek()

---

## Task 6: Update DSD Path in readSamples()

**Files:**
- Modify: `src/AudioEngine.cpp:564-693`

**Step 1: Replace all DSD buffer references**

In the DSD section of readSamples() (lines 564-693), replace all occurrences:

| Old | New |
|-----|-----|
| `m_remainingCount` | `m_dsdRemainderCount` |
| `m_remainingSamples` | `m_dsdRemainderBuffer` |

Specifically update:
- Line 564: `if (m_remainingCount > 0)` → `if (m_dsdRemainderCount > 0)`
- Line 565: `size_t remainingPerCh = m_remainingCount / 2;` → `size_t remainingPerCh = m_dsdRemainderCount / 2;`
- Lines 568-573: All `m_remainingSamples` → `m_dsdRemainderBuffer`
- Lines 576-583: All `m_remainingCount` → `m_dsdRemainderCount`, `m_remainingSamples` → `m_dsdRemainderBuffer`
- Lines 634-642: All references to `m_remainingSamples` and `m_remainingCount`

**Step 2: Build to check DSD path compiles**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Errors should now only be in PCM path (lines 729+)

---

## Task 7: Update seek() for FIFO Reset

**Files:**
- Modify: `src/AudioEngine.cpp:1568-1637`

**Step 1: Update DSD seek path**

At line 1595, change:
```cpp
        m_remainingCount = 0;
```
To:
```cpp
        m_dsdRemainderCount = 0;
```

**Step 2: Update PCM seek path**

At line 1631, change:
```cpp
    m_remainingCount = 0;
```
To:
```cpp
    // Clear DSD remainder
    m_dsdRemainderCount = 0;
    // Reset PCM FIFO
    if (m_pcmFifo) {
        av_audio_fifo_reset(m_pcmFifo);
    }
```

**Step 3: Build to check seek compiles**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Errors only in PCM readSamples() path now

---

## Task 8: Update initResampler() for FIFO Allocation

**Files:**
- Modify: `src/AudioEngine.cpp:959-1019`

**Step 1: Add FIFO allocation after resampler init**

After line 1016 (`return true;`), but before the function closes, insert FIFO allocation. Replace the entire function:

```cpp
bool AudioDecoder::initResampler(uint32_t outputRate, uint32_t outputBits) {
    // Don't resample DSD!
    if (m_trackInfo.isDSD) {
        std::cout << "[AudioDecoder] DSD: No resampling, native passthrough" << std::endl;
        return true;
    }

    // Free existing resampler
    if (m_swrContext) {
        swr_free(&m_swrContext);
    }

    // Free existing FIFO if format changed
    if (m_pcmFifo) {
        av_audio_fifo_free(m_pcmFifo);
        m_pcmFifo = nullptr;
    }

    // Determine output format
    AVSampleFormat outFormat;
    switch (outputBits) {
        case 16:
            outFormat = AV_SAMPLE_FMT_S16;
            break;
        case 24:
        case 32:
            outFormat = AV_SAMPLE_FMT_S32;
            break;
        default:
            outFormat = AV_SAMPLE_FMT_S32;
            break;
    }

    // Allocate resampler with new API
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout, m_codecContext->ch_layout.nb_channels);
    av_channel_layout_default(&outLayout, m_codecContext->ch_layout.nb_channels);

    int ret = swr_alloc_set_opts2(
        &m_swrContext,
        &outLayout,
        outFormat,
        outputRate,
        &inLayout,
        m_codecContext->sample_fmt,
        m_codecContext->sample_rate,
        0,
        nullptr
    );

    if (ret < 0 || !m_swrContext) {
        std::cerr << "[AudioDecoder] Failed to allocate resampler" << std::endl;
        return false;
    }

    // Initialize resampler
    if (swr_init(m_swrContext) < 0) {
        std::cerr << "[AudioDecoder] Failed to initialize resampler" << std::endl;
        swr_free(&m_swrContext);
        return false;
    }

    // Allocate PCM FIFO for output format
    m_pcmFifo = av_audio_fifo_alloc(outFormat, m_trackInfo.channels, 8192);
    if (!m_pcmFifo) {
        std::cerr << "[AudioDecoder] Failed to allocate PCM FIFO" << std::endl;
        swr_free(&m_swrContext);
        return false;
    }

    std::cout << "[AudioDecoder] Resampler: " << m_codecContext->sample_rate
              << "Hz -> " << outputRate << "Hz, " << outputBits << "bit" << std::endl;

    return true;
}
```

**Step 2: Build to check initResampler compiles**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: Errors only in PCM readSamples() path (lines 729-957)

---

## Task 9: Refactor PCM Path - Remove Old Buffer Code

**Files:**
- Modify: `src/AudioEngine.cpp:729-750`

**Step 1: Replace old m_remainingCount drain with FIFO drain**

Replace lines 729-750 (the old "drain remaining samples" code):

```cpp
    // CRITICAL FIX: D'abord, utiliser les samples restants du buffer interne
    if (m_remainingCount > 0) {
        size_t samplesToUse = std::min(m_remainingCount, numSamples);
        memcpy_audio(outputPtr, m_remainingSamples.data(), samplesToUse * bytesPerSample);
        outputPtr += samplesToUse * bytesPerSample;
        totalSamplesRead += samplesToUse;

        // S'il reste encore des samples dans le buffer interne, les décaler
        if (samplesToUse < m_remainingCount) {
            size_t remaining = m_remainingCount - samplesToUse;
            memmove(m_remainingSamples.data(),
                    m_remainingSamples.data() + samplesToUse * bytesPerSample,
                    remaining * bytesPerSample);
            m_remainingCount = remaining;
        } else {
            m_remainingCount = 0;
        }

        // Si on a déjà assez de samples, retourner maintenant
        if (totalSamplesRead >= numSamples) {
            return totalSamplesRead;
        }
    }
```

With:

```cpp
    // Drain PCM FIFO first (O(1) circular read, replaces O(n) memmove)
    if (m_pcmFifo) {
        int fifoSamples = av_audio_fifo_size(m_pcmFifo);
        if (fifoSamples > 0) {
            int samplesToRead = std::min(fifoSamples, (int)(numSamples - totalSamplesRead));
            uint8_t* outPtrs[1] = { outputPtr };

            int samplesRead = av_audio_fifo_read(m_pcmFifo, (void**)outPtrs, samplesToRead);
            if (samplesRead > 0) {
                outputPtr += samplesRead * bytesPerSample;
                totalSamplesRead += samplesRead;
            }

            if (totalSamplesRead >= numSamples) {
                return totalSamplesRead;
            }
        }
    }
```

**Step 2: Build to check FIFO drain compiles**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: May still have errors in resample/passthrough excess code

---

## Task 10: Add Direct Write Path for Resample

**Files:**
- Modify: `src/AudioEngine.cpp:858-918`

**Step 1: Replace resample path with direct write optimization**

Replace the resample section (approximately lines 858-918) with:

```cpp
                if (m_swrContext) {
                    // Get maximum possible output samples from this frame
                    int64_t maxOutput = swr_get_out_samples(m_swrContext, frameSamples);

                    if (maxOutput <= (int64_t)samplesNeeded) {
                        // DIRECT PATH: write straight to output buffer (no temp buffer needed)
                        uint8_t* outPtrs[1] = { outputPtr };

                        int convertedSamples = swr_convert(
                            m_swrContext,
                            outPtrs,
                            maxOutput,
                            (const uint8_t**)m_frame->data,
                            frameSamples
                        );

                        if (convertedSamples > 0) {
                            outputPtr += convertedSamples * bytesPerSample;
                            totalSamplesRead += convertedSamples;
                        }
                    } else {
                        // TEMP BUFFER PATH: need to split output
                        // Reuse member buffer with capacity growth
                        size_t tempBufferSize = maxOutput * bytesPerSample;
                        if (tempBufferSize > m_resampleBufferCapacity) {
                            size_t newCapacity = static_cast<size_t>(tempBufferSize * 1.5);
                            m_resampleBuffer.resize(newCapacity);
                            m_resampleBufferCapacity = m_resampleBuffer.size();
                        }
                        uint8_t* tempPtr = m_resampleBuffer.data();

                        int convertedSamples = swr_convert(
                            m_swrContext,
                            &tempPtr,
                            maxOutput,
                            (const uint8_t**)m_frame->data,
                            frameSamples
                        );

                        if (convertedSamples > 0) {
                            size_t samplesToUse = std::min((size_t)convertedSamples, samplesNeeded);
                            size_t bytesToUse = samplesToUse * bytesPerSample;

                            // Copy needed samples to output
                            memcpy_audio(outputPtr, m_resampleBuffer.data(), bytesToUse);
                            outputPtr += bytesToUse;
                            totalSamplesRead += samplesToUse;

                            // Store excess in FIFO (O(1) write)
                            if ((size_t)convertedSamples > samplesToUse) {
                                size_t excess = convertedSamples - samplesToUse;
                                uint8_t* excessPtr = m_resampleBuffer.data() + bytesToUse;
                                uint8_t* excessPtrs[1] = { excessPtr };

                                av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
                            }
                        }
                    }
```

**Step 2: Build to check direct write path compiles**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X 2>&1 | head -50`
Expected: May have errors in passthrough path

---

## Task 11: Update Passthrough Path to Use FIFO

**Files:**
- Modify: `src/AudioEngine.cpp:919-944`

**Step 1: Replace passthrough excess storage with FIFO**

Replace the passthrough section (approximately lines 919-944):

```cpp
                } else {
                    // No resampling - direct copy
                    size_t samplesToCopy = std::min(frameSamples, samplesNeeded);
                    size_t bytesToCopy = samplesToCopy * bytesPerSample;

                    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
                    outputPtr += bytesToCopy;
                    totalSamplesRead += samplesToCopy;

                    // CRITICAL: S'il reste des samples dans la frame, les stocker
                    if (frameSamples > samplesToCopy) {
                        size_t excess = frameSamples - samplesToCopy;
                        size_t excessBytes = excess * bytesPerSample;

                        if (m_remainingSamples.size() < excessBytes) {
                            m_remainingSamples.resize(excessBytes);
                        }

                        memcpy_audio(m_remainingSamples.data(),
                               m_frame->data[0] + bytesToCopy,
                               excessBytes);
                        m_remainingCount = excess;

                        std::cout << "[AudioDecoder] Buffering " << excess
                                  << " excess samples (no resampling)" << std::endl;
                    }
                }
```

With:

```cpp
                } else {
                    // No resampling - direct copy
                    size_t samplesToCopy = std::min(frameSamples, samplesNeeded);
                    size_t bytesToCopy = samplesToCopy * bytesPerSample;

                    memcpy_audio(outputPtr, m_frame->data[0], bytesToCopy);
                    outputPtr += bytesToCopy;
                    totalSamplesRead += samplesToCopy;

                    // Store excess in FIFO (O(1) write, replaces manual buffer)
                    if (frameSamples > samplesToCopy && m_pcmFifo) {
                        size_t excess = frameSamples - samplesToCopy;
                        uint8_t* excessPtr = m_frame->data[0] + bytesToCopy;
                        uint8_t* excessPtrs[1] = { excessPtr };

                        av_audio_fifo_write(m_pcmFifo, (void**)excessPtrs, excess);
                    }
                }
```

**Step 2: Build to verify full compilation**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X clean && make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X`
Expected: Build succeeds with no errors

**Step 3: Commit all .cpp changes**

```bash
git add src/AudioEngine.cpp
git commit -m "feat: implement AVAudioFifo and direct write optimization

- Replace O(n) memmove with O(1) FIFO for PCM overflow
- Add direct write path when samplesNeeded >= maxOutput
- Update DSD path to use renamed buffer members
- Add FIFO lifecycle management (init, seek, close)

Eliminates 1-2 memcpy operations per frame in common case.

```

---

## Task 12: Functional Testing - PCM Playback

**Files:**
- None (manual testing)

**Step 1: Build the project**

Run: `make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X clean && make -C /Users/mac/Downloads/MemoryPlayControllerSDK/DirettaRendererUPnP-X`
Expected: Build succeeds

**Step 2: Test 44.1kHz/16-bit FLAC**

Play a standard CD-quality FLAC file. Verify:
- Playback starts without errors
- Audio sounds correct (no clicks, pops, or distortion)
- Seek works (jump to middle of track)
- Track completes without hanging

**Step 3: Test 96kHz/24-bit FLAC**

Play a high-resolution FLAC file. Verify:
- Resampler initializes correctly (check log output)
- Audio plays correctly
- Seek works

**Step 4: Test gapless playback (same format)**

Queue two tracks of the same format. Verify:
- Transition is seamless
- No audio artifacts at transition point

---

## Task 13: Functional Testing - DSD Playback

**Files:**
- None (manual testing)

**Step 1: Test DSD64 DSF file**

Play a DSD64 .dsf file. Verify:
- DSD native mode activates (check log: "DSD NATIVE MODE ACTIVATED")
- Audio plays correctly
- DSD remainder buffer works (no audio glitches)

**Step 2: Test DSD128 DFF file**

Play a DSD128 .dff file. Verify:
- Bit reversal works (DFF is MSB first)
- Audio plays correctly

**Step 3: Test seek in DSD**

Seek to middle of DSD track. Verify:
- Seek completes without errors
- Playback resumes correctly
- DSD remainder buffer is cleared

---

## Task 14: Edge Case Testing

**Files:**
- None (manual testing)

**Step 1: Test format change between tracks**

Queue a 44.1kHz track followed by a 96kHz track. Verify:
- Format change is detected (check log: "FORMAT CHANGE DETECTED")
- Stop/start sequence executes cleanly
- Second track plays correctly

**Step 2: Test rapid seek**

Seek multiple times rapidly. Verify:
- FIFO reset works correctly
- No stale audio
- No crashes or hangs

**Step 3: Test stop/restart**

Stop playback mid-track, then restart. Verify:
- FIFO is cleared on stop
- Restart begins from beginning
- No leftover audio from previous playback

---

## Summary

**Files modified:**
- `src/AudioEngine.h` - Added AVAudioFifo include, renamed DSD buffer members, added m_pcmFifo member
- `src/AudioEngine.cpp` - Refactored PCM path, added FIFO lifecycle, added direct write optimization

**Performance improvements:**
- Eliminated O(n) memmove in PCM path (replaced with O(1) FIFO read)
- Added direct write path (skips temp buffer when possible)
- Reduced memory copies by 1-2 per frame in common case

**Risk assessment:**
- Low: DSD path unchanged (just renamed members)
- Medium: PCM path restructured (thorough testing required)
- Low: FIFO lifecycle tied to existing lifecycle hooks
