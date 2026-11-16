package org.reuc_recomp.umbrella_chronicles;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.TextView;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

// The app's entry. The game reads and writes /sdcard/UmbrellaChronicles (the extracted disc in game/, the program data in data/, saves, settings,
// logs, drivers/), which needs "All files access". Once it has that and finds the game it offers Play and the graphics driver: the system's or a
// custom one (Turnip...) from drivers/, where "Add a driver" copies a package the player picks. Every button works with the gamepad (A presses).
// `adb shell am start -n <app>/.LauncherActivity --es UC_AUTOSTART 1 [--es UC_DRIVER <file.zip>]` starts the game right away (the test harness).
public class LauncherActivity extends Activity {
    static File installFolder() { return new File(Environment.getExternalStorageDirectory(), "UmbrellaChronicles"); }

    private static final int PICK_DRIVER = 1;
    private SharedPreferences preferences;
    private TextView message;
    private Button driver, addDriver, play;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        preferences = getSharedPreferences("launcher", MODE_PRIVATE);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        layout.setGravity(Gravity.CENTER);
        final int padding = (int) (32 * getResources().getDisplayMetrics().density);
        layout.setPadding(padding, padding, padding, padding);
        TextView title = new TextView(this);
        title.setText("Resident Evil: The Umbrella Chronicles");
        title.setTextSize(24);
        title.setGravity(Gravity.CENTER);
        message = new TextView(this);
        message.setTextSize(16);
        message.setGravity(Gravity.CENTER);
        play = new Button(this);
        driver = new Button(this);
        driver.setOnClickListener(v -> nextDriver());
        addDriver = new Button(this);
        addDriver.setText("Add a driver (.zip)");
        addDriver.setOnClickListener(v -> pickDriver());
        layout.addView(title);
        layout.addView(message);
        layout.addView(play);
        layout.addView(driver);
        layout.addView(addDriver);
        setContentView(layout);
    }

    @Override
    protected void onResume() {
        super.onResume();
        driver.setVisibility(View.GONE);
        addDriver.setVisibility(View.GONE);
        if (!Environment.isExternalStorageManager()) {
            message.setText("The game, its saves and settings live in " + installFolder() + ".\nAllow \"All files access\" for this app.");
            play.setText("Open the setting");
            play.setOnClickListener(v -> startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName()))));
            play.requestFocus();
            return;
        }
        if (!new File(installFolder(), "game/DATA/sys/main.dol").isFile() || !new File(installFolder(), "data/shaders").isDirectory()) {
            message.setText("The game was not found.\nCopy the extracted RBUE08 disc to " + new File(installFolder(), "game")
                + " (game/DATA/sys/main.dol), and the program data with scripts/android/build.sh.");
            play.setText("Check again");
            play.setOnClickListener(v -> onResume());
            play.requestFocus();
            return;
        }
        if (getIntent().hasExtra("UC_AUTOSTART")) { start(); return; }
        message.setText("");
        play.setText("Play");
        play.setOnClickListener(v -> start());
        driver.setVisibility(View.VISIBLE);
        addDriver.setVisibility(View.VISIBLE);
        showDriver();
        play.requestFocus();
    }

    // The chosen driver package ("" for the system's): the harness's UC_DRIVER, else the player's last choice while its file is there.
    private String chosenDriver() {
        final String asked = getIntent().getStringExtra("UC_DRIVER");
        final String chosen = asked != null ? asked : preferences.getString("driver", "");
        return chosen.isEmpty() || new File(Drivers.folder(), chosen).isFile() ? chosen : "";
    }

    private void showDriver() {
        final String chosen = chosenDriver();
        driver.setText("Graphics driver: " + (chosen.isEmpty() ? "the system's" : chosen));
    }

    private void nextDriver() {   // the system's, then each package in drivers/, round
        final List<String> options = new ArrayList<>();
        options.add("");
        options.addAll(Drivers.list());
        final int at = options.indexOf(chosenDriver());
        preferences.edit().putString("driver", options.get((at + 1) % options.size())).apply();
        showDriver();
    }

    private void pickDriver() {
        final Intent pick = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        pick.addCategory(Intent.CATEGORY_OPENABLE);
        pick.setType("*/*");
        pick.putExtra(Intent.EXTRA_MIME_TYPES, new String[] {"application/zip", "application/x-zip-compressed", "application/octet-stream"});
        startActivityForResult(pick, PICK_DRIVER);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != PICK_DRIVER || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri picked = data.getData();
        String name = "driver.zip";
        try (Cursor cursor = getContentResolver().query(picked, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (cursor != null && cursor.moveToFirst() && cursor.getString(0) != null) name = new File(cursor.getString(0)).getName();
        }
        if (!name.toLowerCase().endsWith(".zip")) name += ".zip";
        final File target = new File(Drivers.folder(), name);
        try {
            if (!Drivers.folder().isDirectory() && !Drivers.folder().mkdirs()) throw new IOException("cannot create " + Drivers.folder());
            try (InputStream in = getContentResolver().openInputStream(picked); OutputStream out = new FileOutputStream(target)) { Drivers.copy(in, out); }
            try (InputStream check = new FileInputStream(target)) {
                if (Drivers.libraryName(check) == null) { target.delete(); throw new IOException(name + " is not an AdrenoTools driver package (no meta.json)"); }
            }
            preferences.edit().putString("driver", name).apply();
            message.setText("Added " + name);
        } catch (IOException error) {
            message.setText("Cannot add the driver: " + error.getMessage());
        }
        showDriver();
    }

    private void start() {
        final Intent game = new Intent(this, GameActivity.class);
        if (getIntent().getExtras() != null) game.putExtras(getIntent().getExtras());   // UC_* diagnostics given to `adb shell am start` reach the game
        game.putExtra("driver", chosenDriver());
        startActivity(game);
        finish();
    }
}
