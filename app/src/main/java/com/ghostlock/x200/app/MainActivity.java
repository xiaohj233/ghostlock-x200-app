/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * MainActivity — GhostLock X200 薄壳（Shizuku 版）。
 * 职责：加载本地 WebView 前端，注入 GhostBridge；
 *       检测/引导 Shizuku 授权 → 通过 Shizuku 以 shell 域（uid 2000）
 *       同步 payload 到 /data/local/tmp/ghostboot → 执行 runner → stdout 流式推给 JS。
 *
 * 设计约束（用户拍板）：
 *   - 不捆绑 / 不自启动 Shizuku，仅消费系统已装的 moe.shizuku.privileged.api；
 *   - 未授权 Shizuku 时 app 不可用（UI 引导授权，Shizuku 支持无线调试手机自举）；
 *   - 提权逻辑 100% 在 native runner 中，本类只做编排与展示。
 */
package com.ghostlock.x200.app;

import android.annotation.SuppressLint;
import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.RemoteException;
import android.util.Log;
import android.webkit.JavascriptInterface;
import android.webkit.WebChromeClient;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;

import org.json.JSONObject;

import java.io.BufferedReader;
import java.io.InputStream;
import java.io.InputStreamReader;

import moe.shizuku.server.IRemoteProcess;

public class MainActivity extends Activity {

    private static final String TAG = "GhostLockX200";
    static final String PREF = "ghostlock_prefs";
    static final String KEY_BOOT_AUTO = "boot_auto";
    private static final String ASSET_ROOT = "www";
    /** shell 域工作目录（untrusted_app 无权限，经 Shizuku 创建/写入） */
    static final String SHELL_WORK = "/data/local/tmp/ghostboot";

    private WebView webView;
    private Handler mainHandler = new Handler(Looper.getMainLooper());
    private volatile boolean running = false;

    @Override
    @SuppressLint("SetJavaScriptEnabled")
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        webView = new WebView(this);
        webView.setWebViewClient(new WebViewClient());
        webView.setWebChromeClient(new WebChromeClient() {
            @Override
            public boolean onConsoleMessage(android.webkit.ConsoleMessage cm) {
                Log.w(TAG, "console[" + cm.messageLevel() + "]: " + cm.message()
                        + " @ " + cm.sourceId() + ":" + cm.lineNumber());
                return true;
            }
        });
        if (android.os.Build.VERSION.SDK_INT >= 19) {
            WebView.setWebContentsDebuggingEnabled(true);
        }
        WebSettings ws = webView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setAllowFileAccess(true);

