#include "aoas_service.hh"

#include <android/log.h>
#include <android/sharedmem.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AOAS", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AOAS", __VA_ARGS__)

namespace aoas {
namespace {

// Ring size bounds. The floor is there because a ring smaller than a couple of
// isochronous transfers cannot absorb a single scheduling hiccup; the ceiling
// because this is shared, resident memory and a client asking for a minute of
// it is a bug, not a preference.
constexpr int kMinRingMillis     = 20;
constexpr int kMaxRingMillis     = 4000;
constexpr int kDefaultRingMillis = 500;

constexpr uid_t kNoOwner = static_cast<uid_t>(-1);

}  // namespace

AoasServer::~AoasServer() {
    int reason;
    {
        std::lock_guard<std::mutex> lock(mu_);
        reason = endOwnershipLocked(kReasonServerStopping);
    }
    fireLost(reason);
    device_.detach();
}

void AoasServer::setOwnershipLostHandler(std::function<void(int)> fn) {
    std::lock_guard<std::mutex> lock(mu_);
    onLost_ = std::move(fn);
}

int AoasServer::acquire(uid_t callerUid, int sampleRate, int channels,
                        int bitDepth, int ringMillis) {
    std::lock_guard<std::mutex> lock(mu_);

    if (ownerUid_ != kNoOwner) {
        // Never a forced takeover. The caller is told who has it (via
        // ownerUid()) so it can say something useful instead of retrying blind.
        LOGI("acquire refused: uid %d already owns the device", ownerUid_);
        return -kErrBusy;
    }

    // Reconfigures only if the format actually differs; an identical format
    // leaves the isochronous stream untouched.
    if (!device_.ensureFormat(sampleRate, channels, bitDepth)) {
        return device_.info().empty() ? -kErrNoDevice : -kErrFormatUnsupported;
    }
    const Format fmt = device_.activeFormat();
    if (!fmt.valid()) return -kErrNoDevice;

    const int millis = std::clamp(ringMillis > 0 ? ringMillis : kDefaultRingMillis,
                                  kMinRingMillis, kMaxRingMillis);
    // Whole frames only: a capacity that split a frame would rotate the
    // channels on every wraparound.
    size_t capacity = static_cast<size_t>(fmt.frameBytes()) *
                      (static_cast<size_t>(fmt.sampleRate) *
                       static_cast<size_t>(millis) / 1000);
    if (capacity == 0) capacity = static_cast<size_t>(fmt.frameBytes());
    const size_t regionBytes = shmRingRegionBytes(capacity);

    const int fd = ASharedMemory_create("aoas-ring", regionBytes);
    if (fd < 0) {
        LOGE("ASharedMemory_create(%zu) failed", regionBytes);
        return -kErrShmFailed;
    }
    void* base = mmap(nullptr, regionBytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (base == MAP_FAILED) {
        LOGE("mmap of the ring failed");
        ::close(fd);
        return -kErrShmFailed;
    }
    ShmRing ring = ShmRing::create(base, regionBytes,
                                   static_cast<uint32_t>(fmt.frameBytes()));
    if (!ring.valid()) {
        munmap(base, regionBytes);
        ::close(fd);
        return -kErrShmFailed;
    }

    // The client gets its own descriptor for the same region: the one it
    // receives is closed when the Binder transaction finishes, and ours has to
    // outlive that. The mapping is what keeps the memory alive either way.
    const int clientFd = ::dup(fd);
    if (clientFd < 0) {
        munmap(base, regionBytes);
        ::close(fd);
        return -kErrShmFailed;
    }

    ownerUid_ = callerUid;
    shmBase_  = base;
    shmBytes_ = regionBytes;
    shmFd_    = fd;

    relay_.start(&device_, ring, fmt.frameBytes());

    LOGI("uid %d acquired the device: %d Hz / %d ch / %d bit, %zu-byte ring (%d ms)",
         ownerUid_, fmt.sampleRate, fmt.channels, fmt.bitDepth, capacity, millis);
    return clientFd;
}

bool AoasServer::release(uid_t callerUid) {
    std::lock_guard<std::mutex> lock(mu_);
    if (ownerUid_ == kNoOwner) return true;       // idempotent by design
    if (callerUid != ownerUid_) {
        LOGE("uid %d tried to release a device owned by uid %d", callerUid, ownerUid_);
        return false;
    }
    LOGI("uid %d released the device", ownerUid_);
    endOwnershipLocked(0);   // it asked; it does not need telling
    return true;
}

void AoasServer::onOwnerDied() {
    std::lock_guard<std::mutex> lock(mu_);
    if (ownerUid_ == kNoOwner) return;
    LOGI("owner uid %d died; freeing the device", ownerUid_);
    endOwnershipLocked(0);   // nobody left to hear a callback
}

void AoasServer::forceDisconnect() {
    int reason;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ownerUid_ == kNoOwner) return;
        LOGI("user disconnected uid %d by hand", ownerUid_);
        reason = endOwnershipLocked(kReasonUserDisconnected);
    }
    fireLost(reason);
}

