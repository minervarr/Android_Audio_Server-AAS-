#ifndef AOAS_RELAY_HH
#define AOAS_RELAY_HH

// Relay — the thread that moves the current owner's PCM to the DAC.
//
// It is deliberately the least clever file in this project. It reads bytes out
// of the owner's shared ring and hands the same bytes to the driver. There is
// no gain stage, no conversion, no mixing and no rate matching, and there is
// nowhere for one to be added later without it being obvious in this loop.
//
// Two properties matter more than speed:
//
//   Nothing is dropped. When the driver's ring is momentarily full, the bytes
//   already pulled out of shared memory are held and retried, never discarded.
//   A dropped chunk is a gap in the audio, which is the failure this project
//   exists to prevent -- and unlike an underrun, the driver cannot paper over
//   it with silence, because it never learns the bytes existed.
//
//   Stopping is clean. stop() joins the thread before the caller unmaps the
//   ring or reconfigures the device, so nothing is reading a mapping that is
//   being torn down.

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

#include "shm_ring.hh"

namespace aoas {

class UsbDevice;

class Relay {
public:
    Relay() = default;
    ~Relay();

    Relay(const Relay&) = delete;
    Relay& operator=(const Relay&) = delete;

    // Begin draining `ring` into `device`. `frameBytes` keeps every transfer a
    // whole number of frames; a chunk split mid-frame would swap the channels
    // for everything after it.
    void start(UsbDevice* device, ShmRing ring, int frameBytes);

    // Stop and join. Safe to call when not running.
    void stop();

    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    UsbDevice* device_ = nullptr;
    ShmRing ring_;
    int frameBytes_ = 4;
    std::vector<uint8_t> scratch_;
};

}  // namespace aoas

#endif  // AOAS_RELAY_HH
