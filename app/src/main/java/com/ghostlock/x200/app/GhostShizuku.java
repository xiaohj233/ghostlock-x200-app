/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * GhostShizuku — Shizuku 薄封装。
 *
 * 设计约束（用户拍板）：
 *   - 不捆绑 / 不自启动任何 Shizuku 本体，只消费系统已安装的
 *     moe.shizuku.privileged.api（Shizuku 支持无线调试手机自举，无需电脑）。
 *   - app 因 Shizuku 授权而获得 shell 域（uid 2000）执行能力，
 *     runner 全链在 shell 域运行（与 rish/adb 手工验证环境逐项一致）。
 *
 * 关键 API（dev.rikka.shizuku:api/aidl/provider 13.1.5）：
 *   Shizuku.pingBinder() / checkSelfPermission() / requestPermission(code)
 *   IShizukuService.newProcess(cmd, env, cwd) -> IRemoteProcess（可读 stdout）
 *   ShizukuProvider：authority = <applicationId>.shizuku
 */
package com.ghostlock.x200.app;

import android.content.Context;
import android.content.pm.PackageManager;
import android.os.IBinder;
import android.os.ParcelFileDescriptor;
import android.os.RemoteException;

import java.io.FileInputStream;
import java.io.IOException;
import java.io.InputStream;

import moe.shizuku.server.IRemoteProcess;
import moe.shizuku.server.IShizukuService;
import rikka.shizuku.Shizuku;

public class GhostShizuku {

    public static final int REQUEST_CODE = 10086;
    public static final String MANAGER_PKG = "moe.shizuku.privileged.api";

    /** Shizuku 管理器 app 是否已安装（app 不捆绑，依赖系统安装） */
    public static boolean installed(Context ctx) {
        try {
            ctx.getPackageManager().getPackageInfo(MANAGER_PKG, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /** Shizuku 服务是否运行中（adb/无线调试启动或手机自举） */
    public static boolean running() {
        return Shizuku.pingBinder();
    }

    /** 是否已授予 shell 域权限 */
    public static boolean granted() {
        try {
            return Shizuku.checkSelfPermission() == PackageManager.PERMISSION_GRANTED;
        } catch (Throwable t) {
            return false;
        }
    }

    /** 发起授权请求（Shizuku 弹窗，结果经 onRequestPermissionsResult 回调） */
    public static void request() {
        try {
            Shizuku.requestPermission(REQUEST_CODE);
        } catch (Throwable t) {
            /* 忽略：未运行等异常由状态查询兜底 */
        }
    }

    /** Shizuku 服务进程 uid（授权后应为 2000） */
    public static int uid() {
        try {
            return Shizuku.getUid();
        } catch (Throwable t) {
            return -1;
        }
    }

    /** Shizuku 服务 SELinux 上下文（授权后应为 u:r:shell:s0） */
    public static String context() {
        try {
            return Shizuku.getSELinuxContext();
        } catch (Throwable t) {
            return "n/a";
        }
    }

    /**
     * 以 shell 域启动进程。
     * @param cmd 命令行（可用 /system/bin/sh -c 包任意脚本）
     * @return IRemoteProcess（getInputStream/getErrorStream 可流式读，waitFor 阻塞）
     */
    public static IRemoteProcess newProcess(String[] cmd) throws RemoteException {
        IBinder b = Shizuku.getBinder();
        if (b == null) throw new IllegalStateException("Shizuku binder 不可用（服务未运行？）");
        IShizukuService svc = IShizukuService.Stub.asInterface(b);
        return svc.newProcess(cmd, null, null);
    }

    /** IRemoteProcess 输出流转 InputStream（ParcelFileDescriptor） */
    public static InputStream stream(IRemoteProcess p) throws RemoteException, IOException {
        ParcelFileDescriptor pfd = p.getInputStream();
        return new FileInputStream(pfd.getFileDescriptor());
    }
}
