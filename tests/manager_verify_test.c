/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 GhostLock-X200-App contributors
 * Host-only tests: no device access, module loading or privilege changes.
 */
#include <assert.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
static time_t clock_now;
static int scenario, attempts;
static char saved_status[64];
static uid_t fake_uid(void) { return scenario >= 8 && scenario != 11 ? 0 : 1000; }
static time_t fake_time(time_t *out) { if (out) *out = clock_now; return clock_now; }
#define time fake_time
#define read_file fake_read_file
#define sleep_s fake_sleep
#define exec_timeout fake_exec_timeout
#define getuid fake_uid
#define geteuid fake_uid
#define write_file fake_write_file
#include "../runner/stages.c"
#undef time
#undef read_file
#undef sleep_s
#undef exec_timeout
#undef getuid
#undef geteuid
#undef write_file
static const char *hash = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
char *fake_read_file(const char *path) {
    if (strstr(path, "ksud.sha256")) {
        if (scenario == 5) return NULL;
        char *value = malloc(66); snprintf(value, 66, "%s\n", hash); return value;
    }
    if (strstr(path, "manager_load.status"))
        return strdup(saved_status[0] ? saved_status : scenario == 1 ? "MANAGER_FAIL 137\n" : scenario == 7 ? "MANAGER_EXISTING 0\n" : "MANAGER_EXIT 0\n");
    if (strcmp(path, "/proc/modules") == 0)
        return strdup((scenario == 2 || scenario == 10) ? "clear_vr_tag 1 0 - Live 0\n" : "kernelsu 1 0 - Live 0\n");
    if (strstr(path, "manager_identity.log")) {
        char *value = malloc(128);
        snprintf(value, 128, "%s  /data/adb/ksud\n", scenario == 9 ? "different" : hash);
        return value;
    }
    if (strstr(path, "manager_verify.log")) {
        char *value = malloc(256);
        snprintf(value, 256, "%s\n%s  /data/adb/ksud\n",
                 scenario == 3 ? "uid=2000(shell)" : "uid=0(root)",
                 scenario == 4 ? "wrong daemon" : hash);
        return value;
    }
    return NULL;
}
int fake_write_file(const char *path, const char *data) {
    if (strstr(path, "manager_load.status")) snprintf(saved_status, sizeof(saved_status), "%s", data);
    return 0;
}
void fake_sleep(int seconds) { clock_now += seconds; }
int fake_exec_timeout(const char *path, char *const argv[], char *const envp[],
                      const char *log, int seconds, int *code) {
    (void)path; (void)argv; (void)envp; (void)log; (void)seconds;
    attempts++;
    *code = scenario == 6 ? 1 : 0;
    return 0;
}
int main(void) {
    RunnerCfg cfg = {.work="/fixture", .manager_package="me.weishu.kernelsu"};
    for (scenario = 0; scenario < 8; scenario++) {
        clock_now = 0; attempts = 0; saved_status[0] = 0;
        int result = manager_verify(&cfg);
        assert((result == 0) == (scenario == 0 || scenario == 7));
        if (scenario == 1 || scenario == 2 || scenario == 5) assert(attempts == 0);
    }
    for (scenario = 8; scenario <= 12; scenario++) {
        clock_now = 0; attempts = 0; saved_status[0] = 0;
        cfg.manager_package = scenario == 12 ? NULL : "me.weishu.kernelsu";
        int result = stage_root_session(&cfg);
        assert((result == 0) == (scenario == 8 || scenario == 12));
        if (scenario == 9) assert(attempts == 1); // identity check only, no loader
        if (scenario == 10) assert(attempts == 1); // loader exited, module absent
        if (scenario == 11 || scenario == 12) assert(attempts == 0);
    }
    puts("manager verification/root session: 13 scenarios PASS");
    return 0;
}
