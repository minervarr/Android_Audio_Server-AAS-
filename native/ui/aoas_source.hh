#pragma once
#include <string>
#include <vector>

// ── What the console reads, and the only thing it may command ───────────────
//
// The bring-up console used to BE the Activity: it called AoasNative's statics
// directly, because it lived in the service's own process. It no longer does —
// the UI is its own process now (see AndroidManifest.xml, android:process
// ":ui"), so that everything Vulkan does happens somewhere that cannot stall
// the thread feeding the USB endpoint. CLAUDE.md's argument for a dedicated
// minimal audio process is exactly this one, and it would be undone by drawing
// a GPU frame inside it.
//
// Which leaves the console needing a way to ask questions across a process
// boundary, and this is it. The interface is deliberately narrow and the
// implementations are two:
//
//   BinderSource — the real one. Binds AoasService and asks it.
//   FakeSource   — a scripted device, for the desktop build.
//
// The desktop half is not a courtesy. Every layout question this console has
// — does a 30-alt-setting format list fit, what does it look like with no
// capture path, how does a 12-line error read — is answerable in a second on a
// Wayland window and takes an install cycle and a plugged-in DAC on a phone.
// navaLauncher splits the same way and for the same reason.

struct AoasFormat {
    int sampleRate = 0;
    int channels   = 0;
    int bitDepth   = 0;
    // Bytes on the wire per sample, which is NOT bitDepth/8 in general: 24-bit
    // is commonly carried in 4-byte subslots. Reported as part of the tuple
    // because the three axes are coupled — see CLAUDE.md on why this is not
    // three independent lists.
    int subslotBytes = 0;
};

// One row of Android's own view of the USB bus. The console reports this
// separately from what the driver parsed, because "Android sees a device but
// we have no permission" and "we have permission but the descriptors are not
// what we expected" are different faults with different fixes, and collapsing
// them into one line is how a bring-up console starts lying.
struct UsbBusEntry {
    std::string name;          // manufacturer + product, as Android reports it
    int  vendorId  = 0;
    int  productId = 0;
    int  interfaces = 0;
    int  endpoints  = 0;
    bool permissionHeld = false;
    bool isAudioClass   = false;
};

struct AoasSnapshot {
    // ── The link to the service ─────────────────────────────────────────────
    // Distinct from deviceReady on purpose: an unbound service and a bound one
    // with no DAC are both "nothing is playing", and only one of them is a bug.
    bool bound = false;
    std::string bindError;

    // ── The device ──────────────────────────────────────────────────────────
    // Attached and READY are different facts and were being collapsed. A DAC
    // can be plugged in, permitted and fully parsed -- 30 alt-settings on
    // screen -- while no stream is open, and the driver answers deviceInfo()
    // with "Not configured" in exactly that state. Reading that as "no device"
    // put "(none attached)" above a full format ladder.
    bool deviceAttached = false;
    bool deviceReady = false;    // a stream is actually open
    std::string deviceInfo;      // empty until the driver has a name for it
    int  uacVersion = 0;        // 1 or 2; 0 when unknown
    bool hardwareVolume = false;
    bool hardwareMute   = false;
    // Whole tuples, never three parallel lists. The HiBy FC4 has no capture
    // path at all and an empty vector says so honestly, where a zero-length
    // "rates" list next to a populated "channels" list would not.
    std::vector<AoasFormat> outputFormats;
    std::vector<AoasFormat> captureFormats;

    // Native DSD alt-settings. bitDepth is meaningless for these and is left
    // at zero; the subslot is the container width the bit stream rides in.
    // Empty is a real answer -- see the note in the DSD section of the draw.
    std::vector<AoasFormat> dsdFormats;
    // True when the DEVICE set bmFormats bit 31 on those alt-settings, false
    // when the driver only inferred DSD from duplicate alts. The difference is
    // the difference between reporting the device's claim and reporting a
    // guess, and the screen must not blur them.
    bool dsdFlaggedByDescriptor = false;

    // ── Ownership ───────────────────────────────────────────────────────────
    int ownerUid = -1;          // -1 when the device is free
    std::string ownerLabel;     // resolved package name, or a bare uid
    AoasFormat activeFormat;    // all zeros when no stream is configured
    int pendingPlaybackMs = 0;

    // ── Android's view of the bus ───────────────────────────────────────────
    std::vector<UsbBusEntry> bus;
};

// The diagnostics. These three are NOT on IAoas and must never be: they
// reconfigure the stream and generate samples, which is precisely what a
// client may not make AOAS do (CLAUDE.md, rule 2). They reach the service over
// a separate, signature-protected IAoasDebug that only this process binds, and
// the service refuses all three while a real client owns the device.
struct DebugCaptureResult {
    bool  ok = false;
    double peakDbfs = 0.0;
    std::string message;
};

class AoasSource {
public:
    virtual ~AoasSource() = default;

    // Cheap enough to call every frame; implementations cache and refresh on
    // their own schedule rather than making the UI decide how stale is too
    // stale.
    virtual const AoasSnapshot& snapshot() = 0;

    // The one non-diagnostic command: the manual disconnect CLAUDE.md carves
    // out as the single case where interrupting a live client is allowed,
    // "equivalent to physically unplugging the DAC today".
    virtual void forceDisconnect() = 0;

    // ── Diagnostics ─────────────────────────────────────────────────────────
    virtual bool debugOpenStream(int sampleRate, int channels, int bitDepth) = 0;
    virtual long debugTone(int sampleRate, int channels, int bitDepth, int seconds) = 0;
    virtual DebugCaptureResult debugCapture(int sampleRate, int channels, int bitDepth) = 0;

    // Whether the diagnostics are currently allowed — false while a client
    // owns the device. The console greys the buttons rather than letting the
    // call fail, because a button that silently does nothing is worse than one
    // that says why it cannot.
    virtual bool diagnosticsPermitted() = 0;

    // A running log the console prints. Every implementation appends to it;
    // the console never writes to it.
    virtual const std::vector<std::string>& log() const = 0;
};
