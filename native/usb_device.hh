#ifndef AOAS_USB_DEVICE_HH
#define AOAS_USB_DEVICE_HH

// UsbDevice — AOAS's single, permanent handle on the DAC.
//
// This is a thin policy layer over audio_engine's UsbAudioDriver, and it is
// thin on purpose: the driver already does everything hard (UAC1/UAC2
// descriptors, alt-setting selection, sample rate over control transfers, the
// isochronous feedback endpoint, its own -19 priority transfer thread). What
// this class adds is the ONE rule that makes AOAS what it is:
//
//   the isochronous stream is opened once and is never closed to hand the
//   device from one client app to the next.
//
// So there is deliberately no close-per-client, no stop-on-release, and no
// open() that a client path can reach. attach() happens when the USB permission
// arrives and detach() when the cable is pulled -- nothing in between.
//
// Idle behaviour needs no code here: when nobody feeds the ring, the driver's
// submitTransfer() pads the packet with silence and keeps the endpoint running
// (usb_audio.cpp), which is exactly what docs/design.md asks for. Stalling the
// endpoint instead would be its own audible glitch.
//
// AOAS never alters a sample: no gain, no resample, no dither, no mix. The
// driver's software-gain path is left at unity and its integer writes stay
// bit-perfect (UsbAudioDriver::isUnityGainBitPerfect).

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class UsbAudioDriver;

namespace aoas {

struct Format {
    int sampleRate   = 0;
    int channels     = 0;
    int bitDepth     = 0;
    int subslotBytes = 0;   // bytes per sample on the wire; NOT always bitDepth/8

    bool valid() const { return sampleRate > 0 && channels > 0 && subslotBytes > 0; }
    int frameBytes() const { return channels * subslotBytes; }
    // Compares only what the caller can ask for. subslotBytes is the DAC's
    // answer, not part of the request, so it is not part of the question.
    bool sameRequestAs(int rate, int ch, int bits) const {
        return sampleRate == rate && channels == ch && bitDepth == bits;
    }
};

// What the DAC's descriptors advertise. Filled from the driver after attach();
// every field is the device's own claim, not something AOAS decided.
struct Caps {
    bool attached = false;
    std::string info;
    int uacVersion = 0;          // 0x0100 (UAC1) or 0x0200 (UAC2)

    // Flattened {rate, channels, bitDepth} triples. Tuples rather than three
    // independent lists because the axes are coupled: a device offering 384 kHz
    // and 24 bit does not necessarily offer them together.
    std::vector<int> outputFormats;
    std::vector<int> captureFormats;

    // Native DSD (RAW_DATA) alt-settings, as (rate, channels, subslotBytes)
    // triples. Kept apart from outputFormats because a DSD alt-setting is not
    // a PCM mode: writing PCM into one is full-scale noise. Empty means the
    // device advertises no NATIVE DSD path -- it says nothing about DoP, which
    // rides inside an ordinary PCM stream and needs no alt-setting of its own.
    std::vector<int> dsdFormats;

    bool hasHwVolume = false;
    bool hasHwMute   = false;
    int  volMinDbQ8 = 0, volMaxDbQ8 = 0, volResDbQ8 = 0;
};

class UsbDevice {
public:
    UsbDevice();
    ~UsbDevice();

    UsbDevice(const UsbDevice&) = delete;
    UsbDevice& operator=(const UsbDevice&) = delete;

    // Take up the DAC on `fd`, which came from Java's
    // UsbDeviceConnection.getFileDescriptor(). We do NOT own it: the Java side
    // must hold that connection open for as long as we stream, or libusb is
    // left holding a descriptor the OS has already reclaimed.
    bool attach(int fd);

    // The cable came out (or the permission was revoked). Everything else in
    // AOAS treats this as the one legitimate reason for the stream to end.
    void detach();

    bool ready() const;
    std::string info() const;

    // Rates the DAC advertises for playback. Empty until attach().
    std::vector<int> supportedRates() const;

    // Bring the stream up in this format, or leave it exactly as it is when it
    // already matches -- and matching is the case worth protecting: it is a
    // handover with no reconfiguration, so the DAC's clock never re-locks and
    // the switch is inaudible.
    //
    // A genuine format change cannot avoid stop()/configure()/start() inside
    // the driver, so the DAC does re-lock and it is usually audible. We take
    // that over the alternative, which would be resampling: one transient
    // between owners is a smaller price than altering every sample.
    //
    // MUST NOT be called while a relay is feeding the device -- callers arrange
    // that by only reconfiguring when there is no owner.
    bool ensureFormat(int sampleRate, int channels, int bitDepth);

    Format activeFormat() const;

    // Hand bytes to the driver's ring. Non-blocking; returns bytes accepted,
    // which is short when the driver's ring is full. Frame-aligned by the
    // driver. These bytes reach the USB endpoint unmodified.
    int write(const uint8_t* data, int length);

    // Throw away the audio already handed to the driver but not yet sent. The
    // ring here is `playbackRingMs` (3000) deep, so without this a client that
    // stops or skips is still heard for up to three seconds -- releasing the
    // shared ring never touched this buffer at all.
    //
    // Deliberately NOT a stop or a reconfigure: the isochronous stream stays
    // open and the driver pads it with silence, so the DAC's clock does not
    // re-lock. That is what separates a flush from a handover.
    //
    // Called only from the relay thread (see Relay::flush).
    void flush();

    // Real (non-silence) audio still buffered between here and the DAC. Lets an
    // owner drain its tail before releasing instead of cutting it off.
    int pendingPlaybackMs() const;

    // --- bring-up diagnostics ----------------------------------------------
    //
    // Everything below this line exists for AoasDebugActivity, the hardware
    // bring-up console, and for nothing else. No client path reaches any of it.
    // It is in this class rather than off to one side because it is the only
    // code that needs the driver handle these answers come from.

    // What the DAC says it can do, straight out of its descriptors: available
    // before any stream is configured, because attach() has already parsed
    // them. This is the answer to "is it output-only, and at what quality".
    Caps caps() const;

    // Play a generated sine straight to the endpoint, blocking until it has
    // been handed over. Diagnostics only: this is AOAS producing samples rather
    // than relaying a client's, which is exactly what rule 2 forbids in the
    // audio path -- so it lives outside that path and is reachable only from
    // the debug UI. It proves the cable, the permission, the alt-setting and
    // the isochronous queue in one press.
    //
    // Returns the number of bytes accepted by the driver, or -1 if the format
    // could not be configured. MUST NOT be called while a client owns the
    // device; the caller checks that.
    long playTestTone(int sampleRate, int channels, int bitDepth,
                      int millis, double hz, double amplitude);

    // Open the capture direction for `millis`, read what arrives and report the
    // peak sample as a fraction of full scale (0.0 to 1.0), or -1 if capture
    // could not be configured. Answers "does input actually deliver frames",
    // which no descriptor dump can.
    double captureProbe(int sampleRate, int channels, int bitDepth, int millis,
                        long* framesOut);

private:
    mutable std::mutex mu_;
    std::unique_ptr<UsbAudioDriver> driver_;
    Format active_{};
    bool streaming_ = false;
};

}  // namespace aoas

#endif  // AOAS_USB_DEVICE_HH