        webView.addJavascriptInterface(new GhostBridge(), "GhostBridge");
        webView.loadUrl("file:///android_asset/" + ASSET_ROOT + "/index.html");
        setContentView(webView);
    }

    @Override
    protected void onResume() {
        super.onResume();
        // 页面恢复时刷新环境 + Shizuku 状态（用户从 Shizuku app 授权返回后自动更新）
        webView.postDelayed(() -> {
            emitEnv();
            emitShizukuState();
        }, 800);
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == GhostShizuku.REQUEST_CODE) {
            mainHandler.postDelayed(this::emitShizukuState, 400);
        }
    }

    /* ---------- 事件注入到 JS ---------- */
    private void emit(String event, JSONObject payload) {
        mainHandler.post(() -> {
            if (webView == null) return;
            String js = "window.dispatchEvent(new CustomEvent('" + event + "',{detail:" + payload + "}))";
            webView.evaluateJavascript(js, null);
        });
    }

    private void emitLog(String line) {
        try {
            Log.i(TAG, "runner: " + line);   /* 诊断：同时进 logcat */
            JSONObject o = new JSONObject();
            o.put("line", line);
            emit("GhostLog", o);
        } catch (Exception e) { /* 忽略 */ }
    }

    private void emitStage(int index, String status) {
        try {
            JSONObject o = new JSONObject();
            o.put("index", index);
            o.put("status", status);
            emit("GhostStage", o);
        } catch (Exception e) { /* 忽略 */ }
    }

    private void emitDone(boolean ok, String text) {
        try {
            JSONObject o = new JSONObject();
            o.put("ok", ok);
            o.put("text", text);
            emit("GhostDone", o);
        } catch (Exception e) { /* 忽略 */ }
    }

    private void emitEnv() {
        try {
            JSONObject o = new JSONObject();
            o.put("model", android.os.Build.MODEL);
            o.put("kernel", System.getProperty("os.version", ""));
            o.put("android", android.os.Build.VERSION.RELEASE);
            o.put("root", hasSu());
            o.put("uptimeSec", android.os.SystemClock.elapsedRealtime() / 1000L);
            emit("GhostEnv", o);
        } catch (Exception e) { /* 忽略 */ }
    }

    private void emitShizukuState() {
        try {
            JSONObject o = new JSONObject();
            o.put("installed", GhostShizuku.installed(this));
            o.put("running", GhostShizuku.running());
            o.put("granted", GhostShizuku.granted());
            o.put("uid", GhostShizuku.uid());
            o.put("context", GhostShizuku.context());
            emit("GhostShizuku", o);
        } catch (Exception e) { /* 忽略 */ }
    }

    private boolean hasSu() {
        for (String p : new String[]{"/system/bin/su", "/system/xbin/su", "/sbin/su", "/data/adb/ksud"}) {
            if (new java.io.File(p).exists()) return true;
        }
        return false;
    }

    /* ---------- Shizuku shell 域执行 ---------- */

    /**
     * payload 同步脚本（shell 域执行）：
     * 从 APK 一次性解出 runner + payload（glt_esync/w2host/ksud/3 ko）到 /data/local/tmp/ghostboot。
     * runner 找 ko 约定为 work 根目录（stages.c:628-630 work/clear_vr_tag.ko 等）。
     * 机型子目录从 assets/device_id 读取（build_app.sh 写入），缺省回退 x200_b57。
     */
    private String buildSyncScript() {
        String apk = getApplicationInfo().sourceDir;
        return "set -e\n"
                + "D=" + SHELL_WORK + "\n"
                + "X=$D/.x\n"
                + "mkdir -p $D\n"
                + "rm -rf $X; mkdir -p $X\n"
                + "DID=$(unzip -p '" + apk + "' assets/device_id 2>/dev/null || echo x200_b57)\n"
                + "unzip -o '" + apk + "' 'assets/runner' assets/payload/$DID/* -d $X >/dev/null 2>&1\n"
                + "mv $X/assets/runner $D/runner\n"
                + "mv $X/assets/payload/$DID/glt_esync $D/glt_esync\n"
                + "mv $X/assets/payload/$DID/w2host $D/w2host\n"
                + "mv $X/assets/payload/$DID/ksud $D/ksud\n"
                + "mv $X/assets/payload/$DID/modules/clear_vr_tag.ko $D/clear_vr_tag.ko\n"
                + "mv $X/assets/payload/$DID/modules/kernelsu.ko $D/kernelsu.ko\n"
                + "mv $X/assets/payload/$DID/modules/permissive_restore.ko $D/permissive_restore.ko\n"
                + "rm -rf $X\n"
                + "chmod 755 $D/runner $D/glt_esync $D/w2host $D/ksud\n"
                + "chmod 644 $D/clear_vr_tag.ko $D/kernelsu.ko $D/permissive_restore.ko\n"
                + "echo '== payload 同步完成 =='\n"
                + "ls -la $D\n";
    }

    /** runner 启动命令（exec 后 stdout 即 runner 输出，流式可达） */
    private String buildRunnerCmd() {
        String tag = Long.toString(System.currentTimeMillis());
        return "exec " + SHELL_WORK + "/runner " + SHELL_WORK + " " + tag + " " + SHELL_WORK + "\n";
    }

    /** 执行 root（Shizuku shell 域全链） */
    private void runRoot() {
        if (running) return;
        if (!GhostShizuku.running() || !GhostShizuku.granted()) {
            emitShizukuState();
            emitLog("需要 Shizuku 授权（未运行或未授权），请按界面引导操作");
            return;
        }
        running = true;
        Thread t = new Thread(() -> {
            try {
                emitLog("== Shizuku shell 域 (uid " + GhostShizuku.uid() + ") ==");
                emitLog("== 同步 payload 到 " + SHELL_WORK + " ==");
                IRemoteProcess sp = GhostShizuku.newProcess(new String[]{"/system/bin/sh", "-c", buildSyncScript()});
                readStreamLoop(sp, false);
                int src = sp.waitFor();
                if (src != 0) {
                    emitDone(false, "payload 同步失败 rc=" + src);
                    return;
                }

                emitLog("== 启动 runner ==");
                IRemoteProcess p = GhostShizuku.newProcess(new String[]{"/system/bin/sh", "-c", buildRunnerCmd()});
                readStreamLoop(p, true);
                int rc = p.waitFor();
                boolean ok = rc == 0;
                emitStage(8, ok ? "done" : "fail");
                emitLog(ok ? "== runner 退出码 0 (成功) ==" : "== runner 退出码 " + rc + " (失败) ==");
                emitDone(ok, ok ? "Root 成功！" : "Root 失败，详见日志。");
            } catch (Throwable e) {
                Log.e(TAG, "runRoot 异常", e);
                emitLog("异常: " + e);
                emitDone(false, "异常: " + e.getMessage());
            } finally {
                running = false;
            }
        });
        t.setDaemon(true);
        t.start();
    }

    /**
     * 流式读取 IRemoteProcess 输出（stdout；STAGE 标记驱动进度）。
     * 每行做两件事：①整行推给 JS（GhostLog → 前端日志面板 + 报错分类器）；
     * ②按 runner 真实日志格式解析「已完成的 UI 阶段数」，单调推进 GhostStage ——
     *   旧实现只认含 PASS 的行，导致执行中途首页永远停在第一阶段，这里改为逐阶段推进。
     */
    private void readStreamLoop(IRemoteProcess p, boolean parseStage) {
        try {
            InputStream is = GhostShizuku.stream(p);
            BufferedReader r = new BufferedReader(new InputStreamReader(is));
            String line;
            int stageIdx = 0;
            while ((line = r.readLine()) != null) {
                if (line.isEmpty()) continue;
                emitLog(line);
                if (!parseStage) continue;
                int idx = stageDone(line);
                if (idx > stageIdx) {          // 单调推进：只前进不后退
                    stageIdx = idx;
                    emitStage(stageIdx, "done");
                }
            }
        } catch (RemoteException | java.io.IOException e) {
            Log.w(TAG, "读取 runner 输出流失败: " + e);
        }
    }

    /**
     * 解析 runner 一行日志 → 已完成的 UI 阶段数（0..8；与前端 stages 数组对齐：
     * 0准备 1SELinux 2kptr 3KASLR 4静默 5泄漏 6CAPSROOT 7装内核 8验证）。
     * 返回 -1 = 该行不推进进度（运行中/失败行由错误横幅承接，结束由 GhostDone 兜底）。
     */
    private int stageDone(String line) {
        if (line.contains("ALL STAGES PASS")) return 8;
        java.util.regex.Matcher m = java.util.regex.Pattern.compile("STAGE(\\d+)").matcher(line);
        if (!m.find()) return -1;
        int s;
        try { s = Integer.parseInt(m.group(1)); } catch (NumberFormatException e) { return -1; }
        boolean ok = line.contains("OK") || line.contains("SKIP");
        if (!ok) return -1;
        switch (s) {
            case 1: return 1;                    // SELinux 完成
            case 2: return 2;                    // kptr 完成
            case 3: return 3;                    // KASLR 完成
            case 4: return line.contains("candidate") ? 5 : (line.contains("quiesced") ? 4 : -1);
            case 5: return 6;                    // CAPSROOT 完成
            case 6: return 7;                    // 装内核 完成
            case 7: return 8;                    // 验证 完成
            default: return -1;
        }
    }

    /* ---------- Bridge ---------- */
    public class GhostBridge {

        @JavascriptInterface
        public void startRoot() {
            runRoot();
        }

        @JavascriptInterface
        public String getShizuku() {
            JSONObject o = new JSONObject();
            try {
                o.put("installed", GhostShizuku.installed(MainActivity.this));
                o.put("running", GhostShizuku.running());
                o.put("granted", GhostShizuku.granted());
                o.put("uid", GhostShizuku.uid());
                o.put("context", GhostShizuku.context());
            } catch (Exception e) { /* 忽略 */ }
            return o.toString();
        }

        @JavascriptInterface
        public void requestShizuku() {
            if (GhostShizuku.running() && !GhostShizuku.granted()) {
                GhostShizuku.request();
            } else {
                emitShizukuState();
            }
        }

        @JavascriptInterface
        public void openShizuku() {
            try {
                Intent i = getPackageManager().getLaunchIntentForPackage(GhostShizuku.MANAGER_PKG);
                if (i != null) startActivity(i);
                else emitLog("未安装 Shizuku，请先安装 moe.shizuku.privileged.api");
            } catch (Exception e) {
                emitLog("打开 Shizuku 失败: " + e);
            }
        }

        @JavascriptInterface
        public void getEnv() {
            emitEnv();
        }

        @JavascriptInterface
        public void setBootAuto(boolean on) {
            SharedPreferences sp = getSharedPreferences(PREF, MODE_PRIVATE);
            sp.edit().putBoolean(KEY_BOOT_AUTO, on).apply();
            emitLog(on ? "开机自启已开启（需 Shizuku 授权后生效）" : "开机自启已关闭");
        }

        @JavascriptInterface
        public boolean getBootAuto() {
            return getSharedPreferences(PREF, MODE_PRIVATE).getBoolean(KEY_BOOT_AUTO, false);
        }
    }
}
