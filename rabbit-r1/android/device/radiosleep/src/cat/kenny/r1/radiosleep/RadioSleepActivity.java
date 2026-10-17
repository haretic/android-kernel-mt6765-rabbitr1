// SPDX-License-Identifier: Apache-2.0
package cat.kenny.r1.radiosleep;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.UserManager;
import android.view.MenuItem;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

public final class RadioSleepActivity extends Activity {
    private static final int[] MINUTES = {0, 1, 5, 10, 15, 30, 60, 120};
    private SharedPreferences prefs;
    private Button choice;
    private String[] labels;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        if (!getSystemService(UserManager.class).isSystemUser()) {
            finish();
            return;
        }
        if (getActionBar() != null) getActionBar().setDisplayHomeAsUpEnabled(true);
        prefs = RadioSleep.prefs(this);
        labels = new String[MINUTES.length];
        labels[0] = getString(R.string.never);
        for (int i = 1; i < MINUTES.length; i++) labels[i] = getString(R.string.minutes, MINUTES[i]);

        ScrollView scroll = new ScrollView(this);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        int padding = Math.round(16 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        TextView after = new TextView(this);
        after.setText(R.string.after);
        layout.addView(after);
        choice = new Button(this);
        choice.setOnClickListener(v -> new AlertDialog.Builder(this)
                .setTitle(R.string.after)
                .setSingleChoiceItems(labels, index(), (dialog, which) -> {
                    prefs.edit().putInt(RadioSleep.MINUTES, MINUTES[which]).apply();
                    update();
                    dialog.dismiss();
                })
                .show());
        layout.addView(choice);
        CheckBox charger = new CheckBox(this);
        charger.setText(R.string.on_charger);
        charger.setChecked(prefs.getBoolean(RadioSleep.ON_CHARGER, false));
        charger.setOnCheckedChangeListener((button, checked) ->
                prefs.edit().putBoolean(RadioSleep.ON_CHARGER, checked).apply());
        layout.addView(charger);
        TextView explanation = new TextView(this);
        explanation.setText(R.string.explanation);
        explanation.setPadding(0, padding, 0, 0);
        layout.addView(explanation);
        scroll.addView(layout);
        setContentView(scroll);
        update();
    }

    private int index() {
        int minutes = prefs.getInt(RadioSleep.MINUTES, RadioSleep.DEFAULT_MINUTES);
        for (int i = 0; i < MINUTES.length; i++) if (MINUTES[i] == minutes) return i;
        return 0;
    }

    private void update() {
        choice.setText(labels[index()]);
    }

    @Override public boolean onOptionsItemSelected(MenuItem item) {
        if (item.getItemId() == android.R.id.home) {
            finish();
            return true;
        }
        return super.onOptionsItemSelected(item);
    }
}
