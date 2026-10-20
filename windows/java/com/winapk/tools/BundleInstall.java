// BundleInstall -- installs a split APK bundle the way a device's installer
// leaves it, for the Windows host.
//
//     java -cp winapk-tools.jar com.winapk.tools.BundleInstall <bundle> <dir>
//
// Apps from the Play Store come as a base APK plus config and feature splits,
// packed by the store mirrors as .apkm (APKMirror), .xapk (APKPure) or .apks
// (bundletool). Upstream's runtime and its DEX converter read an installed app
// as `pm install-multiple` lays it out (runtime/java/com/macapk/loader/
// InstalledSplits.java):
//
//     <dir>/base.apk
//     <dir>/split_<name>.apk ...
//
// and on the Mac its C++ unpacker writes that layout (apk_unpacker.cpp). The
// Windows host has no zip reader of its own, so it runs this instead. Which
// APKs make up the app is decided exactly as upstream's SelectBundleApks
// decides it; every split is installed, the native ABI is chosen later, when
// the app loads its libraries, as upstream does.
//
// Exit codes: 0 installed, 2 usage, 3 not a bundle with an APK in it, 1 error.

package com.winapk.tools;

import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.DirectoryStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.nio.file.StandardCopyOption;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Enumeration;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

public final class BundleInstall {
    private BundleInstall() {}

    public static void main(String[] args) {
        if (args.length != 2) {
            System.err.println("usage: BundleInstall <bundle> <dir>");
            System.exit(2);
        }
        try {
            System.exit(install(Paths.get(args[0]), Paths.get(args[1])));
        } catch (IOException e) {
            System.err.println("[bundle] " + e);
            System.exit(1);
        }
    }

