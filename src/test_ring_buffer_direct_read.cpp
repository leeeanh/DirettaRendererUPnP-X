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
    ring.resize(64, 0x00);  // Small buffer to force wraparound

    // Fill most of buffer
    uint8_t input[60];
    ring.push(input, 60);

    // Pop most data to move read position near end
    uint8_t output[50];
    ring.pop(output, 50);

    // Push more data that wraps around
    ring.push(input, 40);

    // Now data wraps: 10 bytes at end + 40 bytes at start
    // Requesting 30 contiguous should fail (only 10 at end before wrap)
    const uint8_t* region;
    size_t available;
    bool success = ring.getDirectReadRegion(30, region, available);

    // Should return false because contiguous region < 30 (only 10 bytes to end)
    assert(!success && "Should fail when data wraps");

    std::cout << "test_getDirectReadRegion_wraparound_returns_false PASSED" << std::endl;
}

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

int main() {
    test_getDirectReadRegion_basic();
    test_getDirectReadRegion_insufficient_data();
    test_getDirectReadRegion_wraparound_returns_false();
    test_advanceReadPos_basic();
    test_advanceReadPos_full_consume();

    std::cout << "\nAll direct read tests PASSED!" << std::endl;
    return 0;
}
