package org.reuc_recomp.umbrella_chronicles;

import android.os.Bundle;
import android.system.ErrnoException;
import android.system.Os;
import android.util.Log;
import org.libsdl.app.SDLActivity;

// The game: SDL's activity runs SDL_main of libmain.so (game/android_app_main.cpp) on a thread of its own. First UC_INSTALL_DIR names the game's
// folder (common/executable_path_posix.h puts "the program" there, so every path the game keeps beside itself lands in it) and TMPDIR the app's
// cache (the test server's sockets), and a custom Vulkan driver is unpacked for the renderer. The handheld menu (--ui deck, the Steam Deck's) opens
// with the View button.
public class GameActivity extends SDLActivity {
    @Override
    protected void onCreate(Bundle state) {
        try {
            Os.setenv("UC_INSTALL_DIR", LauncherActivity.installFolder().getAbsolutePath(), true);
            Os.setenv("TMPDIR", getCacheDir().getAbsolutePath(), true);
            Os.setenv("UC_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);   // libadrenotools' hooks
            final String driver = getIntent().getStringExtra("driver");   // a custom Vulkan driver (LauncherActivity): unpacked, then loaded by the renderer
            if (driver != null && !driver.isEmpty()) {
                try {
                    final String[] installed = Drivers.install(this, driver);
                    Os.setenv("UC_VULKAN_DRIVER_DIR", installed[0], true);
                    Os.setenv("UC_VULKAN_DRIVER", installed[1], true);
                } catch (java.io.IOException error) {
                    Log.e("uc", "the driver " + driver + " cannot be used, the system's instead", error);
                }
            }
            // Diagnostics: `adb shell am start -n <app>/.LauncherActivity --es UC_PIPELINE_THREADS 1` sets UC_PIPELINE_THREADS for the game
            final Bundle extras = getIntent().getExtras();
            if (extras != null) for (String key : extras.keySet()) if (key.startsWith("UC_")) Os.setenv(key, String.valueOf(extras.get(key)), true);
        } catch (ErrnoException error) {
            Log.e("uc", "cannot set the game's environment", error);
        }
        super.onCreate(state);
    }

    @Override
    protected String[] getLibraries() { return new String[] {"SDL3", "main"}; }   // libc++ is inside libmain.so

    @Override
    protected String[] getArguments() {
        // Diagnostics: `--es UC_ARGS "--render-scale 1"` adds arguments of uc after the handheld menu's
        final java.util.ArrayList<String> arguments = new java.util.ArrayList<>(java.util.Arrays.asList("--ui", "deck"));
        final String extra = getIntent().getStringExtra("UC_ARGS");
        if (extra != null && !extra.isBlank()) arguments.addAll(java.util.Arrays.asList(extra.trim().split("\\s+")));
        return arguments.toArray(new String[0]);
    }
}