int AoasServer::ownerUid() const {
    std::lock_guard<std::mutex> lock(mu_);
    return ownerUid_ == kNoOwner ? -1 : static_cast<int>(ownerUid_);
}

bool AoasServer::onUsbAttached(int fd) { return device_.attach(fd); }

void AoasServer::onUsbDetached() {
    int reason;
    {
        std::lock_guard<std::mutex> lock(mu_);
        LOGI("device detached");
        reason = endOwnershipLocked(kReasonDeviceDetached);
    }
    fireLost(reason);
    device_.detach();
}

int AoasServer::endOwnershipLocked(int reason) {
    if (ownerUid_ == kNoOwner) return 0;

    // Order matters: the relay is joined first, so nothing is reading the
    // shared mapping by the time it is unmapped.
    relay_.stop();

    ownerUid_ = kNoOwner;

    if (shmBase_) {
        munmap(shmBase_, shmBytes_);
        shmBase_  = nullptr;
        shmBytes_ = 0;
    }
    if (shmFd_ >= 0) {
        ::close(shmFd_);
        shmFd_ = -1;
    }

    // The USB stream is deliberately NOT touched. It keeps running, and the
    // driver keeps padding it with silence, until somebody else acquires --
    // that is the entire point of AOAS.
    return reason;
}

void AoasServer::fireLost(int reason) {
    // Called with mu_ released: telling a client it lost the device means a
    // Binder call into another process, and holding the server's lock across
    // that would let a slow -- or malicious -- client stall every other caller.
    if (reason == 0) return;
    std::function<void(int)> fn;
    {
        std::lock_guard<std::mutex> lock(mu_);
        fn = onLost_;
    }
    if (fn) fn(reason);
}

// --- bring-up diagnostics ---------------------------------------------------

long AoasServer::debugTone(int sampleRate, int channels, int bitDepth,
                           int millis, double hz, double amplitude) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        // A tone reconfigures the stream. Doing that under a live owner would
        // be a forced takeover, which is the one thing ownership transfer is
        // built to prevent -- so the debug UI is told no, not the owner.
        if (ownerUid_ != static_cast<uid_t>(-1)) return -1;
    }
    return device_.playTestTone(sampleRate, channels, bitDepth, millis, hz, amplitude);
}

double AoasServer::debugCapture(int sampleRate, int channels, int bitDepth,
                                int millis, long* framesOut) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (ownerUid_ != static_cast<uid_t>(-1)) return -1.0;
    }
    return device_.captureProbe(sampleRate, channels, bitDepth, millis, framesOut);
}

}  // namespace aoas

namespace aoas {

namespace { AoasServer* g_processServer = nullptr; }

void        setProcessServer(AoasServer* s) { g_processServer = s; }
AoasServer* processServer()                 { return g_processServer; }

}  // namespace aoas
