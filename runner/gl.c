/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 *
 * Persist a slot reservation BEFORE returning its address. All callers share
 * the same directory lock, including across shell/root Shizuku sessions.
 * Legacy files may have ignored failed writes: migrate only after a reboot.
 */
#include "runner.h"
#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/file.h>
#include <sys/stat.h>

#define STATE_FILE "gl_lock_state.txt"
#define STATE_FORMAT "ghostlock-atomic-v1"
/* __dump_skip.zeroes is 4 KiB; never walk past the host buffer. */
#define LAST_SLOT ((int)(4096 / DEV_GL_SLOT_SIZE) - 1)

typedef struct {
    char boot_id[37];
    int last_slot;
    int atomic;
} SlotState;

static int valid_boot_id(const char *s) {
    if (!s || strlen(s) != 36) return 0;
    for (int i = 0; i < 36; i++) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return 0;
        } else if (!((s[i] >= '0' && s[i] <= '9') ||
                     (s[i] >= 'a' && s[i] <= 'f') ||
                     (s[i] >= 'A' && s[i] <= 'F'))) return 0;
    }
    return 1;
}

/* A directory lock avoids creating a root-owned lock file that shell could
 * not open later. Nonblocking: another allocator means stop. */
static int state_dir_lock(const char *work) {
    int fd = open(work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int error = errno;
        close(fd);
        errno = error;
        return -1;
    }
    return fd;
}

/* 0 = read; 1 = absent; -1 = unreadable/malformed. An I/O error or truncated
 * state must never be treated as a fresh boot and reuse slot zero. */
static int state_read(int dirfd, SlotState *state) {
    int fd = openat(dirfd, STATE_FILE, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat st;
    char data[160];
    size_t used = 0;
    int error = 0;
    if (fstat(fd, &st) != 0) error = errno;
    else if (!S_ISREG(st.st_mode)) error = EINVAL;
    while (!error && used < sizeof(data) - 1) {
        ssize_t n = read(fd, data + used, sizeof(data) - 1 - used);
        if (n < 0) { if (errno == EINTR) continue; error = errno; break; }
        if (n == 0) break;
        used += (size_t)n;
    }
    close(fd);
    if (error) { errno = error; return -1; }
    if (used == sizeof(data) - 1 || memchr(data, '\0', used)) goto invalid;
    data[used] = '\0';
    char *nl = strchr(data, '\n');
    if (!nl) goto invalid;
    *nl = '\0';
    if (!valid_boot_id(data)) goto invalid;
    char *number = nl + 1, *end;
    errno = 0;
    long slot = strtol(number, &end, 10);
    if (errno || end == number || (*end != '\n' && *end != '\0')) goto invalid;
    const char *marker = *end ? end + 1 : end;
    state->atomic = strcmp(marker, STATE_FORMAT "\n") == 0;
    if (*marker && !state->atomic) goto invalid;
    if (slot < (state->atomic ? -1 : 0) || slot > LAST_SLOT) goto invalid;
    memcpy(state->boot_id, data, sizeof(state->boot_id));
    state->last_slot = (int)slot;
    return 0;
invalid:
    errno = EINVAL;
    return -1;
}

/* Replace a readable root:root 0644 file via its writable parent directory.
 * Never unlink state to fix permissions: that could lose this boot's history. */
static int state_commit(int dirfd, const char *work, const char *boot_id, int slot) {
    char temporary[512], data[96];
    int n = snprintf(temporary, sizeof(temporary), "%s/.gl-state-XXXXXX", work);
    if (n < 0 || (size_t)n >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    n = snprintf(data, sizeof(data), "%s\n%d\n%s\n", boot_id, slot, STATE_FORMAT);
    int fd = mkstemp(temporary);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int error = 0;
    /* Readable across root/shell sessions; only the parent directory's owner
     * needs to replace it. No world-writable state file. */
    if (fchmod(fd, 0644) != 0) error = errno;
    for (size_t done = 0; !error && done < (size_t)n;) {
        ssize_t wrote = write(fd, data + done, (size_t)n - done);
        if (wrote < 0) { if (errno == EINTR) continue; error = errno; }
        else if (wrote == 0) error = EIO;
        else done += (size_t)wrote;
    }
    if (!error && fsync(fd) != 0) error = errno;
    if (close(fd) != 0 && !error) error = errno;
    if (!error && renameat(AT_FDCWD, temporary, dirfd, STATE_FILE) != 0) error = errno;
    if (!error && fsync(dirfd) != 0) error = errno;
    if (error) {
        unlink(temporary);
        errno = error;
        return -1;
    }
    return 0;
}

const char *gl_lock_next(const char *work, char *buf, size_t bufsz) {
    int dirfd = -1, slot = 0;
    char address[32];
    char *bid = get_boot_id();
    if (buf && bufsz) buf[0] = '\0';
    if (!valid_boot_id(bid) || !buf) { errno = EINVAL; goto fail; }
    dirfd = state_dir_lock(work);
    if (dirfd < 0) goto fail;
    SlotState saved;
    int status = state_read(dirfd, &saved);
    if (status < 0) goto fail;
    if (status == 0 && strcmp(saved.boot_id, bid) == 0) {
        if (!saved.atomic) {
            LOG("GL_STATE FAIL: legacy state from this boot; reboot required before retry");
            errno = ESTALE;
            goto fail;
        }
        slot = saved.last_slot + 1;
    }
    if (slot == DEV_GL_SLOT_BAD) slot++;
    if (slot > LAST_SLOT) { errno = ENOSPC; goto fail; }
    int n = snprintf(address, sizeof(address), "0x%llx",
                     (unsigned long long)(DEV_GL_LOCK_BASE + (uint64_t)slot * DEV_GL_SLOT_SIZE));
    if (n < 0 || (size_t)n >= bufsz) { errno = ENOBUFS; goto fail; }
    if (state_commit(dirfd, work, bid, slot) != 0) goto fail;
    memcpy(buf, address, (size_t)n + 1);
    LOG("GL slot=%d lock=%s boot=%.8s (state committed)", slot, buf, bid);
    close(dirfd);
    free(bid);
    return buf;
fail: {
    int error = errno;
    LOG("GL_STATE FAIL: cannot reserve slot (%s); no glt started", strerror(error));
    if (dirfd >= 0) close(dirfd);
    free(bid);
    errno = error;
    return NULL;
}
}

/* Only after permissive_restore clears the kernel host. A failed reset keeps
 * the previously consumed slots rather than resetting an unpersisted state. */
void gl_state_reset(const char *work) {
    char *bid = get_boot_id();
    int dirfd = -1;
    if (!valid_boot_id(bid)) { errno = EINVAL; goto fail; }
    dirfd = state_dir_lock(work);
    if (dirfd < 0 || state_commit(dirfd, work, bid, -1) != 0) goto fail;
    LOG("GL state reset after host clear (state committed)");
    close(dirfd);
    free(bid);
    return;
fail:
    LOG("GL_STATE WARN: reset not committed (%s); keep consumed slots", strerror(errno));
    if (dirfd >= 0) close(dirfd);
    free(bid);
}
