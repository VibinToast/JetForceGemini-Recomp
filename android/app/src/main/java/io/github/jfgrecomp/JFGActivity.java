package io.github.jfgrecomp;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.database.Cursor;
import android.net.Uri;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.system.Os;
import android.util.Log;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.util.ArrayList;

/**
 * Jet Force Gemini: Recompiled on Android.
 *
 * Before SDL starts the game (libmain.so's SDL_main) this copies the program files packed in the APK (assets/program:
 * the launcher UI and the controller database) to internal storage and tells the game where things are:
 *   APP_PROGRAM_PATH  the extracted program files (dino::runtime::get_program_path)
 *   APP_FOLDER_PATH   saves, settings, mods and texture packs, in the app's external files folder, which a PC sees
 *                     over USB as Android/data/io.github.jfgrecomp/files
 * The game's file dialogs (choosing the ROM, installing mods) open the Android document picker; the chosen files are
 * copied into the app folder and their paths passed back to the game.
 */
public class JFGActivity extends SDLActivity {
    private static final String TAG = "JFGRecomp";
    private static final int REQUEST_OPEN_FILE = 0x4a46;
    private static final int REQUEST_OPEN_FILES = 0x4a47;

    private static JFGActivity currentActivity;
    private File appFolder;

    private static native void nativeOnFileDialogResult(boolean success, String path);
    private static native void nativeOnFileDialogMultipleResult(boolean success, String[] paths);

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        currentActivity = this;

        File programDir = new File(getFilesDir(), "program");
        appFolder = getExternalFilesDir(null);
        if (appFolder == null) {
            appFolder = new File(getFilesDir(), "data");
        }
        appFolder.mkdirs();

        try {
            extractProgramFiles(programDir);
            Os.setenv("APP_PROGRAM_PATH", programDir.getAbsolutePath(), true);
            Os.setenv("APP_FOLDER_PATH", appFolder.getAbsolutePath(), true);
            Os.setenv("HOME", appFolder.getAbsolutePath(), true);
        } catch (Exception e) {
            Log.e(TAG, "Setting up the program files failed", e);
        }
        Log.i(TAG, "program=" + programDir + " data=" + appFolder);