    static int install(Path bundle, Path dir) throws IOException {
        try (ZipFile zip = new ZipFile(bundle.toFile())) {
            List<ZipEntry> apks = new ArrayList<>();
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements(); ) {
                ZipEntry z = e.nextElement();
                if (!z.isDirectory() && z.getName().endsWith(".apk")) apks.add(z);
            }
            if (apks.isEmpty()) {
                System.err.println("[bundle] no APK inside " + bundle);
                return 3;
            }
            Map<ZipEntry, String> plan = select(apks, xapkBase(zip), packageGuess(bundle));

            Files.createDirectories(dir);
            // A reinstall replaces the whole set: a split the new bundle no
            // longer has must not stay behind.
            try (DirectoryStream<Path> old = Files.newDirectoryStream(dir, "*.apk")) {
                for (Path p : old) Files.delete(p);
            }
            for (Map.Entry<ZipEntry, String> m : plan.entrySet()) {
                Path target = dir.resolve(m.getValue());
                Path part = dir.resolve(m.getValue() + ".part");
                try (InputStream in = zip.getInputStream(m.getKey())) {
                    Files.copy(in, part, StandardCopyOption.REPLACE_EXISTING);
                }
                Files.move(part, target, StandardCopyOption.REPLACE_EXISTING);
                System.err.println("[bundle] " + m.getKey().getName() + " -> " + m.getValue());
            }
        }
        return 0;
    }

    /** Upstream's SelectBundleApks, returning each chosen entry with the file
     *  name it is installed under, the base first. */
    static Map<ZipEntry, String> select(List<ZipEntry> apks, String manifestBase, String pkg) {
        Map<ZipEntry, String> plan = new LinkedHashMap<>();

        // bundletool .apks: a split set under splits/ (and system/,
        // asset-slices/), named <module>-<config>.apk.
        List<ZipEntry> set = new ArrayList<>();
        set.addAll(under(apks, "system/"));
        set.addAll(under(apks, "splits/"));
        set.addAll(under(apks, "asset-slices/"));
        if (!under(apks, "splits/").isEmpty() || !under(apks, "system/").isEmpty()) {
            set.sort(Comparator.comparingInt(BundleInstall::bundletoolRank)
                    .thenComparing(ZipEntry::getName));
            plan.put(set.get(0), "base.apk");
            for (ZipEntry z : set.subList(1, set.size())) plan.put(z, bundletoolSplitFile(baseName(z)));
            return plan;
        }
        for (ZipEntry z : apks) {
            if (z.getName().equals("universal.apk")) {
                plan.put(z, "base.apk");
                return plan;
            }
        }
        // Only whole-app alternatives: exactly one is the app, the largest.
        List<ZipEntry> whole = under(apks, "standalones/");
        if (whole.isEmpty()) whole = under(apks, "instant/");
        if (whole.isEmpty()) whole = under(apks, "archive/");
        if (!whole.isEmpty()) {
            plan.put(largest(whole), "base.apk");
            return plan;
        }

        // Root layout (.apkm, .xapk): the manifest.json's base, base.apk,
        // <package>.apk, or else the largest APK, which carries the DEX.
        ZipEntry base = null;
        if (manifestBase != null) base = named(apks, manifestBase);
        if (base == null) base = named(apks, "base.apk");
        if (base == null && pkg != null) base = named(apks, pkg + ".apk");
        if (base == null) base = largest(apks);
        plan.put(base, "base.apk");
        List<ZipEntry> rest = new ArrayList<>(apks);
        rest.remove(base);
        rest.sort(Comparator.comparing(ZipEntry::getName));
        for (ZipEntry z : rest) plan.put(z, rootSplitFile(baseName(z)));
        return plan;
    }

    /** system/ first, then the base module's master, then its config splits. */
    static int bundletoolRank(ZipEntry z) {
        if (z.getName().startsWith("system/")) return 0;
        String bn = baseName(z);
        if (bn.equals("base-master.apk")) return 1;
        if (bn.startsWith("base-")) return 2;
        return 3;
    }

    /** bundletool names a split <module>-<config>.apk ("base-xxhdpi.apk",
     *  "feature-master.apk"); the installed split name is the one the split's
     *  own manifest declares: config.<q> for the base module's config splits,
     *  <module> and <module>.config.<q> for a feature module's. */
    static String bundletoolSplitFile(String bn) {
        String stem = bn.substring(0, bn.length() - 4);
        int dash = stem.indexOf('-');
        if (dash < 0) return "split_" + stem + ".apk";
        String module = stem.substring(0, dash), config = stem.substring(dash + 1);
        if (module.equals("base")) return "split_config." + config + ".apk";
        if (config.equals("master")) return "split_" + module + ".apk";
        return "split_" + module + ".config." + config + ".apk";
    }

    /** APKMirror already names splits split_config.<q>.apk; APKPure names
     *  them config.<q>.apk; anything else keeps its own name. */
    static String rootSplitFile(String bn) {
        return bn.startsWith("split_") ? bn : "split_" + bn;
    }

    /** APKPure's manifest.json names the base: {"file": "x.apk", "id": "base"}. */
    static String xapkBase(ZipFile zip) throws IOException {
        ZipEntry m = zip.getEntry("manifest.json");
        if (m == null) return null;
        String json;
        try (InputStream in = zip.getInputStream(m)) {
            json = new String(in.readAllBytes(), StandardCharsets.UTF_8);
        }
        Matcher obj = Pattern.compile("\\{[^{}]*\\}").matcher(json);
        while (obj.find()) {
            String o = obj.group();
            if (!o.matches("(?s).*\"id\"\\s*:\\s*\"base\".*")) continue;
            Matcher file = Pattern.compile("\"file\"\\s*:\\s*\"([^\"]+)\"").matcher(o);
            if (file.find()) return file.group(1);
        }
        return null;
    }

    /** "com.example.app_1.2.3.xapk" -> "com.example.app": a hint only. */
    static String packageGuess(Path bundle) {
        String n = bundle.getFileName().toString();
        int dot = n.lastIndexOf('.');
        if (dot > 0) n = n.substring(0, dot);
        int us = n.indexOf('_');
        return us > 0 ? n.substring(0, us) : n;
    }

    static List<ZipEntry> under(List<ZipEntry> apks, String dir) {
        List<ZipEntry> out = new ArrayList<>();
        for (ZipEntry z : apks) if (z.getName().startsWith(dir)) out.add(z);
        return out;
    }

    static ZipEntry named(List<ZipEntry> apks, String name) {
        for (ZipEntry z : apks) if (z.getName().equals(name)) return z;
        return null;
    }

    static ZipEntry largest(List<ZipEntry> apks) {
        ZipEntry best = apks.get(0);
        for (ZipEntry z : apks) if (z.getSize() > best.getSize()) best = z;
        return best;
    }

    static String baseName(ZipEntry z) {
        String n = z.getName();
        return n.substring(n.lastIndexOf('/') + 1);
    }
}
