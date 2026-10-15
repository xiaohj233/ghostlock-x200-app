#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 GhostLock-X200-App contributors
"""Run the real Java resolver against small fake Android package metadata.
No installed Android apps are executed. Requires JDK 11+, unzip and sha256sum.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
stubs = {
    'android/content/Context.java': '''package android.content;
public class Context { public android.content.pm.PackageManager pm = new android.content.pm.PackageManager();
public android.content.pm.PackageManager getPackageManager() { return pm; } }''',
    'android/content/pm/PackageManager.java': '''package android.content.pm;
public class PackageManager { public java.util.Map<String,PackageInfo> apps = new java.util.HashMap<>();
public PackageInfo getPackageInfo(String name, int flags) throws NameNotFoundException {
if (!apps.containsKey(name)) throw new NameNotFoundException(); return apps.get(name); }
public static class NameNotFoundException extends Exception {} }''',
    'android/content/pm/PackageInfo.java': '''package android.content.pm;
public class PackageInfo { public String versionName; public int versionCode;
public ApplicationInfo applicationInfo; public long getLongVersionCode() { return versionCode; } }''',
    'android/content/pm/ApplicationInfo.java': '''package android.content.pm;
public class ApplicationInfo { public boolean enabled = true; public String sourceDir, nativeLibraryDir;
public String[] splitSourceDirs; }''',
    'android/os/Build.java': '''package android.os;
public class Build { public static class VERSION { public static int SDK_INT = 35; } }''',
}
harness = r'''package com.ghostlock.x200.app;
import android.content.Context;
import android.content.pm.*;
import java.nio.file.*;
import java.util.zip.*;
public class ResolverTest {
    static byte[] elf() {
        byte[] b = new byte[40]; b[0]=127; b[1]='E'; b[2]='L'; b[3]='F';
        b[4]=2; b[5]=1; b[16]=3; b[18]=(byte)183; return b;
    }
    static void apk(Path p, byte[] bytes) throws Exception {
        try (ZipOutputStream z = new ZipOutputStream(Files.newOutputStream(p))) {
            z.putNextEntry(new ZipEntry(bytes == null ? "empty" : "lib/arm64-v8a/libksud.so"));
            if (bytes != null) z.write(bytes); z.closeEntry();
        }
    }
    static PackageInfo info(Path p) {
        PackageInfo i = new PackageInfo(); i.versionName="test"; i.versionCode=123;
        i.applicationInfo=new ApplicationInfo(); i.applicationInfo.sourceDir=p.toString(); return i;
    }
    static void require(boolean condition) { if (!condition) throw new AssertionError(); }
    static int copy(RootManager.Asset asset, Path dest) throws Exception {
        return new ProcessBuilder("sh", "-c", "set -e\nD="+RootManager.quote(dest.toString())+"\n"+asset.copyScript())
                .redirectErrorStream(true).redirectOutput(ProcessBuilder.Redirect.DISCARD).start().waitFor();
    }
    public static void main(String[] args) throws Exception {
        Path dir=Paths.get(args[0]); Context c=new Context();
        require(RootManager.installed(c).isEmpty());
        Path base=dir.resolve("base '$(touch SHOULD_NOT_EXIST).apk"); apk(base, elf());
        PackageInfo i=info(base); c.pm.apps.put("me.weishu.kernelsu",i);
        RootManager m=RootManager.installed(c).get(0);
        require(m.versionCode==123 && m.version.equals("test"));
        RootManager.Asset asset=m.resolve(); require(asset.entry.equals("lib/arm64-v8a/libksud.so"));
        Path dest=dir.resolve("out"); Files.createDirectory(dest);
        require(copy(asset,dest)==0);
        require(java.util.Arrays.equals(Files.readAllBytes(dest.resolve("ksud")),elf()));
        require(Files.readString(dest.resolve("ksud.sha256")).equals(asset.sha256+"\n"));
        byte[] changed=elf(); changed[30]=1; apk(base,changed);
        require(copy(asset,dest)!=0);
        require(java.util.Arrays.equals(Files.readAllBytes(dest.resolve("ksud")),elf()));
        Path split=dir.resolve("split.apk"); apk(base,null); apk(split,elf());
        i.applicationInfo.splitSourceDirs=new String[]{split.toString()};
        require(m.resolve().path.equals(split.toString()));
        changed[18]=62; apk(split,changed);
        boolean rejected=false; try { m.resolve(); } catch (java.io.IOException e) { rejected=true; }
        require(rejected);
        i.applicationInfo.splitSourceDirs=null; Path nativeDir=dir.resolve("native"); Files.createDirectory(nativeDir);
        Files.write(nativeDir.resolve("libksud.so"),elf()); i.applicationInfo.nativeLibraryDir=nativeDir.toString();
        require(m.resolve().entry==null);
        require(copy(m.resolve(),dest)==0);
        c.pm.apps.put("com.sukisu.ultra",info(base));
        require(RootManager.installed(c).size()==2);
        i.applicationInfo.enabled=false;
        require(RootManager.installed(c).size()==1);
        require(!Files.exists(Paths.get("SHOULD_NOT_EXIST")));
        System.out.println("manager resolver: detection/base/split/native/ELF/hash-race/quoting PASS");
    }
}'''
with tempfile.TemporaryDirectory(prefix='manager-test-', dir=ROOT/'work') as directory:
    work = Path(directory)
    for name, content in stubs.items():
        p = work/name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(content)
    test = work/'com/ghostlock/x200/app/ResolverTest.java'
    test.parent.mkdir(parents=True)
    test.write_text(harness)
    classes = work/'classes'
    files = list(work.rglob('*.java')) + [ROOT/'app/src/main/java/com/ghostlock/x200/app/RootManager.java']
    subprocess.run(['javac', '--release', '11', '-d', str(classes), *map(str, files)], check=True)
    subprocess.run(['java', '-cp', str(classes), 'com.ghostlock.x200.app.ResolverTest', str(work)], check=True, cwd=work)
