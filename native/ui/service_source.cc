#include "service_source.hh"

#include <algorithm>
#include <cstdio>

#include "aoas_service.hh"

namespace {

// The driver reports {rate, channels, bitDepth} triples; the subslot width is a
// property of the OPEN stream, not of the descriptor, so it is unknown until a
// format is actually configured. Reported as 0 rather than guessed at
// bitDepth/8 — that guess is wrong for the common 24-in-4 case, and a console
// that quietly prints a plausible wrong number is worse than one that prints
// nothing.
std::vector<AoasFormat> unflatten(const std::vector<int>& triples) {
    std::vector<AoasFormat> out;
    out.reserve(triples.size() / 3);
    for (size_t i = 0; i + 2 < triples.size(); i += 3)
        out.push_back({triples[i], triples[i + 1], triples[i + 2], 0});

    // The driver returns alt-settings in DESCRIPTOR order, which groups them by
    // bit depth and so scatters each rate across the list -- 44.1 kHz appears
    // three times, hundreds of pixels apart. Sorted here rather than in the
    // draw, because the console's selected-format index points into THIS
    // vector: a list sorted only at draw time would highlight one format and
    // open another.
    std::sort(out.begin(), out.end(), [](const AoasFormat& a, const AoasFormat& b) {
        if (a.sampleRate != b.sampleRate) return a.sampleRate < b.sampleRate;
        if (a.bitDepth   != b.bitDepth)   return a.bitDepth   < b.bitDepth;
        return a.channels < b.channels;
    });
    return out;
}

}  // namespace

void ServiceSource::note(const std::string& s) {
    log_.push_back(s);
    if (log_.size() > 12) log_.erase(log_.begin());
}

const AoasSnapshot& ServiceSource::snapshot() {
    aoas::AoasServer* srv = aoas::processServer();

    snap_ = AoasSnapshot{};
    if (!srv) {
        // The Activity can be opened before AoasService has ever been started —
        // nothing has plugged a DAC in, so nothing started it. Said plainly
        // rather than rendered as a device failure, because it is not one.
        snap_.bindError = "AoasService is not running (plug the DAC in)";
        return snap_;
    }

    snap_.bound       = true;
    snap_.deviceReady = srv->deviceReady();

    const auto caps = srv->caps();
    snap_.deviceAttached = caps.attached;

    // "Not configured" is the driver's answer for "no stream is open", not a
    // device name. Normalised to empty here so exactly one place decides what
    // an unnamed device looks like.
    const std::string info = srv->deviceInfo();
    snap_.deviceInfo = (info == "Not configured") ? std::string() : info;
    // The driver reports UAC as the descriptor's own bcd (0x0100 / 0x0200);
    // the console shows the human number.
    snap_.uacVersion     = caps.uacVersion >> 8;
    snap_.hardwareVolume = caps.hasHwVolume;
    snap_.hardwareMute   = caps.hasHwMute;
    snap_.outputFormats  = unflatten(caps.outputFormats);
    snap_.captureFormats = unflatten(caps.captureFormats);
    // The DSD entries are (rate, channels, SUBSLOT, flaggedByDescriptor)
    // quads, not (rate, channels, bits) triples, so they cannot go through
    // unflatten().
    for (size_t i = 0; i + 3 < caps.dsdFormats.size(); i += 4) {
        snap_.dsdFormats.push_back({caps.dsdFormats[i], caps.dsdFormats[i + 1],
                                    0, caps.dsdFormats[i + 2]});
        if (caps.dsdFormats[i + 3]) snap_.dsdFlaggedByDescriptor = true;
    }
    std::sort(snap_.dsdFormats.begin(), snap_.dsdFormats.end(),
              [](const AoasFormat& a, const AoasFormat& b) {
                  return a.sampleRate < b.sampleRate;
              });

    const auto f = srv->activeFormat();
    snap_.activeFormat = {f.sampleRate, f.channels, f.bitDepth, f.subslotBytes};
    snap_.pendingPlaybackMs = srv->pendingPlaybackMs();

    const int uid = srv->ownerUid();
    snap_.ownerUid = uid;
    if (uid >= 0) {
        // A uid, not a package name: turning one into the other needs
        // PackageManager, which is Java-only. The notification already does
        // that translation on the Java side, where it is free; doing it here
        // would mean a JNI hop for a string this screen shows to one developer.
        char b[64];
        std::snprintf(b, sizeof b, "uid %d", uid);
        snap_.ownerLabel = b;
    }

    // Android's own view of the USB bus is not here yet: enumerating it needs
    // UsbManager, and the JNI for that is a separate piece of work. What is
    // shown instead is what the DRIVER holds, which answers the narrower
    // question ("do we have the device") and not the wider one ("does Android
    // see a device we failed to claim"). The distinction matters and this is
    // the honest half of it.
    // "Not configured" is the driver's answer when no STREAM is open, not a
    // device name — it was being shown as one on the bus row too.
    if (caps.attached) {
        UsbBusEntry e;
        e.name = snap_.deviceInfo.empty() ? "(attached, not yet named)"
                                          : snap_.deviceInfo;
        e.permissionHeld = true;
        e.isAudioClass   = true;
        // vendor/product, interface and endpoint counts stay zero: they come
        // from Android's UsbDevice, which this side has not been wired to yet.
        // Left at zero and NOT drawn (see console_draw.cc) rather than printed
        // as 0000:0000, which read as a device with no identity instead of as
        // a field nobody has filled in.
        snap_.bus.push_back(e);
    }
    return snap_;
}

