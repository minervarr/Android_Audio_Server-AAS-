#include "usb_device.hh"

#include <android/log.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "usb_audio.h"   // audio_engine: backends/usb

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "AOAS", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AOAS", __VA_ARGS__)

namespace aoas {

UsbDevice::UsbDevice() = default;
UsbDevice::~UsbDevice() = default;

bool UsbDevice::attach(int fd) {
    std::lock_guard<std::mutex> lock(mu_);
    if (driver_) {
        LOGE("attach: a device is already attached; ignoring fd %d", fd);
        return false;
    }

    auto driver = std::make_unique<UsbAudioDriver>();

    // One driver instance per process: open() sets libusb's
    // NO_DEVICE_DISCOVERY option globally and initialises a context, so a
    // second instance would be fighting the first over the same state.
    if (!driver->open(fd)) {
        LOGE("attach: driver->open(%d) failed", fd);
        return false;
    }
    if (!driver->parseDescriptors()) {
        LOGE("attach: parseDescriptors failed -- not a USB Audio Class device?");
        driver->close();
        return false;
    }

    // STABLE over LOW_LATENCY: AOAS is a background service that must hold the
    // endpoint steady for hours across app switches, screen-off and whatever
    // else the phone is doing. A deeper isochronous queue is what buys that.
    // Latency is not the currency here; not dropping out is.
    driver->setLatencyProfile(UsbAudioDriver::PROFILE_STABLE);

    // Unity gain, forever. Anything else would be AOAS processing audio.
    driver->setSoftwareGain(1.0f);

    driver_ = std::move(driver);
    LOGI("attached: %s", driver_->getDeviceInfo().c_str());
    return true;
}

void UsbDevice::detach() {
    std::lock_guard<std::mutex> lock(mu_);
    if (!driver_) return;
    LOGI("detaching device");
    if (streaming_) driver_->stop();
    driver_->close();
    driver_.reset();
    streaming_ = false;
    active_ = Format{};
}

bool UsbDevice::ready() const {
    std::lock_guard<std::mutex> lock(mu_);
    return driver_ != nullptr && streaming_;
}

std::string UsbDevice::info() const {
    std::lock_guard<std::mutex> lock(mu_);
    return driver_ ? driver_->getDeviceInfo() : std::string{};
}

std::vector<int> UsbDevice::supportedRates() const {
    std::lock_guard<std::mutex> lock(mu_);
    return driver_ ? driver_->getSupportedRates() : std::vector<int>{};
}

bool UsbDevice::ensureFormat(int sampleRate, int channels, int bitDepth) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!driver_) return false;

    // The case this whole project exists to protect: the incoming owner wants
    // what is already playing, so we touch nothing. No stop(), no configure(),
    // no re-lock, no pop -- the DAC never learns that the app changed.
    if (streaming_ && active_.sameRequestAs(sampleRate, channels, bitDepth)) {
        LOGI("format %d Hz / %d ch / %d bit already running -- silent handover",
             sampleRate, channels, bitDepth);
        return true;
    }

    if (streaming_) {
        LOGI("format change %d -> %d Hz: the DAC will re-lock its clock",
             active_.sampleRate, sampleRate);
        driver_->stop();
        streaming_ = false;
        active_ = Format{};
    }

    if (!driver_->configure(sampleRate, channels, bitDepth)) {
        LOGE("configure(%d, %d, %d) failed -- the DAC does not offer this "
             "format, and AOAS will not resample to pretend it does",
             sampleRate, channels, bitDepth);
        return false;
    }
    if (!driver_->start()) {
        LOGE("start() failed after a successful configure()");
        return false;
    }

    streaming_ = true;
    active_ = Format{driver_->getConfiguredRate(),
                     driver_->getConfiguredChannels(),
                     driver_->getConfiguredBitDepth(),
                     driver_->getConfiguredSubslotSize()};
    LOGI("streaming: %d Hz / %d ch / %d bit / %d-byte subslots",
         active_.sampleRate, active_.channels, active_.bitDepth,
         active_.subslotBytes);
    return true;
}

Format UsbDevice::activeFormat() const {
    std::lock_guard<std::mutex> lock(mu_);
    return active_;
}

int UsbDevice::write(const uint8_t* data, int length) {
    // No lock: this runs on the relay thread once per drain, and the driver's
    // write() is a lock-free ring push. Taking mu_ here would put the relay
    // behind whatever a Binder thread happens to be asking. The lifetime is
    // safe because driver_ is only reset in detach(), which stops the relay
    // first.
    if (!driver_) return 0;
    // write() is the raw path: bytes already in wire layout, straight into the
    // driver's ring. Deliberately not writeFloat32/writeInt16, which would run
    // the samples through a gain multiply and a requantise.
    return driver_->write(data, length);
}

void UsbDevice::flush() {
    // No lock, for exactly the reason write() states: this runs on the relay
    // thread, between two of its own writes, and driver_ is only reset in
    // detach(), which stops the relay first.
    if (!driver_) return;
    // Clears the driver's playback ring and resets its drain accounting. It
    // does NOT stop or reconfigure the stream -- the endpoint keeps running on
    // padded silence, so the DAC's clock survives, which is the one thing this
    // whole project exists to protect.
    driver_->flush();
}

int UsbDevice::pendingPlaybackMs() const {
    std::lock_guard<std::mutex> lock(mu_);
    return driver_ ? driver_->getPendingPlaybackMs() : 0;
}

// --- bring-up diagnostics ---------------------------------------------------