        super.onCreate(savedInstanceState);
    }

    @Override
    protected void onDestroy() {
        if (currentActivity == this) {
            currentActivity = null;
        }
        super.onDestroy();
        // The game can't start a second time in the same process (its audio device, threads and globals are still
        // there), so a closed activity takes the process with it and the next launch starts clean.
        if (isFinishing()) {
            android.os.Process.killProcess(android.os.Process.myPid());
        }
    }

    // The APK's program files are copied once per installed version (versionCode + update time), so an update
    // replaces them and normal starts skip the copy.
    private void extractProgramFiles(File programDir) throws Exception {
        PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
        String stamp = info.versionCode + "-" + info.lastUpdateTime;
        File stampFile = new File(programDir, ".extracted");
        if (stampFile.exists() && stamp.equals(readSmallFile(stampFile))) {
            return;
        }
        deleteRecursive(programDir);
        copyAssetTree(getAssets(), "program", programDir);
        try (OutputStream out = new FileOutputStream(stampFile)) {
            out.write(stamp.getBytes("UTF-8"));
        }
    }

    private static void copyAssetTree(AssetManager assets, String path, File destination) throws IOException {
        String[] children = assets.list(path);
        if (children == null || children.length == 0) {
            destination.getParentFile().mkdirs();
            try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(destination)) {
                copyStream(in, out);
            }
            return;
        }
        destination.mkdirs();
        for (String child : children) {
            copyAssetTree(assets, path + "/" + child, new File(destination, child));
        }
    }

    // Called by the game (runtime/support.cpp) on its own thread.
    public static void openFileDialog() {
        launchPicker(REQUEST_OPEN_FILE, false);
    }

    public static void openFileDialogMultiple() {
        launchPicker(REQUEST_OPEN_FILES, true);
    }

    private static void launchPicker(int request, boolean multiple) {
        JFGActivity activity = currentActivity;
        if (activity == null) {
            reportResult(request, null);
            return;
        }
        activity.runOnUiThread(() -> {
            try {
                Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
                intent.addCategory(Intent.CATEGORY_OPENABLE);
                intent.setType("*/*");
                intent.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, multiple);
                activity.startActivityForResult(intent, request);
            } catch (Exception e) {
                Log.e(TAG, "Opening the file picker failed", e);
                reportResult(request, null);
            }
        });
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_OPEN_FILE && requestCode != REQUEST_OPEN_FILES) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }

        ArrayList<Uri> uris = new ArrayList<>();
        if (resultCode == Activity.RESULT_OK && data != null) {
            if (data.getClipData() != null) {
                for (int i = 0; i < data.getClipData().getItemCount(); i++) {
                    uris.add(data.getClipData().getItemAt(i).getUri());
                }
            } else if (data.getData() != null) {
                uris.add(data.getData());
            }
        }

        // Copying a ROM takes a moment; keep it off the UI thread.
        final int request = requestCode;
        new Thread(() -> {
            ArrayList<String> paths = new ArrayList<>();
            File importDir = new File(appFolder, "imported");
            importDir.mkdirs();
            for (Uri uri : uris) {
                try {
                    File target = new File(importDir, displayName(uri));
                    try (InputStream in = getContentResolver().openInputStream(uri);
                         OutputStream out = new FileOutputStream(target)) {
                        copyStream(in, out);
                    }
                    // ROM managers (RomM, for example) often hand out a zip; the game wants the ROM itself.
                    File rom = request == REQUEST_OPEN_FILE ? extractZippedRom(target, importDir) : null;
                    if (rom != null) {
                        target.delete();
                        target = rom;
                    }
                    paths.add(target.getAbsolutePath());
                } catch (Exception e) {
                    Log.e(TAG, "Copying " + uri + " failed", e);
                }
            }
            reportResult(request, paths);
        }, "JFG file import").start();
    }

    /**
     * If file is a zip with an N64 ROM in it (.z64, .n64 or .v64), extracts the first such ROM next to it and returns
     * it; otherwise returns null and leaves the file alone (mods and texture packs are zips too, without a ROM). The
     * game handles byte-swapped .n64/.v64 dumps itself.
     */
    private static File extractZippedRom(File file, File directory) {
        try (java.util.zip.ZipFile zip = new java.util.zip.ZipFile(file)) {
            java.util.Enumeration<? extends java.util.zip.ZipEntry> entries = zip.entries();
            while (entries.hasMoreElements()) {
                java.util.zip.ZipEntry entry = entries.nextElement();
                String name = entry.getName();
                String lower = name.toLowerCase(java.util.Locale.ROOT);
                if (entry.isDirectory() || !(lower.endsWith(".z64") || lower.endsWith(".n64") || lower.endsWith(".v64"))) {
                    continue;
                }
                // Only the file name: never a path from inside the archive.
                String baseName = new File(name).getName().replaceAll("[\\\\/:*?\"<>|]", "_");
                File rom = new File(directory, baseName);
                try (InputStream in = zip.getInputStream(entry); OutputStream out = new FileOutputStream(rom)) {
                    copyStream(in, out);
                }
                return rom;
            }
        } catch (IOException notAZip) {
            // Not a zip (or unreadable as one): use the file as it is.
        }
        return null;
    }

    private static void reportResult(int request, ArrayList<String> paths) {
        boolean ok = paths != null && !paths.isEmpty();
        if (request == REQUEST_OPEN_FILE) {
            nativeOnFileDialogResult(ok, ok ? paths.get(0) : "");
        } else {
            nativeOnFileDialogMultipleResult(ok, ok ? paths.toArray(new String[0]) : new String[0]);
        }
    }

    // HTTPS for the launcher (release list, official texture pack), called by the game on its own worker threads
    // (src/jfg/jfg_update_check.cpp). Android has no curl, which the desktop Linux build uses.

    /** The body of a small HTTPS GET (at most 4 MB), or null on any failure. */
    public static String httpGetText(String url) {
        HttpURLConnection connection = null;
        try {
            connection = openHttps(url, 8000);
            connection.setRequestProperty("Accept", "application/vnd.github+json");
            if (connection.getResponseCode() != 200) {
                return null;
            }
            java.io.ByteArrayOutputStream body = new java.io.ByteArrayOutputStream();
            try (InputStream in = connection.getInputStream()) {
                byte[] buffer = new byte[16384];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    if (body.size() + read > (4 << 20)) {
                        return null;
                    }
                    body.write(buffer, 0, read);
                }
            }
            return body.toString("UTF-8");
        } catch (Exception e) {
            Log.w(TAG, "GET " + url + " failed: " + e);
            return null;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    /**
     * Downloads url into path. With resumeFrom > 0 the file already holds that many bytes and only the rest is
     * requested; a server that sends the whole file anyway gets it written from the start. Returns true when the
     * download completed.
     */
    public static boolean httpDownload(String url, String path, long resumeFrom) {
        HttpURLConnection connection = null;
        try {
            connection = openHttps(url, 30000);
            connection.setRequestProperty("Accept", "application/octet-stream");
            if (resumeFrom > 0) {
                connection.setRequestProperty("Range", "bytes=" + resumeFrom + "-");
            }
            int status = connection.getResponseCode();
            boolean append = resumeFrom > 0 && status == 206;
            if (status != 200 && !append) {
                Log.w(TAG, "Download " + url + ": HTTP " + status);
                return false;
            }
            try (InputStream in = connection.getInputStream();
                 OutputStream out = new FileOutputStream(path, append)) {
                copyStream(in, out);
            }
            return true;
        } catch (Exception e) {
            Log.w(TAG, "Download " + url + " failed: " + e);
            return false;
        } finally {
            if (connection != null) {
                connection.disconnect();
            }
        }
    }

    // GitHub release assets redirect to another https host, which HttpURLConnection follows.
    private static HttpURLConnection openHttps(String url, int readTimeoutMs) throws IOException {
        if (!url.startsWith("https://")) {
            throw new IOException("only https is allowed");
        }
        HttpURLConnection connection = (HttpURLConnection) new URL(url).openConnection();
        connection.setInstanceFollowRedirects(true);
        connection.setConnectTimeout(8000);
        connection.setReadTimeout(readTimeoutMs);
        connection.setRequestProperty("User-Agent", "JetForceGemini-Recomp");
        return connection;
    }

    private String displayName(Uri uri) {
        String name = null;
        try (Cursor cursor = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                name = cursor.getString(0);
            }
        } catch (Exception ignored) {
        }
        if (name == null || name.isEmpty()) {
            name = "imported.bin";
        }
        return name.replaceAll("[\\\\/:*?\"<>|]", "_");
    }

    private static void copyStream(InputStream in, OutputStream out) throws IOException {
        byte[] buffer = new byte[1 << 16];
        int read;
        while ((read = in.read(buffer)) > 0) {
            out.write(buffer, 0, read);
        }
    }

    private static String readSmallFile(File file) {
        try (InputStream in = new java.io.FileInputStream(file)) {
            byte[] bytes = new byte[256];
            int read = in.read(bytes);
            return read > 0 ? new String(bytes, 0, read, "UTF-8") : "";
        } catch (IOException e) {
            return "";
        }
    }

    private static void deleteRecursive(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursive(child);
            }
        }
        file.delete();
    }
}