bool ServiceSource::diagnosticsPermitted() {
    aoas::AoasServer* srv = aoas::processServer();
    return srv && srv->deviceReady() && srv->ownerUid() < 0;
}

void ServiceSource::forceDisconnect() {
    if (auto* srv = aoas::processServer()) {
        srv->forceDisconnect();
        note("forced disconnect");
    }
}

bool ServiceSource::debugOpenStream(int rate, int ch, int bits) {
    if (!diagnosticsPermitted()) { note("refused: a client owns the device"); return false; }
    // Opening the stream IS playing a zero-length tone as far as the driver is
    // concerned: it configures the alt-setting and starts the transfers. Kept
    // as its own button because "does this format configure at all" and "does
    // it make sound" are separate questions on a bring-up console.
    const long n = aoas::processServer()->debugTone(rate, ch, bits, /*millis=*/0,
                                                    /*hz=*/1000.0, /*amplitude=*/0.0);
    char b[96];
    std::snprintf(b, sizeof b, "open %d/%d/%d -> %s", rate, ch, bits,
                  n >= 0 ? "ok" : "refused");
    note(b);
    return n >= 0;
}

long ServiceSource::debugTone(int rate, int ch, int bits, int seconds) {
    if (!diagnosticsPermitted()) { note("refused: a client owns the device"); return -1; }
    const long n = aoas::processServer()->debugTone(rate, ch, bits, seconds * 1000,
                                                    1000.0, 0.25);
    char b[96];
    std::snprintf(b, sizeof b, "tone %d/%d/%d -> %ld bytes", rate, ch, bits, n);
    note(b);
    return n;
}

DebugCaptureResult ServiceSource::debugCapture(int rate, int ch, int bits) {
    DebugCaptureResult r;
    if (!diagnosticsPermitted()) { r.message = "a client owns the device"; note(r.message); return r; }
    const double peak = aoas::processServer()->debugCapture(rate, ch, bits, 500, nullptr);
    if (peak < 0.0) {
        // How an output-only device answers, and it is an answer rather than a
        // failure — the FC4 genuinely has no capture path.
        r.message = "no capture path on this device";
    } else {
        r.ok = true;
        r.peakDbfs = peak;
        char b[64];
        std::snprintf(b, sizeof b, "capture peak %.4f of full scale", peak);
        r.message = b;
    }
    note(r.message);
    return r;
}
