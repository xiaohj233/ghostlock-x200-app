/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * gl.c — GL 槽位轮换（root.ps1 Get-GLLock 的 C 移植）。
 * 状态文件 <work>/gl_lock_state.txt：首行 boot_id，次行 slot。
 * boot_id 变化 → slot=0；slot==1 → 用 2（宿主有毒）；lock = GL_LOCK_BASE + slot*0x10。
 */
#include "runner.h"
#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *gl_lock_next(const char *work, char *buf, size_t bufsz) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/gl_lock_state.txt", work);

    char *bid = get_boot_id();
    if (!bid) return NULL;

    char *saved_bid = NULL;
    int slot = 0;
    char *content = read_file(state_path);
    if (content) {
        char *nl = strchr(content, '\n');
        if (nl) {
            *nl = '\0';
            saved_bid = strdup(content);
            slot = atoi(nl + 1);
        }
        free(content);
    }

    if (!saved_bid || strcmp(saved_bid, bid) != 0) {
        slot = 0;                       /* 新 boot：从头 */
    } else {
        slot++;                          /* 同 boot：下一个槽位 */
        if (slot == DEV_GL_SLOT_BAD) slot = 2;
    }
    free(saved_bid);

    char new_state[512];
    snprintf(new_state, sizeof(new_state), "%s\n%d\n", bid, slot);
    write_file(state_path, new_state);
    free(bid);

    snprintf(buf, bufsz, "0x%llx", (unsigned long long)(DEV_GL_LOCK_BASE + (uint64_t)slot * DEV_GL_SLOT_SIZE));
    return buf;
}

void gl_state_reset(const char *work) {
    char state_path[512];
    snprintf(state_path, sizeof(state_path), "%s/gl_lock_state.txt", work);
    remove(state_path);
    LOG("GL state reset (boot_id cleared)");
}
