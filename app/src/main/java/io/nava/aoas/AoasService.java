package io.nava.aoas;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.pm.ServiceInfo;
import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

/**
 * The foreground service that owns the USB permission and, through it, the one
 * live connection to the DAC.
 *
 * This class does the three things Android offers no native API for -- ask for
 * the USB permission, stay alive in the background with a notification, and
 * hear the permission answer on a BroadcastReceiver -- and then gets out of the
 * way. Everything else is C++ behind AoasNative.
 *
 * The connection deliberately outlives every client: it is opened when the DAC
 * appears and closed only when the cable comes out or the service stops. It is
 * never closed to hand the device from one app to the next, because that is
 * precisely what makes the DAC re-lock its clock and click.
 */
public final class AoasService extends Service {

    private static final String TAG = "AOAS";
    private static final String CHANNEL_ID = "aoas";
    private static final int NOTIFICATION_ID = 1;

    private static final String ACTION_USB_PERMISSION = "io.nava.aoas.USB_PERMISSION";
    /** The disconnect button on the notification: the one sanctioned interruption. */
    private static final String ACTION_DISCONNECT = "io.nava.aoas.DISCONNECT";
    /**
     * Ask again for the USB permission. Only the bring-up console sends this:
     * normally the request happens by itself on attach and on start, and this
     * is for the case where the DAC was already plugged in and the grant was
     * refused or never offered.
     */
    static final String ACTION_REQUEST_PERMISSION = "io.nava.aoas.REQUEST_PERMISSION";

    private long handle;
    private AoasBinder binder;

    /**
     * Held for as long as we stream, and that is not incidental: the native
     * driver wraps this connection's file descriptor without taking ownership,
     * so closing this would pull the descriptor out from under libusb.
     */
    private UsbDeviceConnection connection;
    private UsbDevice device;

    /** True between startForeground() and stopForeground(); see openDevice(). */
    private boolean foreground;

