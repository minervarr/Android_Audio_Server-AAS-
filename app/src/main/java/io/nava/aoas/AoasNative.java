package io.nava.aoas;

/**
 * The native boundary, and nothing else.
 *
 * Every method here is a straight pass-through to native/jni_bridge.cc. No
 * decision about the DAC, ownership, formats or buffering is made on this side
 * of the line -- that all lives in C++ (native/aoas_service.hh explains why the
 * line is drawn where it is). If logic ever starts accumulating in this file,
 * it is in the wrong file.
 */
final class AoasNative {

    static {
        // Loaded here rather than in the Service so that any class touching
        // native state gets the library, in whatever order they are first used.
        System.loadLibrary("aoas");
    }

    private AoasNative() {}

    /** Told when ownership ended for a reason the owner did not ask for. */
    interface OwnershipLostListener {
        void onOwnershipLost(int reason);
    }

    private static volatile OwnershipLostListener sListener;

    static void setOwnershipLostListener(OwnershipLostListener listener) {
        sListener = listener;
    }

    /**
     * Called from C++ (see notifyOwnershipLost in jni_bridge.cc), on whichever
     * thread took the device away. Java holds the IAoasClient reference, so
     * Java does the actual notifying.
     */
    @SuppressWarnings("unused")   // invoked by JNI, not by Java callers
    static void onOwnershipLost(int reason) {
        OwnershipLostListener listener = sListener;
        if (listener != null) {
            listener.onOwnershipLost(reason);
        }
    }

    static native long nativeCreate();
    static native void nativeDestroy(long handle);

    /**
     * @return a file descriptor for the shared ring, or a negative
     *         IAoas.ERR_* value. One int carries both answers, so "granted but
     *         with nowhere to write" is not a state that can exist.
     */
    static native int nativeAcquire(long handle, int uid, int sampleRate,
                                    int channels, int bitDepth, int ringMillis);

    static native boolean nativeRelease(long handle, int uid);
    static native void nativeOwnerDied(long handle);
    static native void nativeForceDisconnect(long handle);
    static native int nativeOwnerUid(long handle);

    /** {sampleRate, channels, bitDepth, subslotBytes}, or empty if no stream. */
    static native int[] nativeActiveFormat(long handle);

    static native int nativePendingPlaybackMs(long handle);
    static native boolean nativeDeviceReady(long handle);
    static native String nativeDeviceInfo(long handle);

    /**
     * Descriptor-level capabilities, flattened into one array so there is no
     * parallel Java type to keep in step with C++:
     *
     * <pre>
     *   [0] attached  [1] UAC version  [2] hw volume  [3] hw mute
     *   [4..6] volume min/max/step, dB in Q8
     *   [7] output format count  [8] capture format count
     *   then that many {rate, channels, bitDepth} triples, output first.
     * </pre>
     *
     * Available as soon as the device is attached: reading descriptors does not
     * need a stream. Used only by the bring-up console.
     */
    static native int[] nativeCaps(long handle);

    /**
     * Play a generated sine to the DAC. Diagnostics only, and never on the main
     * thread: it blocks for the length of the tone.
     *
     * @return bytes the driver accepted, or -1 if the format was refused or a
     *         client currently owns the device.
     */
    static native long nativeDebugTone(long handle, int sampleRate, int channels,
                                       int bitDepth, int millis, double hz,
                                       double amplitude);

    /**
     * Open the capture direction briefly and report what arrived.
     *
     * @return {peak as a fraction of full scale, frames read}; peak is -1 when
     *         capture could not be configured, which is how an output-only
     *         device answers.
     */
    static native double[] nativeDebugCapture(long handle, int sampleRate,
                                              int channels, int bitDepth,
                                              int millis);

    /** @param fd from UsbDeviceConnection.getFileDescriptor(); stays ours. */
    static native boolean nativeUsbAttached(long handle, int fd);
    static native void nativeUsbDetached(long handle);
}
