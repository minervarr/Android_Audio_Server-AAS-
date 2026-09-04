package io.nava.aoas;

import android.os.Binder;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;
import android.util.Log;

import java.io.IOException;
import java.util.NoSuchElementException;

/**
 * The AIDL interface, implemented as a shell over the C++ server.
 *
 * Each method takes the caller's uid and hands everything to native code on its
 * first line. What is genuinely Java's job here -- and the reason this class has
 * any state at all -- is the client's callback object: an IBinder lives on this
 * side, so this is where the reference and its death recipient live.
 *
 * Ownership is never taken from a live client. acquire() refuses with ERR_BUSY
 * while someone else holds the device; a forced takeover mid-stream would be the
 * audible interruption this project exists to remove.
 */
final class AoasBinder extends IAoas.Stub implements AoasNative.OwnershipLostListener {

    private static final String TAG = "AOAS";

    private final long handle;

    /** The current owner's callback, and the recipient watching it die. */
    private IAoasClient client;
    private IBinder.DeathRecipient deathRecipient;

    // --- bring-up diagnostics ---------------------------------------------
    //
    // Deliberately NOT on IAoas. The AIDL file is the contract every client app
    // compiles against, and none of this belongs in it: it reconfigures the
    // stream and generates samples, which is the opposite of what a client is
    // allowed to make AOAS do. AoasDebugActivity runs in this same process and
    // reaches these by casting the local binder, so they never cross a process
    // boundary and no other app can call them.

    int[] debugCaps() {
        return AoasNative.nativeCaps(handle);
    }

    long debugTone(int sampleRate, int channels, int bitDepth, int millis,
                   double hz, double amplitude) {
        return AoasNative.nativeDebugTone(handle, sampleRate, channels, bitDepth,
                                          millis, hz, amplitude);
    }

    double[] debugCapture(int sampleRate, int channels, int bitDepth, int millis) {
        return AoasNative.nativeDebugCapture(handle, sampleRate, channels,
                                             bitDepth, millis);
    }

    AoasBinder(long handle) {
        this.handle = handle;
        AoasNative.setOwnershipLostListener(this);
    }

    @Override
    public int acquire(IAoasClient client, int sampleRate, int channels, int bitDepth,
                       int ringMillis, ParcelFileDescriptor[] ringOut) {
        if (ringOut == null || ringOut.length < 1) {
            throw new IllegalArgumentException(
                    "ringOut must be a one-element array to receive the ring");
        }
        if (client == null) {
            throw new NullPointerException("client callback is required: a client "
                    + "that cannot be told it lost the DAC would write into a ring "
                    + "nobody drains");
        }
        final int uid = Binder.getCallingUid();

        final int result = AoasNative.nativeAcquire(handle, uid, sampleRate, channels,
                                                    bitDepth, ringMillis);
        if (result < 0) {
            return -result;   // an ERR_* code; ringOut is left untouched
        }

        // Watch the client die BEFORE handing the descriptor over. If it dies in
        // the gap, the death recipient still fires and the device comes free on
        // its own -- no user intervention, no waiting for the next app.
        synchronized (this) {
            this.client = client;
            this.deathRecipient = () -> {
                Log.i(TAG, "owner process died; freeing the device");
                AoasNative.nativeOwnerDied(handle);
                clearClient();
            };
            try {
                client.asBinder().linkToDeath(this.deathRecipient, 0);
            } catch (RemoteException e) {
                // Already dead. Undo the grant rather than own a client we
                // cannot notice dying, and close the descriptor we were about
                // to hand it -- nobody else will.
                AoasNative.nativeRelease(handle, uid);
                clearClient();
                try {
                    ParcelFileDescriptor.adoptFd(result).close();
                } catch (IOException closeFailed) {
                    Log.w(TAG, "could not close the ring descriptor", closeFailed);
                }
                return IAoas.ERR_NO_DEVICE;
            }
        }

        // adoptFd: the descriptor is closed once the transaction is done. The
        // server keeps its own, and the mapping is what holds the memory.
        ringOut[0] = ParcelFileDescriptor.adoptFd(result);
        return IAoas.OK;
    }

    @Override
    public void release() {
        final int uid = Binder.getCallingUid();
        if (!AoasNative.nativeRelease(handle, uid)) {
            throw new SecurityException("uid " + uid + " does not own the device");
        }
        clearClient();
    }

    /**
     * Unlike release(), the client stays the owner and keeps its ring, so there
     * is deliberately no clearClient() here: the death recipient must go on
     * watching a client that is still holding the DAC.
     */
    @Override
    public void flush() {
        final int uid = Binder.getCallingUid();
        if (!AoasNative.nativeFlush(handle, uid)) {
            throw new SecurityException("uid " + uid + " does not own the device");
        }
    }

    @Override
    public int[] activeFormat() {
        return AoasNative.nativeActiveFormat(handle);
    }

    @Override
    public int pendingPlaybackMs() {
        return AoasNative.nativePendingPlaybackMs(handle);
    }

    @Override
    public boolean isDeviceReady() {
        return AoasNative.nativeDeviceReady(handle);
    }

    @Override
    public String deviceInfo() {
        return AoasNative.nativeDeviceInfo(handle);
    }

    @Override
    public int currentOwnerUid() {
        return AoasNative.nativeOwnerUid(handle);
    }

    /**
     * From C++, when ownership ended and the owner did not ask for it: the user
     * disconnected it by hand, the DAC was unplugged, or we are shutting down.
     */
    @Override
    public void onOwnershipLost(int reason) {
        final IAoasClient doomed;
        synchronized (this) {
            doomed = client;
        }
        if (doomed != null) {
            try {
                doomed.onOwnershipLost(reason);   // oneway; does not block us
            } catch (RemoteException e) {
                Log.i(TAG, "client was already gone when told it lost the device");
            }
        }
        clearClient();
    }

    private void clearClient() {
        synchronized (this) {
            if (client != null && deathRecipient != null) {
                try {
                    client.asBinder().unlinkToDeath(deathRecipient, 0);
                } catch (NoSuchElementException ignored) {
                    // The binder already died, so the recipient is gone with it.
                    // Nothing left to detach from.
                }
            }
            client = null;
            deathRecipient = null;
        }
    }
}
