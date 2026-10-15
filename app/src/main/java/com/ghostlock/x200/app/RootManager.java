/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 */
package com.ghostlock.x200.app;

import android.content.Context;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import java.io.File;
import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.List;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/** Discovers installed managers and resolves their own ARM64 daemon, without executing it. */
final class RootManager {
    static final String PREF_PACKAGE = "root_manager_package";
    private static final String[] PACKAGES = {
        "me.weishu.kernelsu", "com.sukisu.ultra", "com.resukisu.resukisu"
    };
    private static final String[] LABELS = {"KernelSU", "SukiSU", "ReSukiSU"};
    final String packageName, label, version;
    final long versionCode;
    private final ApplicationInfo application;

    private RootManager(String pkg, String name, PackageInfo info) {
        packageName = pkg;
        label = name;
        version = info.versionName == null ? "未知版本" : info.versionName;
        versionCode = android.os.Build.VERSION.SDK_INT >= 28
                ? info.getLongVersionCode() : info.versionCode;
        application = info.applicationInfo;
    }

    static List<RootManager> installed(Context context) {
        List<RootManager> result = new ArrayList<>();
        for (int i = 0; i < PACKAGES.length; i++) {
            try {
                PackageInfo info = context.getPackageManager().getPackageInfo(PACKAGES[i], 0);
                if (info.applicationInfo != null && info.applicationInfo.enabled)
                    result.add(new RootManager(PACKAGES[i], LABELS[i], info));
            } catch (PackageManager.NameNotFoundException ignored) { }
        }
        return result;
    }

    String description() { return label + " " + version + " (" + versionCode + ")"; }

    static final class Asset {
        final String path, entry, sha256;
        Asset(String path, String entry, String hash) {
            this.path = path; this.entry = entry; sha256 = hash;
        }
        String copyScript() {
            String copy = entry == null ? "cp " + quote(path)
                    : "unzip -p " + quote(path) + " " + quote(entry);
            return (entry == null ? copy + " \"$D/ksud.next\""
                    : copy + " > \"$D/ksud.next\"") + "\n"
                    + "ACTUAL=$(sha256sum \"$D/ksud.next\"); ACTUAL=${ACTUAL%% *}\n"
                    + "[ \"$ACTUAL\" = " + quote(sha256) + " ] || { echo '管理器资产校验失败，请重试'; exit 1; }\n"
                    + "mv \"$D/ksud.next\" \"$D/ksud\"\n"
                    + "printf '%s\\n' " + quote(sha256) + " > \"$D/ksud.sha256\"\n";
        }
    }

    Asset resolve() throws Exception {
        // Search APKs first: this avoids accidentally preferring an x86 library directory.
        List<String> apks = new ArrayList<>();
        apks.add(application.sourceDir);
        if (application.splitSourceDirs != null)
            java.util.Collections.addAll(apks, application.splitSourceDirs);
        for (String path : apks) {
            try (ZipFile zip = new ZipFile(path)) {
                ZipEntry entry = zip.getEntry("lib/arm64-v8a/libksud.so");
                if (entry != null) {
                    try (InputStream stream = zip.getInputStream(entry)) {
                        return new Asset(path, entry.getName(), validateAndHash(stream));
                    }
                }
            }
        }
        if (application.nativeLibraryDir != null) {
            File file = new File(application.nativeLibraryDir, "libksud.so");
            if (file.isFile()) {
                try (InputStream stream = new FileInputStream(file)) {
                    return new Asset(file.getAbsolutePath(), null, validateAndHash(stream));
                }
            }
        }
        throw new IOException(description() + " 未包含 ARM64 libksud.so，请安装支持 late-load 的版本");
    }

    private static String validateAndHash(InputStream input) throws Exception {
        byte[] header = new byte[20];
        int used = 0;
        while (used < header.length) {
            int n = input.read(header, used, header.length - used);
            if (n < 0) throw new IOException("管理器加载程序不完整");
            used += n;
        }
        int type = (header[16] & 255) | ((header[17] & 255) << 8);
        if (header[0] != 127 || header[1] != 'E' || header[2] != 'L' || header[3] != 'F'
                || header[4] != 2 || header[5] != 1 || (type != 2 && type != 3)
                || (header[18] & 255) != 183 || header[19] != 0)
            throw new IOException("管理器加载程序不是 ARM64 ELF");
        MessageDigest digest = MessageDigest.getInstance("SHA-256");
        digest.update(header);
        byte[] buffer = new byte[65536];
        long total = used;
        int n;
        while ((n = input.read(buffer)) != -1) {
            total += n;
            if (total > 128L * 1024 * 1024) throw new IOException("管理器加载程序过大");
            digest.update(buffer, 0, n);
        }
        StringBuilder hash = new StringBuilder();
        for (byte b : digest.digest()) hash.append(String.format(java.util.Locale.ROOT, "%02x", b & 255));
        return hash.toString();
    }

    static String quote(String text) { return "'" + text.replace("'", "'\"'\"'") + "'"; }
}
