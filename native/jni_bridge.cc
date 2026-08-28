// jni_bridge — the only place C++ and Java meet, kept this small on purpose.
//
// Java exists here for exactly two reasons, both of them "the platform gives us
// no choice":
//
//   1. UsbManager.requestPermission, the Service class with its foreground
//      notification, and the BroadcastReceiver that hears the permission
//      answer have no NDK equivalent.
//   2. The AIDL interface is generated with the Java backend, because the NDK
//      ships only libbinder_ndk's C headers -- see the long note at the top of
//      aoas_service.hh for why that is the right trade rather than a defeat.
//
// Everything with an opinion about audio -- the driver, the ring, ownership,
// the relay -- is C++ behind this file. The Java stub's methods do nothing but
// call straight through here, so no Java code runs between a client's request
// and the state machine that answers it, and none whatsoever runs near a
// sample: audio never crosses Binder, it goes through shared memory.

#include <jni.h>

#include <android/log.h>

#include <string>
#include <vector>

#include "aoas_service.hh"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "AOAS", __VA_ARGS__)

namespace {

JavaVM* g_vm = nullptr;

// Cached for the up-call. The class reference is global because the up-call can
// come from the relay's thread or a Binder thread, neither of which inherits
// the local frame that looked the class up.
jclass    g_nativeClass = nullptr;
jmethodID g_onLost      = nullptr;

aoas::AoasServer* serverOf(jlong handle) {
    return reinterpret_cast<aoas::AoasServer*>(handle);
}

// UTF-8 -> UTF-16, then NewString. Deliberately not NewStringUTF: that takes
// Java's *modified* UTF-8, which encodes U+0000 and everything outside the BMP
// differently from real UTF-8. A USB string descriptor is whatever the device
// manufacturer put there, so it is not ours to assume it is ASCII.
jstring toJavaString(JNIEnv* env, const std::string& utf8) {
    std::vector<jchar> utf16;
    utf16.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp;
        size_t extra;
        if (c < 0x80)        { cp = c;        extra = 0; }
        else if (c < 0xE0)   { cp = c & 0x1Fu; extra = 1; }
        else if (c < 0xF0)   { cp = c & 0x0Fu; extra = 2; }
        else                 { cp = c & 0x07u; extra = 3; }
        if (i + extra >= utf8.size()) break;   // truncated: stop, do not guess
        for (size_t k = 1; k <= extra; ++k) {
            cp = (cp << 6) | (static_cast<unsigned char>(utf8[i + k]) & 0x3Fu);
        }
        i += extra + 1;
        if (cp <= 0xFFFF) {
            utf16.push_back(static_cast<jchar>(cp));
        } else {                                // surrogate pair
            cp -= 0x10000;
            utf16.push_back(static_cast<jchar>(0xD800 + (cp >> 10)));
            utf16.push_back(static_cast<jchar>(0xDC00 + (cp & 0x3FF)));
        }
    }
    return env->NewString(utf16.data(), static_cast<jsize>(utf16.size()));
}

// Up-call: ownership ended for a reason the owner did not ask for. Java holds
// the IAoasClient reference, so Java does the actual notifying.
void notifyOwnershipLost(int reason) {
    if (!g_vm || !g_nativeClass || !g_onLost) return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
        attached = true;
    }
    env->CallStaticVoidMethod(g_nativeClass, g_onLost, static_cast<jint>(reason));
    if (env->ExceptionCheck()) {
        // A client's callback threw. That is the client's problem, not a reason
        // to take the audio server down with it.
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (attached) g_vm->DetachCurrentThread();
}

}  // namespace

extern "C" {

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass local = env->FindClass("io/nava/aoas/AoasNative");
    if (!local) return JNI_ERR;
    g_nativeClass = static_cast<jclass>(env->NewGlobalRef(local));
    g_onLost = env->GetStaticMethodID(g_nativeClass, "onOwnershipLost", "(I)V");
    if (!g_onLost) return JNI_ERR;
    return JNI_VERSION_1_6;
}

