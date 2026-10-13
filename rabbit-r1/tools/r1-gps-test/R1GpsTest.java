// SPDX-License-Identifier: Apache-2.0
// Requests GPS fixes from the shell and prints satellite status and locations:
//   adb push r1-gps-test.jar /data/local/tmp/
//   adb shell CLASSPATH=/data/local/tmp/r1-gps-test.jar app_process / R1GpsTest [seconds]
// Runs as the shell user, which holds ACCESS_FINE_LOCATION.
import android.content.Context;
import android.location.GnssStatus;
import android.location.Location;
import android.location.LocationListener;
import android.location.LocationManager;
import android.os.Handler;
import android.os.Looper;
import java.lang.reflect.Constructor;
import java.lang.reflect.Field;

public class R1GpsTest {
    private static Context shellContext() throws Exception {
        Class<?> at = Class.forName("android.app.ActivityThread");
        Constructor<?> ctor = at.getDeclaredConstructor();
        ctor.setAccessible(true);
        Object thread = ctor.newInstance();
        Field current = at.getDeclaredField("sCurrentActivityThread");
        current.setAccessible(true);
        current.set(null, thread);
        Context system = (Context) at.getDeclaredMethod("getSystemContext").invoke(thread);
        return system.createPackageContext("com.android.shell", 0);
    }

    public static void main(String[] args) throws Exception {
        int seconds = args.length > 0 ? Integer.parseInt(args[0]) : 120;
        Looper.prepareMainLooper();
        LocationManager lm = (LocationManager) shellContext().getSystemService(Context.LOCATION_SERVICE);
        System.out.println("gps enabled: " + lm.isProviderEnabled(LocationManager.GPS_PROVIDER));
        Handler handler = new Handler(Looper.getMainLooper());
        lm.registerGnssStatusCallback(new GnssStatus.Callback() {
            @Override public void onStarted() { System.out.println("engine started"); }
            @Override public void onStopped() { System.out.println("engine stopped"); }
            @Override public void onFirstFix(int ttffMillis) { System.out.println("first fix after " + ttffMillis + " ms"); }
            @Override public void onSatelliteStatusChanged(GnssStatus s) {
                int used = 0;
                StringBuilder cn0 = new StringBuilder();
                for (int i = 0; i < s.getSatelliteCount(); i++) {
                    if (s.usedInFix(i))
                        used++;
                    cn0.append(String.format(" %d/%d:%.0f", s.getConstellationType(i), s.getSvid(i), s.getCn0DbHz(i)));
                }
                System.out.println("satellites " + s.getSatelliteCount() + " used " + used + cn0);
            }
        }, handler);
        lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000, 0, new LocationListener() {
            @Override public void onLocationChanged(Location l) {
                System.out.println(String.format("fix accuracy %.0f m, %d satellites", l.getAccuracy(),
                        l.getExtras() != null ? l.getExtras().getInt("satellites") : -1));
            }
        }, Looper.getMainLooper());
        handler.postDelayed(() -> System.exit(0), seconds * 1000L);
        Looper.loop();
    }
}
