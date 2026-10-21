// AppMeta -- what the Windows launcher shows for an installed app: its icon
// and its name.
//
// winapk-runtime --install-only calls write() in its own JVM, with the host
// natives bound, after installing the app and converting its code. The icon
// is upstream's: Boot.extractIconOnly renders the launcher icon (adaptive and
// vector ones included) to <app dir>/icon.png, as upstream's --extract-only
// install does for its library. The name is <application android:label>,
// resolved against the app's resources the same way, and goes to
// <app dir>/label.txt (UTF-8); the package name stands in when there is none.

package com.winapk.tools;

import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.content.res.Resources;
import android.util.DisplayMetrics;
import com.macapk.loader.Boot;
import com.macapk.loader.Manifest;
import java.io.File;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;

public final class AppMeta {
    private AppMeta() {}

    public static void write(String apkPath, String appDir) {
        Boot.extractIconOnly(apkPath, appDir, null);
        String label = null;
        String pkg = null;
        try {
            Manifest mf = Manifest.parse(new File(""), apkPath);
            pkg = mf.packageName;
            label = resolve(mf.applicationLabel, apkPath, pkg);
        } catch (Throwable t) {
            System.err.println("[win.apk] reading the app's name failed: " + t);
        }
        if (label == null || label.isEmpty()) label = pkg;
        if (label == null || label.isEmpty()) label = new File(apkPath).getName();
        try {
            Files.write(new File(appDir, "label.txt").toPath(), label.trim().getBytes(StandardCharsets.UTF_8));
        } catch (Exception e) {
            System.err.println("[win.apk] writing label.txt failed: " + e);
        }
    }

    // A literal label stays as it is; "@0x7f..." or "@string/name" is looked up.
    static String resolve(String raw, String apkPath, String pkg) throws Exception {
        if (raw == null || !raw.startsWith("@")) return raw;
        AssetManager assets = new AssetManager(apkPath);
        DisplayMetrics metrics = new DisplayMetrics();
        metrics.setToDefaults();
        Configuration cfg = Configuration.macapkPhoneDefaults();
        Resources res = new Resources(assets, metrics, cfg);
        Resources.macapkSetPackageName(pkg);
        String ref = raw.substring(1);
        int id = 0;
        if (ref.startsWith("0x") || ref.startsWith("0X")) {
            id = (int) Long.parseLong(ref.substring(2), 16);
        } else {
            int slash = ref.indexOf('/');
            String type = slash >= 0 ? ref.substring(0, slash) : "string";
            String name = slash >= 0 ? ref.substring(slash + 1) : ref;
            id = res.getIdentifier(name, type, pkg);
        }
        return id != 0 ? res.getString(id) : null;
    }
}
