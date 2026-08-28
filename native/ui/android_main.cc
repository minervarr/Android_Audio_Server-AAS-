// The Android entry point, and the only file in the console that includes JNI.
//
// AOAS's UI is a NativeActivity: there is no Java Activity, no layout XML and
// no Java view code anywhere in this app. What Java remains (AoasService,
// AoasBinder, AoasNative) exists only where the platform offers no native API
// — a Service class, an AIDL Stub, and the System.loadLibrary boundary — and
// each forwards to C++ on its first line.
#include <android_native_app_glue.h>
#include <android/log.h>
#include <jni.h>

#include <memory>

#include "console_app.hh"
#include "os/android_host.hh"   // app_shell
#include "service_source.hh"

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "aoas.ui", __VA_ARGS__)

namespace {

// Ask Android to start AoasService.
//
// Necessary because the console can be opened cold: nothing has been plugged
// in, so the manifest's USB_DEVICE_ATTACHED filter has not fired and the
// service has never run. Without this the screen would report "not running"
// forever and the DAC would only ever be picked up by a physical re-plug.
//
// startService, never startForegroundService — Android 16 would then demand a
// foreground promotion within five seconds, and AOAS cannot legally promote
// itself until it holds a USB device permission (see CLAUDE.md, the constraint
// that was discovered on hardware rather than designed).
void start_service(android_app* state) {
    JavaVM* vm = state->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK &&
        vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
        LOGE("AttachCurrentThread failed; AoasService not started");
        return;
    }

    jobject activity = state->activity->clazz;

    // FindClass on the glue thread would search the SYSTEM class loader, which
    // does not know this APK's classes. The activity's own loader does, and
    // reaching it through the activity object is the standard way to ask.
    jclass  actCls   = env->GetObjectClass(activity);
    jmethodID getLdr = env->GetMethodID(actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    jobject loader   = env->CallObjectMethod(activity, getLdr);
    jclass  ldrCls   = env->GetObjectClass(loader);
    jmethodID loadCl = env->GetMethodID(ldrCls, "loadClass",
                                        "(Ljava/lang/String;)Ljava/lang/Class;");
    jstring  svcName = env->NewStringUTF("io.nava.aoas.AoasService");
    jclass   svcCls  = static_cast<jclass>(env->CallObjectMethod(loader, loadCl, svcName));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGE("could not load io.nava.aoas.AoasService");
        return;
    }

    jclass intentCls = env->FindClass("android/content/Intent");
    jmethodID ctor   = env->GetMethodID(intentCls, "<init>",
                                        "(Landroid/content/Context;Ljava/lang/Class;)V");
    jobject intent   = env->NewObject(intentCls, ctor, activity, svcCls);

    jmethodID startSvc = env->GetMethodID(actCls, "startService",
                                          "(Landroid/content/Intent;)Landroid/content/ComponentName;");
    env->CallObjectMethod(activity, startSvc, intent);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        LOGE("startService threw");
    }

    env->DeleteLocalRef(svcName);
}

}  // namespace

extern "C" void android_main(android_app* state) {
    start_service(state);

    // No launch extra and no all-files prompt: AOAS is not launched with an
    // argument and never reads shared storage.
    auto host = std::make_unique<AndroidHost>(state,
                                              /*launchExtraKey=*/nullptr,
                                              /*fallback=*/nullptr,
                                              /*requestAllFilesAccess=*/false);

    ConsoleApp app(std::make_unique<ServiceSource>());
    if (app.create(std::move(host))) app.run();
}
