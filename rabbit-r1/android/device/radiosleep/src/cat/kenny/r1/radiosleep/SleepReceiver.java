// SPDX-License-Identifier: Apache-2.0
package cat.kenny.r1.radiosleep;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** The screen-off alarm. */
public final class SleepReceiver extends BroadcastReceiver {
    @Override public void onReceive(Context context, Intent intent) {
        RadioSleep.sleep(context);
    }
}
