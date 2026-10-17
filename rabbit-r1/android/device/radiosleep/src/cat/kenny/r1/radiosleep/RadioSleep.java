// SPDX-License-Identifier: Apache-2.0
package cat.kenny.r1.radiosleep;

import android.app.AlarmManager;
import android.app.Application;
import android.app.PendingIntent;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.net.ConnectivityManager;
import android.os.BatteryManager;
import android.os.PowerManager;
import android.os.SystemClock;
import android.os.UserManager;
import android.provider.Settings;
import android.util.Log;

/**
 * Airplane mode once the screen has been off for the chosen time, lifted when it comes
 * back on. Only airplane mode this app set is lifted.
 */
public final class RadioSleep extends Application {
    private static final String TAG = "R1RadioSleep";
    static final String MINUTES = "minutes";
    static final String ON_CHARGER = "on_charger";
    private static final String OURS = "airplane_ours";
    static final int DEFAULT_MINUTES = 10;

    static SharedPreferences prefs(Context context) {
        return context.createDeviceProtectedStorageContext()
                .getSharedPreferences("radio_sleep", Context.MODE_PRIVATE);
    }

    @Override public void onCreate() {
        super.onCreate();
        if (!getSystemService(UserManager.class).isSystemUser()) return;
        IntentFilter filter = new IntentFilter(Intent.ACTION_SCREEN_OFF);
        filter.addAction(Intent.ACTION_SCREEN_ON);
        registerReceiver(new BroadcastReceiver() {
            @Override public void onReceive(Context context, Intent intent) {
                if (Intent.ACTION_SCREEN_OFF.equals(intent.getAction())) schedule(context);
                else wake(context);
            }
        }, filter, Context.RECEIVER_NOT_EXPORTED);
        // A restarted process may have missed the screen coming back on.
        if (getSystemService(PowerManager.class).isInteractive()) wake(this);
        else schedule(this);
    }

    private static PendingIntent alarm(Context context) {
        return PendingIntent.getBroadcast(context, 0, new Intent(context, SleepReceiver.class),
                PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);
    }

    private static void schedule(Context context) {
        int minutes = prefs(context).getInt(MINUTES, DEFAULT_MINUTES);
        if (minutes <= 0) return;
        context.getSystemService(AlarmManager.class).setExactAndAllowWhileIdle(
                AlarmManager.ELAPSED_REALTIME_WAKEUP,
                SystemClock.elapsedRealtime() + minutes * 60_000L, alarm(context));
    }

    private static boolean airplane(Context context) {
        return Settings.Global.getInt(context.getContentResolver(),
                Settings.Global.AIRPLANE_MODE_ON, 0) != 0;
    }

    private static boolean plugged(Context context) {
        Intent battery = context.getApplicationContext().registerReceiver(null,
                new IntentFilter(Intent.ACTION_BATTERY_CHANGED));
        return battery != null && battery.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) != 0;
    }

    static void sleep(Context context) {
        SharedPreferences prefs = prefs(context);
        if (context.getSystemService(PowerManager.class).isInteractive() || airplane(context)
                || prefs.getInt(MINUTES, DEFAULT_MINUTES) <= 0
                || (!prefs.getBoolean(ON_CHARGER, false) && plugged(context))) return;
        // Recorded first, so that a crash in between cannot leave the radios off for good.
        prefs.edit().putBoolean(OURS, true).commit();
        Log.i(TAG, "Screen off long enough: radios off");
        context.getSystemService(ConnectivityManager.class).setAirplaneMode(true);
    }

    private static void wake(Context context) {
        context.getSystemService(AlarmManager.class).cancel(alarm(context));
        SharedPreferences prefs = prefs(context);
        if (!prefs.getBoolean(OURS, false)) return;
        prefs.edit().putBoolean(OURS, false).commit();
        if (airplane(context)) {
            Log.i(TAG, "Screen on: radios back on");
            context.getSystemService(ConnectivityManager.class).setAirplaneMode(false);
        }
    }
}
