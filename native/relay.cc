#include "relay.hh"

#include <android/log.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "usb_device.hh"

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AOAS", __VA_ARGS__)

namespace aoas {
namespace {

// One drain is a few milliseconds of audio at any sane format: big enough that
// the loop is not syscall-bound, small enough that stop() is prompt and a
// format change does not sit behind a huge in-flight chunk.
constexpr size_t kScratchBytes = 16 * 1024;

// How long to wait when there is nothing to move -- either the client has not
// written yet or the driver's ring is full. Short enough that we never keep the
// driver waiting on us, long enough not to spin a core flat while a phone is in
// someone's pocket. The driver buffers seconds ahead, so this is slack, not
// latency.
constexpr long kIdleNanos = 500 * 1000;  // 0.5 ms

void napBriefly() {
    struct timespec ts { 0, kIdleNanos };
    nanosleep(&ts, nullptr);
}

}  // namespace

Relay::~Relay() { stop(); }

void Relay::start(UsbDevice* device, ShmRing ring, int frameBytes) {
    stop();
    if (!device || !ring.valid() || frameBytes <= 0) return;

    device_     = device;
    ring_       = ring;
    frameBytes_ = frameBytes;

    // Round down to a whole number of frames so a drain never splits one.
    size_t chunk = (kScratchBytes / static_cast<size_t>(frameBytes)) *
                   static_cast<size_t>(frameBytes);
    if (chunk == 0) chunk = static_cast<size_t>(frameBytes);
    scratch_.assign(chunk, 0);

    running_.store(true, std::memory_order_release);
    thread_ = std::thread(&Relay::run, this);
}

void Relay::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) {
        if (thread_.joinable()) thread_.join();
        return;
    }
    if (thread_.joinable()) thread_.join();
    device_ = nullptr;
    ring_ = ShmRing{};
}

void Relay::run() {
    // Just under the driver's own transfer thread (-19). This thread only has
    // to keep a ring measured in seconds from running dry, so it does not need
    // to outrank the thread with the actual isochronous deadline -- but it must
    // outrank ordinary app work, or a busy foreground app starves it.
    // Not SCHED_FIFO: that needs privileges a normal app does not have, and
    // asking for them and failing silently would be worse than not asking.
    setpriority(PRIO_PROCESS, 0, -16);

    LOGI("relay: started (%d bytes/frame, %zu-byte drains)",
         frameBytes_, scratch_.size());

    // Bytes pulled from shared memory that the driver has not accepted yet.
    // They are never dropped, only retried -- see the header.
    size_t pending = 0;
    size_t pendingOffset = 0;

    while (running_.load(std::memory_order_acquire)) {
        if (pending == 0) {
            pending = ring_.read(scratch_.data(), scratch_.size());
            pendingOffset = 0;
            if (pending == 0) {          // client has written nothing yet
                napBriefly();            // the driver emits silence meanwhile
                continue;
            }
        }

        const int accepted = device_->write(scratch_.data() + pendingOffset,
                                            static_cast<int>(pending));
        if (accepted <= 0) {             // driver ring full: hold, do not drop
            napBriefly();
            continue;
        }

        pendingOffset += static_cast<size_t>(accepted);
        pending       -= static_cast<size_t>(accepted);
    }

    LOGI("relay: stopped");
}

}  // namespace aoas
