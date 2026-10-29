// File/layer: app/android/src/.../SessionService.java | Java, Android only
// Testing: CI's android-build compiles it and checks the manifest names it;
//          BENCH and Vehicle on the phone (ADR-0029 §5): the notification
//          while an adapter session is up, none after, the session running
//          on with the screen off

package org.qtproject.example.qttune;

import android.Manifest;
import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;
import android.os.PowerManager;
import android.util.Log;

// The foreground service held while an adapter session is up (ADR-0029
// §2), so Android keeps the process running with the screen off. It does
// no work itself: core's threads and the GUI thread do the session's. Its
// notification says what is connected; a tap opens the app.
// androidsession.cpp starts, updates and stops it through JNI.
//
// It also holds a partial wake lock while it runs: the service keeps
// Android from freezing the app, not the CPU from sleeping, and with the
// screen off the CPU slept about 100 s of a session, no request going out
// (docs/bench-sessions/2026-10-28-android-9.md). The screen still goes off
public class SessionService extends Service
{
    private static final String TAG = "QtTune";
    private static final String CHANNEL = "session";
    private static final String EXTRA_TEXT = "text";
    private static final int NOTIFICATION_ID = 1;

    private PowerManager.WakeLock wakeLock;

    // Starts the service, or updates its notification's text. The app is
    // in the foreground at a connect, where Android allows the start
    public static void start(Context context, String text)
    {
        try {
            Intent intent = new Intent(context, SessionService.class);
            intent.putExtra(EXTRA_TEXT, text);
            context.startForegroundService(intent);
        } catch (RuntimeException e) {
            // Refused (the app in the background): the session runs on
            // without it, as before
            Log.w(TAG, "the session's foreground service didn't start: " + e);
        }
    }

    public static void stop(Context context)
    {
        context.stopService(new Intent(context, SessionService.class));
    }

    // Android 13+ asks before an app shows notifications: once, at the
    // first adapter connect. Refused, the service still runs and Android
    // lists it in its task manager only
    public static void askToNotifyOnce(Context context)
    {
        if (Build.VERSION.SDK_INT < 33 || !(context instanceof Activity))
            return;
        final Activity activity = (Activity) context;
        if (activity.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)
                == PackageManager.PERMISSION_GRANTED)
            return;
        SharedPreferences prefs = activity.getSharedPreferences("session", MODE_PRIVATE);
        if (prefs.getBoolean("askedToNotify", false))
            return;
        prefs.edit().putBoolean("askedToNotify", true).apply();
        activity.runOnUiThread(() -> activity.requestPermissions(
                new String[] { Manifest.permission.POST_NOTIFICATIONS }, 0x5E55));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId)
    {
        String text = intent != null ? intent.getStringExtra(EXTRA_TEXT) : null;
        try {
            startForeground(NOTIFICATION_ID, notification(text != null ? text : ""),
                    ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE);
            holdWakeLock();
        } catch (RuntimeException e) {
            Log.w(TAG, "the session's foreground service was refused: " + e);
            stopSelf();
        }
        return START_NOT_STICKY;
    }

    // Until the service stops: no timeout, since a session lasts as long
    // as the drive. Android releases it if the process dies
    private void holdWakeLock()
    {
        if (wakeLock != null)
            return;
        PowerManager power = getSystemService(PowerManager.class);
        wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "QtTune:session");
        wakeLock.setReferenceCounted(false);
        wakeLock.acquire();
    }

    @Override
    public void onDestroy()
    {
        if (wakeLock != null && wakeLock.isHeld())
            wakeLock.release();
        wakeLock = null;
        super.onDestroy();
    }

    @Override
    public IBinder onBind(Intent intent)
    {
        return null;
    }

    private Notification notification(String text)
    {
        NotificationManager manager = getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(
                CHANNEL, "Sessions", NotificationManager.IMPORTANCE_LOW));
        Intent open = getPackageManager().getLaunchIntentForPackage(getPackageName());
        PendingIntent tap = PendingIntent.getActivity(this, 0, open,
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
        return new Notification.Builder(this, CHANNEL)
                .setSmallIcon(getApplicationInfo().icon)
                .setContentTitle("QtTune")
                .setContentText(text)
                .setContentIntent(tap)
                .setOngoing(true)
                .setCategory(Notification.CATEGORY_SERVICE)
                .build();
    }
}
