#include "fake_source.hh"

#include <cstdio>

namespace {

// The FC4's real ladder, abbreviated to the rates that change the layout.
// 30 alt-settings render as 30 lines; listing every one here would only test
// the scrollbar, which is vk_canvas's problem and not this console's.
const int kRates[] = {44100, 48000, 88200, 96000, 176400, 192000,
                      352800, 384000, 705600, 768000};

}  // namespace

FakeSource::FakeSource(Scene scene) { setScene(scene); }

void FakeSource::note(const std::string& s) {
    log_.push_back(s);
    if (log_.size() > 12) log_.erase(log_.begin());
}

void FakeSource::setScene(Scene s) {
    snap_ = AoasSnapshot{};
    log_.clear();

    if (s == Scene::Unbound) {
        snap_.bindError = "bindService returned false (is AOAS installed?)";
        note("not bound");
        return;
    }

    snap_.bound = true;

    UsbBusEntry dac;
    dac.name = "HiBy FC4";
    dac.vendorId = 0x262a; dac.productId = 0x1048;
    dac.interfaces = 3; dac.endpoints = 2;
    dac.permissionHeld = true; dac.isAudioClass = true;

    if (s == Scene::NoDevice) {
        note("no DAC connected");
        return;
    }

    snap_.bus.push_back(dac);
    snap_.deviceReady   = true;
    snap_.deviceInfo    = "HiBy FC4 (262a:1048)";
    snap_.uacVersion    = 2;
    snap_.hardwareVolume = false;
    snap_.hardwareMute   = false;

    for (int r : kRates)
        for (int b : {16, 24, 32})
            snap_.outputFormats.push_back({r, 2, b, b == 24 ? 3 : b / 8});
    // Left empty on purpose: this device really has no capture path.

    snap_.activeFormat = {48000, 2, 24, 3};

    if (s == Scene::ClientOwns) {
        snap_.ownerUid = 10412;
        snap_.ownerLabel = "io.nava.matrixplayer (uid 10412)";
        snap_.pendingPlaybackMs = 42;
        note("client owns the device; diagnostics unavailable");
    } else {
        note("device idle, stream open at 48000/2/24");
    }
}

void FakeSource::forceDisconnect() {
    snap_.ownerUid = -1;
    snap_.ownerLabel.clear();
    snap_.pendingPlaybackMs = 0;
    note("forced disconnect");
}

bool FakeSource::debugOpenStream(int rate, int ch, int bits) {
    if (!diagnosticsPermitted()) return false;
    snap_.activeFormat = {rate, ch, bits, bits == 24 ? 3 : bits / 8};
    char b[96];
    std::snprintf(b, sizeof b, "opened %d/%d/%d", rate, ch, bits);
    note(b);
    return true;
}

long FakeSource::debugTone(int rate, int ch, int bits, int seconds) {
    if (!diagnosticsPermitted()) return -1;
    const int subslot = bits == 24 ? 3 : bits / 8;
    const long bytes = static_cast<long>(rate) * ch * subslot * seconds;
    char b[96];
    std::snprintf(b, sizeof b, "tone accepted %ld bytes", bytes);
    note(b);
    return bytes;
}

DebugCaptureResult FakeSource::debugCapture(int, int, int) {
    DebugCaptureResult r;
    if (snap_.captureFormats.empty()) {
        r.message = "device has no capture path";
        note(r.message);
        return r;
    }
    r.ok = true;
    r.peakDbfs = -18.3;
    note("capture peak -18.3 dBFS");
    return r;
}