Caps UsbDevice::caps() const {
    std::lock_guard<std::mutex> lock(mu_);
    Caps c;
    if (!driver_) return c;
    c.attached       = true;
    c.info           = driver_->getDeviceInfo();
    c.uacVersion     = driver_->getUacVersion();
    c.outputFormats  = driver_->getOutputFormatTuples();
    c.captureFormats = driver_->getCaptureFormatTuples();
    c.dsdFormats     = driver_->getDsdFormatTuples();
    c.hasHwVolume    = driver_->hasHardwareVolume();
    c.hasHwMute      = driver_->hasHardwareMute();
    c.volMinDbQ8     = driver_->getVolumeMinDbQ8();
    c.volMaxDbQ8     = driver_->getVolumeMaxDbQ8();
    c.volResDbQ8     = driver_->getVolumeResDbQ8();
    return c;
}

namespace {

// One PCM sample, laid out the way the wire wants it: little-endian, and
// left-aligned inside the subslot when the subslot is wider than the bit depth
// (UAC2 puts the padding at the LSB end; usb_audio.cpp says the same).
void storeSample(uint8_t* dst, int32_t value, int subslotBytes, int padBits) {
    uint32_t wire = static_cast<uint32_t>(value) << padBits;
    for (int b = 0; b < subslotBytes; ++b) {
        dst[b] = static_cast<uint8_t>((wire >> (8 * b)) & 0xFFu);
    }
}

// Read one sample back out of the wire layout, sign-extended to int32.
int32_t loadSample(const uint8_t* src, int subslotBytes, int padBits, int bitDepth) {
    uint32_t wire = 0;
    for (int b = 0; b < subslotBytes; ++b) {
        wire |= static_cast<uint32_t>(src[b]) << (8 * b);
    }
    int32_t value = static_cast<int32_t>(wire >> padBits);
    const int32_t sign = int32_t{1} << (bitDepth - 1);
    if (value & sign) value -= sign << 1;
    return value;
}

}  // namespace

long UsbDevice::playTestTone(int sampleRate, int channels, int bitDepth,
                             int millis, double hz, double amplitude) {
    // ensureFormat takes mu_ itself, so it is called before anything locks.
    if (!ensureFormat(sampleRate, channels, bitDepth)) return -1;

    const Format fmt = activeFormat();
    if (!fmt.valid()) return -1;

    const int padBits = fmt.subslotBytes * 8 - fmt.bitDepth;
    const int32_t peak = (int32_t{1} << (fmt.bitDepth - 1)) - 1;
    const double gain  = amplitude * static_cast<double>(peak);

    // A tenth of a second per chunk: small enough that the loop notices a
    // detach quickly, large enough not to spin.
    const long totalFrames = static_cast<long>(fmt.sampleRate) * millis / 1000;
    const int chunkFrames = fmt.sampleRate / 10;
    std::vector<uint8_t> chunk(static_cast<size_t>(chunkFrames) *
                               static_cast<size_t>(fmt.frameBytes()));

    long framesDone = 0;
    long bytesAccepted = 0;
    double phase = 0.0;
    const double step = 2.0 * M_PI * hz / static_cast<double>(fmt.sampleRate);

    while (framesDone < totalFrames) {
        const int n = static_cast<int>(std::min<long>(chunkFrames, totalFrames - framesDone));
        uint8_t* out = chunk.data();
        for (int f = 0; f < n; ++f) {
            const int32_t v = static_cast<int32_t>(std::lround(std::sin(phase) * gain));
            phase += step;
            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
            for (int ch = 0; ch < fmt.channels; ++ch) {
                storeSample(out, v, fmt.subslotBytes, padBits);
                out += fmt.subslotBytes;
            }
        }

        int offset = 0;
        const int wanted = n * fmt.frameBytes();
        while (offset < wanted) {
            if (!driver_) return bytesAccepted;          // detached mid-tone
            const int wrote = write(chunk.data() + offset, wanted - offset);
            if (wrote <= 0) {
                // The driver ring being full is the normal state while a tone
                // plays: it means the DAC is consuming at its own rate.
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            offset += wrote;
            bytesAccepted += wrote;
        }
        framesDone += n;
    }
    return bytesAccepted;
}

double UsbDevice::captureProbe(int sampleRate, int channels, int bitDepth,
                               int millis, long* framesOut) {
    if (framesOut) *framesOut = 0;

    std::lock_guard<std::mutex> lock(mu_);
    if (!driver_) return -1.0;
    if (!driver_->configureCapture(sampleRate, channels, bitDepth)) return -1.0;
    if (!driver_->startCapture()) return -1.0;

    const int subslot = driver_->getConfiguredCaptureSubslotSize();
    const int bits    = driver_->getConfiguredCaptureBitDepth();
    const int chans   = driver_->getConfiguredCaptureChannels();
    const int padBits = subslot * 8 - bits;
    const double full = static_cast<double>(int32_t{1} << (bits - 1));

    std::vector<uint8_t> buf(16384);
    long frames = 0;
    int32_t maxAbs = 0;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(millis);
    while (std::chrono::steady_clock::now() < deadline) {
        const int got = driver_->readCapture(buf.data(), static_cast<int>(buf.size()));
        if (got <= 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        const int samples = got / subslot;
        for (int i = 0; i < samples; ++i) {
            int32_t v = loadSample(buf.data() + i * subslot, subslot, padBits, bits);
            if (v < 0) v = -v;
            if (v > maxAbs) maxAbs = v;
        }
        frames += samples / (chans > 0 ? chans : 1);
    }
    driver_->stopCapture();

    if (framesOut) *framesOut = frames;
    return static_cast<double>(maxAbs) / full;
}

}  // namespace aoas