JNIEXPORT jlong JNICALL
Java_io_nava_aoas_AoasNative_nativeCreate(JNIEnv*, jclass) {
    auto* server = new aoas::AoasServer();
    server->setOwnershipLostHandler(&notifyOwnershipLost);
    // The console runs in this same process (a NativeActivity; see
    // native/ui/android_main.cc) and reads the server directly rather than
    // through Binder. It has to find it somehow, and the alternative -- handing
    // the handle up to Java and back down through another JNI call -- would be
    // the same pointer with two more chances to be stale.
    aoas::setProcessServer(server);
    return reinterpret_cast<jlong>(server);
}

JNIEXPORT void JNICALL
Java_io_nava_aoas_AoasNative_nativeDestroy(JNIEnv*, jclass, jlong h) {
    aoas::setProcessServer(nullptr);
    delete serverOf(h);
}

// Returns a file descriptor for the shared ring, or a negative IAoas.ERR_*
// value. One int carries both answers so there is no state where a caller was
// granted ownership but has nowhere to write.
JNIEXPORT jint JNICALL
Java_io_nava_aoas_AoasNative_nativeAcquire(JNIEnv*, jclass, jlong h, jint uid,
                                           jint rate, jint channels, jint bits,
                                           jint ringMillis) {
    auto* s = serverOf(h);
    if (!s) return -aoas::kErrNoDevice;
    return s->acquire(static_cast<uid_t>(uid), rate, channels, bits, ringMillis);
}

JNIEXPORT jboolean JNICALL
Java_io_nava_aoas_AoasNative_nativeRelease(JNIEnv*, jclass, jlong h, jint uid) {
    auto* s = serverOf(h);
    return (s && s->release(static_cast<uid_t>(uid))) ? JNI_TRUE : JNI_FALSE;
}

// The owner's process died -- Java's death recipient fired.
JNIEXPORT void JNICALL
Java_io_nava_aoas_AoasNative_nativeOwnerDied(JNIEnv*, jclass, jlong h) {
    if (auto* s = serverOf(h)) s->onOwnerDied();
}

// The disconnect action on our notification: the one sanctioned interruption of
// a client that is still streaming.
JNIEXPORT void JNICALL
Java_io_nava_aoas_AoasNative_nativeForceDisconnect(JNIEnv*, jclass, jlong h) {
    if (auto* s = serverOf(h)) s->forceDisconnect();
}

JNIEXPORT jint JNICALL
Java_io_nava_aoas_AoasNative_nativeOwnerUid(JNIEnv*, jclass, jlong h) {
    auto* s = serverOf(h);
    return s ? s->ownerUid() : -1;
}

// {sampleRate, channels, bitDepth, subslotBytes}, or empty when no stream is
// configured. The order matches IAoas's FMT_* constants.
JNIEXPORT jintArray JNICALL
Java_io_nava_aoas_AoasNative_nativeActiveFormat(JNIEnv* env, jclass, jlong h) {
    auto* s = serverOf(h);
    const aoas::Format fmt = s ? s->activeFormat() : aoas::Format{};
    if (!fmt.valid()) return env->NewIntArray(0);
    jint values[4] = {fmt.sampleRate, fmt.channels, fmt.bitDepth, fmt.subslotBytes};
    jintArray out = env->NewIntArray(4);
    if (out) env->SetIntArrayRegion(out, 0, 4, values);
    return out;
}

JNIEXPORT jint JNICALL
Java_io_nava_aoas_AoasNative_nativePendingPlaybackMs(JNIEnv*, jclass, jlong h) {
    auto* s = serverOf(h);
    return s ? s->pendingPlaybackMs() : 0;
}

