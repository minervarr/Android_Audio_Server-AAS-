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

    // Throw away everything buffered between the client and the DAC -- what is
    // still in the shared ring, what this thread has pulled out but not handed
    // over, and the seconds already sitting in the driver's own ring -- without
    // stopping the relay and without touching the isochronous stream.
    //
    // The work happens ON the relay thread, not the caller's. That is the whole
    // design: the ring and the driver are otherwise touched by exactly one
    // thread, and a flush that reached in from Binder would be the first thing
    // to break that. The caller blocks until the loop has honoured the request,
    // because "discarded" has to be true before the client writes its next
    // track -- see IAoas.flush(), which is synchronous for the same reason.
    //
    // Returns false if the relay is not running, or if the request was not
    // honoured within `timeoutMs`.
    bool flush(int timeoutMs = 50);

    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{false};
    // Set by flush(), cleared by the relay thread once the discard is done.
    std::atomic<bool> flushRequest_{false};
    UsbDevice* device_ = nullptr;
    ShmRing ring_;
    int frameBytes_ = 4;
    std::vector<uint8_t> scratch_;
};

}  // namespace aoas

#endif  // AOAS_RELAY_HH
