/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * rootcmd.c — w2host AF_UNIX 客户端。
 * 协议：每连接发一行 "CMD\n"，读全部响应（服务端处理完关闭）。
 * 命令：ID / READ <path> / WRITE <path> <hex> / LIST <dir> / CP <src> <dst> /
 *       DD <path> <off_hex> <len_hex> / RM <path> / RMMOD <name> /
 *       INSMOD <path> [k=v ...] / EXEC <cmd> / MKDIR <path> / CHMOD ... / STAT <path>
 */
#include "runner.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

char *rootcmd_call(const char *sock_path, const char *cmd) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return NULL;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return NULL;
    }
    size_t n = strlen(cmd);
    if (write(fd, cmd, n) != (ssize_t)n || write(fd, "\n", 1) != 1) {
        close(fd);
        return NULL;
    }
    /* 读全部（16MB 上限防失控） */
    size_t cap = 1 << 24, used = 0;
    char *buf = malloc(cap);
    if (!buf) { close(fd); return NULL; }
    ssize_t r;
    while ((r = read(fd, buf + used, cap - used - 1)) > 0) {
        used += (size_t)r;
        if (used + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); close(fd); return NULL; }
            buf = nb;
        }
    }
    close(fd);
    buf[used] = '\0';
    return buf;
}