JNIEXPORT jboolean JNICALL
Java_io_nava_aoas_AoasNative_nativeDeviceReady(JNIEnv*, jclass, jlong h) {
    auto* s = serverOf(h);
    return (s && s->deviceReady()) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jstring JNICALL
Java_io_nava_aoas_AoasNative_nativeDeviceInfo(JNIEnv* env, jclass, jlong h) {
    auto* s = serverOf(h);
    return toJavaString(env, s ? s->deviceInfo() : std::string{});
}

// Java has the USB permission and an open UsbDeviceConnection. `fd` stays
// Java's: it must keep that connection open for as long as we stream, because
// the driver wraps the descriptor without taking ownership of it.
JNIEXPORT jboolean JNICALL
Java_io_nava_aoas_AoasNative_nativeUsbAttached(JNIEnv*, jclass, jlong h, jint fd) {
    auto* s = serverOf(h);
    return (s && s->onUsbAttached(static_cast<int>(fd))) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_io_nava_aoas_AoasNative_nativeUsbDetached(JNIEnv*, jclass, jlong h) {
    if (auto* s = serverOf(h)) s->onUsbDetached();
}


// --- bring-up diagnostics ---------------------------------------------------
//
// One int array rather than a Caps class mirrored in Java: this is a debug
// console, and a header plus flattened triples costs one JNI call and no
// parallel type definition that could drift.
//
//   [0] attached (0/1)   [1] UAC version   [2] hw volume (0/1)
//   [3] hw mute (0/1)    [4..6] volume min/max/step in dB Q8
//   [7] output format count   [8] capture format count
//   then that many {rate, channels, bitDepth} triples, output first.
JNIEXPORT jintArray JNICALL
Java_io_nava_aoas_AoasNative_nativeCaps(JNIEnv* env, jclass, jlong h) {
    auto* s = serverOf(h);
    const aoas::Caps c = s ? s->caps() : aoas::Caps{};

    const size_t nOut = c.outputFormats.size() / 3;
    const size_t nCap = c.captureFormats.size() / 3;

    std::vector<jint> v;
    v.reserve(9 + c.outputFormats.size() + c.captureFormats.size());
    v.push_back(c.attached ? 1 : 0);
    v.push_back(c.uacVersion);
    v.push_back(c.hasHwVolume ? 1 : 0);
    v.push_back(c.hasHwMute ? 1 : 0);
    v.push_back(c.volMinDbQ8);
    v.push_back(c.volMaxDbQ8);
    v.push_back(c.volResDbQ8);
    v.push_back(static_cast<jint>(nOut));
    v.push_back(static_cast<jint>(nCap));
    v.insert(v.end(), c.outputFormats.begin(), c.outputFormats.end());
    v.insert(v.end(), c.captureFormats.begin(), c.captureFormats.end());

    jintArray out = env->NewIntArray(static_cast<jsize>(v.size()));
    if (out) env->SetIntArrayRegion(out, 0, static_cast<jsize>(v.size()), v.data());
    return out;
}

// Blocks for the length of the tone; the debug UI calls it off the main thread.
JNIEXPORT jlong JNICALL
Java_io_nava_aoas_AoasNative_nativeDebugTone(JNIEnv*, jclass, jlong h, jint rate,
                                             jint channels, jint bits, jint millis,
                                             jdouble hz, jdouble amplitude) {
    auto* s = serverOf(h);
    if (!s) return -1;
    return static_cast<jlong>(
        s->debugTone(rate, channels, bits, millis, hz, amplitude));
}

// {peak as a fraction of full scale, frames read}. Peak is -1 when capture
// could not be configured, which is also how "output only" shows up.
JNIEXPORT jdoubleArray JNICALL
Java_io_nava_aoas_AoasNative_nativeDebugCapture(JNIEnv* env, jclass, jlong h,
                                                jint rate, jint channels,
                                                jint bits, jint millis) {
    auto* s = serverOf(h);
    long frames = 0;
    const double peak = s ? s->debugCapture(rate, channels, bits, millis, &frames)
                          : -1.0;
    jdouble values[2] = {peak, static_cast<jdouble>(frames)};
    jdoubleArray out = env->NewDoubleArray(2);
    if (out) env->SetDoubleArrayRegion(out, 0, 2, values);
    return out;
}

}  // extern "C"
