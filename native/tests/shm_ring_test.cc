// shm_ring_test — the bit-exactness contract for the cross-process ring.
//
// AOAS's whole claim is that audio arrives at the DAC as the exact bytes the
// client wrote. The ring is the only thing between them, so it is the only
// thing that can break that claim. This test is the null test for it: a known
// pseudorandom byte stream goes in, and the identical stream must come out --
// through thousands of wraparounds, at chunk sizes that never line up with the
// capacity, with producer and consumer racing on separate threads.
//
// Runs on the desktop (no Android, no NDK): the ring only needs a MAP_SHARED
// region, and an anonymous one exercises the same code an ASharedMemory fd
// would. Build and run it with:
//   c++ -std=c++17 -O2 -fsanitize=address,undefined -pthread
//       native/tests/shm_ring_test.cc -o /tmp/shm_ring_test && /tmp/shm_ring_test

#include <sys/mman.h>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "../shm_ring.hh"

namespace {

// Deterministic, cheap, and not std::rand (whose sequence is implementation
// defined -- the point is that both sides agree on the expected bytes).
struct Lcg {
    uint64_t s;
    uint32_t next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<uint32_t>(s >> 33);
    }
};

void* mapShared(size_t bytes) {
    void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    assert(p != MAP_FAILED);
    return p;
}

int failures = 0;
void check(bool cond, const char* what) {
    if (!cond) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// --- 1. bit-exact round trip under contention -------------------------------
void testRoundTrip() {
    // A capacity that is neither a power of two nor a multiple of the chunk
    // sizes below, so wraps land mid-chunk and split every memcpy.
    constexpr size_t kCapacity = 4093;
    constexpr size_t kTotal    = 8 * 1024 * 1024;   // ~2000 wraps

    const size_t region = aoas::shmRingRegionBytes(kCapacity);
    void* base = mapShared(region);
    aoas::ShmRing ring = aoas::ShmRing::create(base, region, /*frameBytes=*/8);
    check(ring.valid(), "create() produced a valid ring");
    check(ring.capacity() == kCapacity, "capacity survives create()");

    std::atomic<bool> producerDone{false};

    std::thread producer([&] {
        Lcg rng{0x9E3779B97F4A7C15ULL};
        Lcg chunk{12345};
        std::vector<uint8_t> buf(1500);
        size_t sent = 0;
        while (sent < kTotal) {
            size_t n = 1 + chunk.next() % buf.size();
            if (n > kTotal - sent) n = kTotal - sent;
            for (size_t i = 0; i < n; ++i) buf[i] = static_cast<uint8_t>(rng.next());
            // Short writes mean full: retry, never drop. A dropped byte here
            // would be a gap in the audio.
            size_t off = 0;
            while (off < n) {
                size_t w = ring.write(buf.data() + off, n - off);
                if (w == 0) std::this_thread::yield();
                off += w;
            }
            sent += n;
        }
        producerDone.store(true, std::memory_order_release);
    });

    std::thread consumer([&] {
        Lcg expect{0x9E3779B97F4A7C15ULL};
        Lcg chunk{67890};
        std::vector<uint8_t> buf(2000);
        size_t got = 0;
        size_t mismatches = 0;
        while (got < kTotal) {
            size_t n = 1 + chunk.next() % buf.size();
            size_t r = ring.read(buf.data(), n);
            if (r == 0) {
                if (producerDone.load(std::memory_order_acquire) &&
                    ring.available() == 0 && got < kTotal) {
                    break;  // producer finished but bytes went missing
                }
                std::this_thread::yield();
                continue;
            }
            for (size_t i = 0; i < r; ++i) {
                if (buf[i] != static_cast<uint8_t>(expect.next())) ++mismatches;
            }
            got += r;
        }
        check(got == kTotal, "every byte written came back out");
        check(mismatches == 0, "every byte came back BIT-EXACT");
    });

    producer.join();
    consumer.join();
    munmap(base, region);
}

// --- 2. attach() validates, and a hostile peer cannot move our memcpy -------
void testUntrustedPeer() {
    constexpr size_t kCapacity = 1024;
    const size_t region = aoas::shmRingRegionBytes(kCapacity);
    void* base = mapShared(region);

    // Garbage region: attach must refuse rather than invent a geometry.
    std::memset(base, 0xAB, region);
    check(!aoas::ShmRing::attach(base, region).valid(),
          "attach() rejects a region with no valid header");

    aoas::ShmRing::create(base, region, 8);
    aoas::ShmRing peer = aoas::ShmRing::attach(base, region);
    check(peer.valid(), "attach() accepts a region create() stamped");

    // The producer claims it wrote far past the end of the mapping. If the
    // ring trusted that, the memcpy below would read off the end -- with ASan
    // on, that is a crash, which is the point of testing it.
    auto* h = static_cast<aoas::ShmRingHeader*>(base);
    h->writePos.store(0xFFFFFFFFu, std::memory_order_release);
    std::vector<uint8_t> out(kCapacity);
    check(peer.read(out.data(), out.size()) == 0,
          "an out-of-range peer writePos yields no data instead of a fault");
    check(peer.available() == 0, "available() is 0 when the peer index is corrupt");

    h->writePos.store(static_cast<uint32_t>(kCapacity), std::memory_order_release);
    check(peer.read(out.data(), out.size()) == 0,
          "writePos == capacity is out of range too (indices are < capacity)");

    munmap(base, region);
}

// --- 3. the reserved byte: full is distinguishable from empty ---------------
void testFullVersusEmpty() {
    constexpr size_t kCapacity = 64;
    const size_t region = aoas::shmRingRegionBytes(kCapacity);
    void* base = mapShared(region);
    aoas::ShmRing ring = aoas::ShmRing::create(base, region, 4);

    check(ring.available() == 0, "a fresh ring is empty");
    check(ring.freeSpace() == kCapacity - 1, "one byte is reserved to tell full from empty");

    std::vector<uint8_t> in(kCapacity, 0x5A);
    size_t w = ring.write(in.data(), in.size());
    check(w == kCapacity - 1, "write() fills to capacity - 1 and no further");
    check(ring.freeSpace() == 0, "a full ring reports no free space");
    check(ring.available() == kCapacity - 1, "a full ring is not mistaken for empty");
    check(ring.write(in.data(), 1) == 0, "writing to a full ring accepts nothing");

    std::vector<uint8_t> out(kCapacity);
    check(ring.read(out.data(), out.size()) == kCapacity - 1, "read() drains it");
    check(ring.available() == 0, "drained ring is empty again");

    munmap(base, region);
}

}  // namespace

int main() {
    testRoundTrip();
    testUntrustedPeer();
    testFullVersusEmpty();
    if (failures == 0) std::puts("shm_ring_test: all checks passed");
    return failures == 0 ? 0 : 1;
}
