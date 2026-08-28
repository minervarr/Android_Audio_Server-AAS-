#ifndef AOAS_SERVICE_HH
#define AOAS_SERVICE_HH

// AoasServer — the ownership state machine. One instance per process, living as
// long as the foreground service.
//
// Everything here answers one of two questions:
//   who is allowed to write to the DAC right now, and
//   how does that stop being true without the audio stopping too.
//
// Ownership changes hands cooperatively (docs/design.md rule 4): the outgoing
// owner releases, we confirm, and only then can anyone acquire. There is no
// call that takes the device from a live client, because a forced takeover
// mid-stream would be exactly the audible interruption this project exists to
// remove. Two things are allowed to break that, and both are honest: the user
// pressing disconnect in our own notification, and the owner's process dying.
//
// The USB stream is not part of this state machine. It belongs to UsbDevice and
// stays open across every transition here.
//
// --- why this class knows nothing about Binder -------------------------------
//
// The AIDL interface is generated with the Java backend and implemented by a
// stub that does nothing but call straight down here (see AoasBinder.java). The
// NDK backend was the first choice -- it would have kept the whole interface in
// C++ -- but the NDK ships only libbinder_ndk's C headers; the C++ ones the
// generated code needs (binder_interface_utils.h and friends) exist only in the
// platform build. Vendoring them into an app would be betting on
// platform-internal headers staying in step with the device's .so.
//
// It costs nothing that matters. Binder never carries a sample: audio moves
// through shared memory, and Binder only carries acquire/release/status. What
// the Java backend buys is real -- every client app gets generated, versioned
// stubs instead of hand-rolling the wire format, which for a hub serving
// several independently updated apps is the difference between an interface and
// a standing invitation to bugs.
//
// So: Java owns the client callback objects and their death recipients, because
// that is where the IBinder lives. This class owns everything with an opinion
// about audio, and tells Java when to fire a callback.

#include <sys/types.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include "relay.hh"
#include "shm_ring.hh"
#include "usb_device.hh"

namespace aoas {

// Mirrors the ERR_* constants in IAoas.aidl. acquire() returns the negated
// value so one int carries both "here is your descriptor" and "here is why not".
enum AcquireError {
    kErrBusy              = 1,
    kErrNoDevice          = 2,
    kErrFormatUnsupported = 3,
    kErrShmFailed         = 4,
};

// Mirrors the REASON_* constants in IAoasClient.aidl.
enum LostReason {
    kReasonUserDisconnected = 1,
    kReasonDeviceDetached   = 2,
    kReasonServerStopping   = 3,
};

class AoasServer {
public:
    AoasServer() = default;
    ~AoasServer();

    AoasServer(const AoasServer&) = delete;
    AoasServer& operator=(const AoasServer&) = delete;

    // Called when ownership ends for a reason the owner did not ask for. The
    // JNI layer forwards it to the Java-side IAoasClient. Never called while
    // holding a lock the callback could need.
    void setOwnershipLostHandler(std::function<void(int reason)> fn);

    // --- ownership ----------------------------------------------------------

    // Become the active owner. Returns a file descriptor for the shared ring on
    // success, or -kErr* on failure. The descriptor is the caller's to close.
    //
    // Reconfigures the DAC only if `sampleRate`/`channels`/`bitDepth` differ
    // from what is already streaming. An identical format -- the common case,
    // and the whole reason this server exists -- is a silent handover: the
    // isochronous stream is not touched, so the DAC never re-locks its clock.
    int acquire(uid_t callerUid, int sampleRate, int channels, int bitDepth,
                int ringMillis);

    // Give the device up cleanly. False if the caller is not the owner: one
    // client releasing another's ownership would be a forced takeover wearing a
    // different name.
    bool release(uid_t callerUid);

    // The owner's process died. Frees the device with no callback -- there is
    // nobody left to hear it.
    void onOwnerDied();

    // The user pressed disconnect in our notification while a client was still
    // streaming. The single sanctioned interruption of a live owner: a
    // deliberate act, equivalent to unplugging the DAC by hand.
    void forceDisconnect();

    int ownerUid() const;

    // --- device -------------------------------------------------------------

    // The USB permission came through and Java opened the device. `fd` belongs
    // to Java's UsbDeviceConnection, which must stay open while we stream.
    bool onUsbAttached(int fd);

    // The cable came out. The one unavoidable end of the stream.
    void onUsbDetached();

    // --- bring-up diagnostics ----------------------------------------------
    //
    // Reachable only from AoasDebugActivity. Each one refuses while a client
    // owns the device, because all of them touch the stream configuration and
    // taking that from a live owner is the forced interruption rule 4 forbids.

    Caps caps() const { return device_.caps(); }

    // -1 when a client owns the device or the format could not be configured;
    // otherwise the byte count the driver accepted.
    long debugTone(int sampleRate, int channels, int bitDepth, int millis,
                   double hz, double amplitude);

    // -1 when a client owns the device or capture could not be configured;
    // otherwise peak level as a fraction of full scale.
    double debugCapture(int sampleRate, int channels, int bitDepth, int millis,
                        long* framesOut);

    bool deviceReady() const { return device_.ready(); }
    std::string deviceInfo() const { return device_.info(); }
    Format activeFormat() const { return device_.activeFormat(); }
    int pendingPlaybackMs() const { return device_.pendingPlaybackMs(); }

private:
    // Tears ownership down. Caller holds mu_. Returns the reason to report once
    // the lock is dropped, or 0 for none.
    int endOwnershipLocked(int reason);
    void fireLost(int reason);

    mutable std::mutex mu_;

    UsbDevice device_;
    Relay relay_;

    uid_t ownerUid_ = static_cast<uid_t>(-1);   // -1 means the device is free
    void* shmBase_ = nullptr;
    size_t shmBytes_ = 0;
    int shmFd_ = -1;

    std::function<void(int)> onLost_;
};

}  // namespace aoas

#endif  // AOAS_SERVICE_HH

namespace aoas {

// ── The process-wide server, for the in-process console ──────────────────────
//
// AOAS's bring-up console is a NativeActivity in this same process (see
// native/ui/). It needs the running AoasServer and cannot reasonably be handed
// one: NativeActivity is constructed by the platform, and the server is
// constructed by AoasService.onCreate() through JNI, in whichever order the
// user happens to open things.
//
// So the server publishes itself here on construction and withdraws on
// destruction. Deliberately narrow: a raw pointer, no ownership, no locking,
// and only ever touched from the main thread -- both writers are JNI calls on
// AoasService's thread and the only reader is the UI thread, which on Android
// is the same thread. It is NOT a general service locator and nothing in the
// audio path may use it.
void        setProcessServer(AoasServer* s);
AoasServer* processServer();

}  // namespace aoas
