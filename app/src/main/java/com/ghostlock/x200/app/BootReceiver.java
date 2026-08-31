/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * BootReceiver — 开机自启（默认关闭，需用户在 UI 开启）。
 * 设计：静置检测（延迟 + loadavg 判断）后触发 runner；
 * 首版仅做启动 MainActivity 记录日志，自动触发留待真机验证后启用。
 */
package com.ghostlock.x200.app;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.util.Log;

public class BootReceiver extends BroadcastReceiver {

    private static final String TAG = "GhostLockX200";

    @Override
    public void onReceive(Context context, Intent intent) {
        SharedPreferences sp = context.getSharedPreferences(MainActivity.PREF, Context.MODE_PRIVATE);
        boolean auto = sp.getBoolean(MainActivity.KEY_BOOT_AUTO, false);
        Log.i(TAG, "BOOT_COMPLETED auto=" + auto);
        if (!auto) return;

        // 首版：仅启动界面并记录，自动执行逻辑在真机验证阶段接入
        Intent i = new Intent(context, MainActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        context.startActivity(i);
    }
}
