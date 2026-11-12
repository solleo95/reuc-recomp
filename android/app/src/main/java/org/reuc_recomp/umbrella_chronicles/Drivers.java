package org.reuc_recomp.umbrella_chronicles;

import android.content.Context;
import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;
import org.json.JSONException;
import org.json.JSONObject;

// Custom Vulkan drivers (Turnip and others) in the AdrenoTools package format: a .zip with meta.json, whose "libraryName" is the driver library,
// next to that library. The player keeps them in <game folder>/drivers; Android loads no code from shared storage, so the chosen one is unpacked
// into the app's private files, where libadrenotools loads it in place of the system's driver (renderer/android_vulkan.cpp).
final class Drivers {
    private Drivers() {}

    static File folder() { return new File(LauncherActivity.installFolder(), "drivers"); }

    // The driver packages in the game folder, by file name.
    static List<String> list() {
        final String[] names = folder().list((directory, name) -> name.toLowerCase().endsWith(".zip"));
        final List<String> drivers = new ArrayList<>(names == null ? List.of() : Arrays.asList(names));
        drivers.sort(String.CASE_INSENSITIVE_ORDER);
        return drivers;
    }

    // The "libraryName" of a package's meta.json; null when it is not a driver package.
    static String libraryName(InputStream zip) throws IOException {
        try (ZipInputStream entries = new ZipInputStream(zip)) {
            for (ZipEntry entry; (entry = entries.getNextEntry()) != null;) {
                if (!entry.getName().equals("meta.json")) continue;
                final ByteArrayOutputStream text = new ByteArrayOutputStream();
                copy(entries, text);
                try {
                    final String library = new JSONObject(text.toString(StandardCharsets.UTF_8.name())).optString("libraryName", "");
                    return library.isEmpty() || library.contains("/") ? null : library;
                } catch (JSONException error) {
                    return null;
                }
            }
        }
        return null;
    }

    // Unpacks <game folder>/drivers/<name> into the app's files (again only when the package changed) and returns {folder ending in "/", library}.
    static String[] install(Context context, String name) throws IOException {
        final File zip = new File(folder(), name);
        final String library;
        try (InputStream in = new FileInputStream(zip)) { library = libraryName(in); }
        if (library == null) throw new IOException(name + " has no meta.json with a libraryName: not an AdrenoTools driver package");
        final File target = new File(new File(context.getFilesDir(), "drivers"), name.substring(0, name.length() - 4));
        final File stamp = new File(target, ".installed");
        final String version = zip.length() + ":" + zip.lastModified();
        if (!(stamp.isFile() && version.equals(read(stamp)) && new File(target, library).isFile())) {
            delete(target);
            if (!target.mkdirs()) throw new IOException("cannot create " + target);
            final String root = target.getCanonicalPath() + File.separator;
            try (ZipInputStream entries = new ZipInputStream(new FileInputStream(zip))) {
                for (ZipEntry entry; (entry = entries.getNextEntry()) != null;) {
                    final File out = new File(target, entry.getName());
                    if (!out.getCanonicalPath().startsWith(root)) throw new IOException("unsafe entry in " + name + ": " + entry.getName());
                    if (entry.isDirectory()) { out.mkdirs(); continue; }
                    out.getParentFile().mkdirs();
                    try (OutputStream file = new FileOutputStream(out)) { copy(entries, file); }
                }
            }
            try (OutputStream file = new FileOutputStream(stamp)) { file.write(version.getBytes(StandardCharsets.UTF_8)); }
        }
        return new String[] {target.getAbsolutePath() + "/", library};
    }

    static void copy(InputStream in, OutputStream out) throws IOException {
        final byte[] buffer = new byte[65536];
        for (int count; (count = in.read(buffer)) > 0;) out.write(buffer, 0, count);
    }

    private static String read(File file) throws IOException {
        try (InputStream in = new FileInputStream(file)) {
            final ByteArrayOutputStream text = new ByteArrayOutputStream();
            copy(in, text);
            return text.toString(StandardCharsets.UTF_8.name());
        }
    }

    private static void delete(File file) {
        final File[] children = file.listFiles();
        if (children != null) for (File child : children) delete(child);
        file.delete();
    }
}