    private final BroadcastReceiver receiver = new BroadcastReceiver() {
        @Override
        public void onReceive(Context context, Intent intent) {
            final String action = intent.getAction();
            if (action == null) return;
            switch (action) {
                case ACTION_USB_PERMISSION: {
                    UsbDevice granted = usbDeviceFrom(intent);
                    if (intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)) {
                        openDevice(granted);
                    } else {
                        Log.i(TAG, "USB permission refused for " + granted);
                    }
                    break;
                }
                case UsbManager.ACTION_USB_DEVICE_ATTACHED: {
                    requestPermissionFor(usbDeviceFrom(intent));
                    break;
                }
                case UsbManager.ACTION_USB_DEVICE_DETACHED: {
                    UsbDevice gone = usbDeviceFrom(intent);
                    if (device != null && device.equals(gone)) {
                        closeDevice();
                    }
                    break;
                }
                default:
                    break;
            }
        }
    };

    /**
     * Pulls the UsbDevice out of a broadcast. The typed overload landed in API
     * 33; below that the untyped one is the only option, so both live here
     * rather than being repeated at each of the three call sites.
     */
    @SuppressWarnings("deprecation")
    private static UsbDevice usbDeviceFrom(Intent intent) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE, UsbDevice.class);
        }
        return intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
    }

    @Override
    public void onCreate() {
        super.onCreate();
        handle = AoasNative.nativeCreate();
        binder = new AoasBinder(handle);

        IntentFilter filter = new IntentFilter();
        filter.addAction(ACTION_USB_PERMISSION);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED);
        filter.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(receiver, filter);
        }

        createNotificationChannel();
        // Deliberately NOT going foreground here. Android 16 refuses to start a
        // connectedDevice foreground service unless the app already holds a
        // permission from the "connected device" family, and for AOAS that means
        // a granted USB device permission -- which we do not have until the user
        // (or the ATTACHED intent-filter) gives us one. So the promotion happens
        // in openDevice(), the first moment it is legal, and the service simply
        // runs in the background until then. That ordering is not a workaround:
        // with no DAC held there is nothing for a foreground service to protect.
        attachAlreadyConnectedDac();
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        final String action = intent != null ? intent.getAction() : null;
        if (ACTION_DISCONNECT.equals(action)) {
            // The user asked, in our own UI, to take the device off whoever has
            // it -- treated as deliberate, like unplugging the DAC by hand.
            AoasNative.nativeForceDisconnect(handle);
            updateNotification();
        } else if (ACTION_REQUEST_PERMISSION.equals(action)) {
            attachAlreadyConnectedDac();
        }
        // STICKY: if Android kills us under memory pressure, come back. A dead
        // AOAS mid-playback is the failure this whole design exists to prevent.
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return binder;
    }

    @Override
    public void onDestroy() {
        unregisterReceiver(receiver);
        closeDevice();
        AoasNative.nativeDestroy(handle);
        handle = 0;
        super.onDestroy();
    }

    // --- USB --------------------------------------------------------------

    /**
     * The DAC may already be plugged in when we start -- Android only broadcasts
     * ATTACHED for devices that arrive while somebody is listening.
     */
    private void attachAlreadyConnectedDac() {
        UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (usb == null) return;
        for (UsbDevice candidate : usb.getDeviceList().values()) {
            if (isAudioDevice(candidate)) {
                requestPermissionFor(candidate);
                return;   // exactly one DAC, by design (CLAUDE.md open question 4)
            }
        }
    }

    private void requestPermissionFor(UsbDevice candidate) {
        if (candidate == null || !isAudioDevice(candidate)) return;
        UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (usb == null) return;

        if (usb.hasPermission(candidate)) {
            openDevice(candidate);
            return;
        }
        // The dialog the user sees at most once. With the ATTACHED intent-filter
        // declared in the manifest, Android usually grants it without asking at
        // all -- and once AOAS holds it, no other app of ours ever has to.
        Intent intent = new Intent(ACTION_USB_PERMISSION).setPackage(getPackageName());
        int flags = PendingIntent.FLAG_UPDATE_CURRENT;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            flags |= PendingIntent.FLAG_MUTABLE;   // the system fills in the result
        }
        usb.requestPermission(candidate, PendingIntent.getBroadcast(this, 0, intent, flags));
    }

    /** True if any interface is USB Audio Class -- what the driver can drive. */
    private static boolean isAudioDevice(UsbDevice candidate) {
        for (int i = 0; i < candidate.getInterfaceCount(); i++) {
            UsbInterface iface = candidate.getInterface(i);
            if (iface.getInterfaceClass() == UsbConstants.USB_CLASS_AUDIO) return true;
        }
        return false;
    }

    private void openDevice(UsbDevice granted) {
        if (granted == null || connection != null) return;
        UsbManager usb = (UsbManager) getSystemService(Context.USB_SERVICE);
        if (usb == null) return;

        UsbDeviceConnection opened = usb.openDevice(granted);
        if (opened == null) {
            Log.e(TAG, "openDevice failed for " + granted.getDeviceName());
            return;
        }
        // The fd is borrowed, not given: `opened` must stay referenced and open
        // for the entire life of the stream.
        if (!AoasNative.nativeUsbAttached(handle, opened.getFileDescriptor())) {
            Log.e(TAG, "the native driver refused the device");
            opened.close();
            return;
        }
        connection = opened;
        device = granted;
        Log.i(TAG, "holding " + AoasNative.nativeDeviceInfo(handle));
        // Now, and not before: holding the USB permission is what makes a
        // connectedDevice foreground service legal in the first place.
        startInForeground();
        updateNotification();
    }

    private void closeDevice() {
        if (connection == null) return;
        AoasNative.nativeUsbDetached(handle);   // stops the stream first
        connection.close();                     // then the fd it was using
        connection = null;
        device = null;
        stopForegroundWithDevice();
    }

    // --- notification -----------------------------------------------------

    private void createNotificationChannel() {
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null && nm.getNotificationChannel(CHANNEL_ID) == null) {
            NotificationChannel channel = new NotificationChannel(
                    CHANNEL_ID, getString(R.string.channel_name),
                    NotificationManager.IMPORTANCE_LOW);   // silent: it is furniture
            channel.setDescription(getString(R.string.channel_description));
            nm.createNotificationChannel(channel);
        }
    }

    /**
     * Promote to a foreground service. Only legal once the USB permission is
     * held, so it is called from openDevice() and nowhere else.
     */
    private void startInForeground() {
        if (foreground) return;
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            startForeground(NOTIFICATION_ID, buildNotification(),
                            ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
        } else {
            startForeground(NOTIFICATION_ID, buildNotification());
        }
        foreground = true;
    }

    /**
     * Drop back out of the foreground with the device. Keeping the notification
     * up with no DAC behind it would be claiming to hold something we do not.
     */
    private void stopForegroundWithDevice() {
        if (!foreground) return;
        stopForeground(STOP_FOREGROUND_REMOVE);
        foreground = false;
    }

    private void updateNotification() {
        if (!foreground) return;
        NotificationManager nm = getSystemService(NotificationManager.class);
        if (nm != null) nm.notify(NOTIFICATION_ID, buildNotification());
    }

    /**
     * The whole user interface, for now: what the DAC is, who is using it, and
     * a way to take it back.
     */
    private Notification buildNotification() {
        final String info = AoasNative.nativeDeviceInfo(handle);
        final String title = info.isEmpty() ? getString(R.string.no_device) : info;

        final int ownerUid = AoasNative.nativeOwnerUid(handle);
        String text;
        if (ownerUid < 0) {
            text = getString(R.string.idle);
        } else {
            // The uid comes from C++; turning it into a name needs
            // PackageManager, which is why the translation happens here.
            String[] packages = getPackageManager().getPackagesForUid(ownerUid);
            String owner = (packages != null && packages.length > 0)
                    ? packages[0] : ("uid " + ownerUid);
            text = getString(R.string.in_use_by, owner);
        }

        Notification.Builder builder = new Notification.Builder(this, CHANNEL_ID)
                .setContentTitle(title)
                .setContentText(text)
                .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
                .setOngoing(true);

        if (ownerUid >= 0) {
            Intent disconnect = new Intent(this, AoasService.class).setAction(ACTION_DISCONNECT);
            int flags = PendingIntent.FLAG_UPDATE_CURRENT | PendingIntent.FLAG_IMMUTABLE;
            builder.addAction(new Notification.Action.Builder(
                    null, getString(R.string.disconnect),
                    PendingIntent.getService(this, 0, disconnect, flags)).build());
        }
        return builder.build();
    }
}
