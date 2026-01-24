/**
 * @file AudioTiming.h
 * @brief Audio timing constants for stable playback
 *
 * Shared constants for quantized chunk sizes and jitter buffer targets.
 * Eliminates timing variance by using fixed sizes per sample rate tier.
 */

#ifndef AUDIO_TIMING_H
#define AUDIO_TIMING_H

#include <cstddef>

namespace AudioTiming {
    // Quantized chunk sizes (samples)
    // Fixed sizes eliminate the thousands of unique chunk sizes from adaptive sizing
    constexpr size_t PCM_CHUNK_LOW = 2048;    // ≤48kHz
    constexpr size_t PCM_CHUNK_MID = 4096;    // 88.2-96kHz
    constexpr size_t PCM_CHUNK_HIGH = 8192;   // ≥176.4kHz
    constexpr size_t DSD_CHUNK = 32768;       // All DSD rates

    // Jitter buffer targets (milliseconds)
    // Compressed formats need more buffering to absorb decode time variance
    constexpr int JITTER_TARGET_COMPRESSED = 200;   // FLAC, ALAC
    constexpr int JITTER_TARGET_UNCOMPRESSED = 100; // WAV, AIFF
}

#endif // AUDIO_TIMING_H
