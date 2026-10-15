/* SPDX-License-Identifier: Apache-2.0
 * Real filesystem regression tests. Faults intercept only persistence calls;
 * no kernel targets, device commands, modules or exploits are executed.
 */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

enum { NONE, WRITE_FAIL, FILE_SYNC_FAIL, RENAME_FAIL, DIR_SYNC_FAIL, SHORT_WRITE, INTERRUPTED_WRITE };
static int fault, transient;
static const char *boot = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa";
static const char *other_boot = "bbbbbbbb-bbbb-bbbb-bbbb-bbbbbbbbbbbb";
static char state_path[512];
static int cases;
static ssize_t test_write(int fd, const void *data, size_t length);
static int test_fsync(int fd);
static int test_renameat(int olddir, const char *old, int newdir, const char *newname);
#define write test_write
#define fsync test_fsync
#define renameat test_renameat
#include "../runner/gl.c"
#undef write
#undef fsync
#undef renameat

char *get_boot_id(void) { return boot ? strdup(boot) : NULL; }
static ssize_t test_write(int fd, const void *data, size_t length) {
    if (fault == WRITE_FAIL) { errno = ENOSPC; return -1; }
    if (!transient++ && fault == INTERRUPTED_WRITE) { errno = EINTR; return -1; }
    if (fault == SHORT_WRITE && length > 3) length = 3;
    return write(fd, data, length);
}
static int test_fsync(int fd) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    if ((fault == FILE_SYNC_FAIL && S_ISREG(st.st_mode)) ||
        (fault == DIR_SYNC_FAIL && S_ISDIR(st.st_mode))) { errno = EIO; return -1; }
    return fsync(fd);
}
static int test_renameat(int olddir, const char *old, int newdir, const char *newname) {
    if (fault == RENAME_FAIL) { errno = EACCES; return -1; }
    return renameat(olddir, old, newdir, newname);
}
static void fixture(const void *data, size_t length) {
    assert(unlink(state_path) == 0 || errno == ENOENT);
    int fd = open(state_path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    assert(fd >= 0);
    assert(write(fd, data, length) == (ssize_t)length);
    assert(close(fd) == 0);
}
static void saved(const char *bid, int slot, int atomic) {
    char data[128];
    int length = snprintf(data, sizeof(data), "%s\n%d\n%s", bid, slot,
                          atomic ? STATE_FORMAT "\n" : "");
    fixture(data, (size_t)length);
}
static char *snapshot(void) {
    char *data = calloc(512, 1);
    int fd = open(state_path, O_RDONLY);
    assert(fd >= 0 && data);
    assert(read(fd, data, 511) >= 0);
    assert(close(fd) == 0);
    return data;
}
static void unchanged(const char *old) {
    char *current = snapshot();
    assert(strcmp(old, current) == 0);
    free(current);
}
static int reserve(const char *work) {
    char address[64] = "stale address";
    const char *result = gl_lock_next(work, address, sizeof(address));
    if (!result) { assert(address[0] == '\0'); return -1; }
    uint64_t value = strtoull(address, NULL, 0);
    assert(value >= DEV_GL_LOCK_BASE);
    assert((value - DEV_GL_LOCK_BASE) % DEV_GL_SLOT_SIZE == 0);
    return (int)((value - DEV_GL_LOCK_BASE) / DEV_GL_SLOT_SIZE);
}
static void pass(const char *name) { printf("PASS: %s\n", name); cases++; }

int main(void) {
    char work[] = "work/tests/gl-state-XXXXXX";
    assert(mkdtemp(work));
    snprintf(state_path, sizeof(state_path), "%s/" STATE_FILE, work);

    assert(reserve(work) == 0 && reserve(work) == 2 && reserve(work) == 3);
    pass("successive calls/restarts skip the reserved bad slot");
    saved(other_boot, 72, 1);
    assert(reserve(work) == 0);
    pass("new boot resets consumed slots");

    saved(other_boot, 0, 0);
    assert(chmod(state_path, 0444) == 0);
    struct stat before, after;
    assert(stat(state_path, &before) == 0);
    assert(reserve(work) == 0);
    assert(stat(state_path, &after) == 0 && before.st_ino != after.st_ino);
    char *text = snapshot();
    assert(strstr(text, STATE_FORMAT "\n")); free(text);
    pass("read-only legacy file migrates by replacement after reboot");

    saved(boot, 0, 0);
    text = snapshot();
    assert(reserve(work) == -1 && errno == ESTALE);
    unchanged(text); free(text);
    pass("same-boot unreliable legacy history requires reboot");

    saved(boot, 2, 1);
    assert(chmod(state_path, 0444) == 0);
    assert(reserve(work) == 3);
    pass("read-only current state keeps slot history");

    const char *invalid[] = {"", "broken", "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n",
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n-2\n" STATE_FORMAT "\n",
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n999999999999999999999999\n",
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n0junk\n",
        "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n0\nunknown-format\n"};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        fixture(invalid[i], strlen(invalid[i]));
        assert(reserve(work) == -1);
    }
    const char nul[] = "aaaaaaaa-aaaa-aaaa-aaaa-aaaaaaaaaaaa\n0\0\n";
    fixture(nul, sizeof(nul)); assert(reserve(work) == -1);
    pass("corrupt/truncated/overflowing/NUL state never resets to slot zero");

    for (fault = WRITE_FAIL; fault <= RENAME_FAIL; fault++) {
        saved(boot, 4, 1);
        text = snapshot();
        assert(reserve(work) == -1);
        unchanged(text); free(text);
    }
    fault = NONE;
    assert(reserve(work) == 5);
    pass("write/file-fsync/rename failures return no address and preserve history");

    saved(boot, 8, 1);
    fault = DIR_SYNC_FAIL;
    assert(reserve(work) == -1);
    fault = NONE;
    assert(reserve(work) == 10);
    pass("post-rename directory-fsync failure consumes slot without executing it");

    for (fault = SHORT_WRITE; fault <= INTERRUPTED_WRITE; fault++) {
        saved(boot, 8, 1); transient = 0;
        assert(reserve(work) == 9);
    }
    fault = NONE;
    pass("short/interrupted writes finish before returning an address");

    saved(boot, LAST_SLOT, 1);
    text = snapshot();
    assert(reserve(work) == -1 && errno == ENOSPC);
    unchanged(text); free(text);
    pass("exhausted 4KiB slot buffer stops instead of overflowing");

    saved(boot, 2, 1);
    char tiny[2]; text = snapshot();
    assert(!gl_lock_next(work, tiny, sizeof(tiny)) && tiny[0] == '\0');
    unchanged(text); free(text);
    pass("short output buffer returns no truncated address");

    int fd = open(work, O_RDONLY | O_DIRECTORY);
    assert(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0);
    assert(reserve(work) == -1);
    close(fd);
    assert(reserve(work) == 3);
    pass("another allocator holds the directory lock");

    saved(boot, 44, 1); gl_state_reset(work);
    assert(reserve(work) == 0);
    saved(boot, 44, 1); fault = RENAME_FAIL; gl_state_reset(work); fault = NONE;
    assert(reserve(work) == 45);
    pass("host-clear reset is atomic; a failed reset preserves consumed slots");

    assert(unlink(state_path) == 0);
    assert(symlink("/dev/null", state_path) == 0 && reserve(work) == -1);
    assert(unlink(state_path) == 0);
    assert(mkfifo(state_path, 0600) == 0 && reserve(work) == -1);
    assert(unlink(state_path) == 0);
    assert(mkdir(state_path, 0700) == 0 && reserve(work) == -1);
    assert(rmdir(state_path) == 0);
    pass("symlink/FIFO/directory state is rejected without blocking");

    const char *real_boot = boot; boot = NULL;
    assert(reserve(work) == -1); boot = "invalid";
    assert(reserve(work) == -1); boot = real_boot;
    pass("missing/invalid boot ID stops reservation");

    fflush(stdout);
    int pipes[8][2]; pid_t children[8];
    for (int i = 0; i < 8; i++) {
        assert(pipe(pipes[i]) == 0);
        children[i] = fork(); assert(children[i] >= 0);
        if (children[i] == 0) {
            close(pipes[i][0]);
            int slot = -1;
            for (int retry = 0; slot < 0 && retry < 200; retry++) {
                slot = reserve(work);
                if (slot < 0) { assert(errno == EWOULDBLOCK || errno == EAGAIN); usleep(1000); }
            }
            assert(slot >= 0);
            assert(write(pipes[i][1], &slot, sizeof(slot)) == sizeof(slot));
            _exit(0);
        }
        close(pipes[i][1]);
    }
    int used[256] = {0};
    for (int i = 0; i < 8; i++) {
        int slot, status;
        assert(read(pipes[i][0], &slot, sizeof(slot)) == sizeof(slot));
        close(pipes[i][0]);
        assert(slot >= 0 && slot < 256 && slot != DEV_GL_SLOT_BAD && !used[slot]++);
        assert(waitpid(children[i], &status, 0) == children[i]);
        assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    pass("eight independent processes reserve different slots");

    DIR *directory = opendir(work); assert(directory);
    struct dirent *entry;
    while ((entry = readdir(directory))) assert(strncmp(entry->d_name, ".gl-state-", 10) != 0);
    closedir(directory);
    assert(unlink(state_path) == 0 && rmdir(work) == 0);
    printf("slot persistence: %d groups PASS\n", cases);
    return 0;
}
