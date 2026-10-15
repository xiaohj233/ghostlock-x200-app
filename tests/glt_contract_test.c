/* SPDX-License-Identifier: Apache-2.0
 * Host-only orchestration regression: intercept exec; NEVER run an exploit.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../runner/runner.h"

static int executions, reserve_fails, active_stage, scenario, cleanups;
long get_uptime_seconds(void) { return 600; }
int getenforce_permissive(void) { return executions ? 1 : 0; }
int enforce_readable(void) { return 1; }
long get_kptr_restrict(void) { return executions ? 0 : 1; }
const char *gl_lock_next(const char *work, char *buf, size_t size) {
    (void)work;
    if (reserve_fails) return NULL;
    snprintf(buf, size, "0xffffff8002342840");
    return buf;
}
int cleanup_exec(const char *path, int milliseconds) {
    (void)milliseconds;
    if (!strstr(path, "glt_esync")) return 0;
    cleanups++;
    return (scenario == 2 && cleanups == 1) || (scenario == 3 && cleanups == 2) ? -1 : 0;
}
void runner_event(const RunnerCfg *cfg, const char *event, int result) {
    (void)cfg; (void)event; (void)result;
}
void sleep_s(int seconds) { (void)seconds; }
int sh_run(const char *cmd, char *out, size_t size) {
    (void)cmd;
    if (out && size) *out = '\0';
    return 0;
}
char *read_file(const char *path) {
    if (active_stage == 5 && executions && strstr(path, "/proof.txt")) return strdup("CAPSROOT");
    return NULL;
}
uint64_t symtab_capsym(const SymTab *table, uint64_t base) {
    (void)table; (void)base;
    return 0xffffffc012345678ULL;
}
int exec_timeout(const char *path, char *const argv[], char *const envp[],
                 const char *log, int seconds, int *code) {
    (void)path; (void)argv; (void)log; (void)seconds;
    int no_sched = 0, task_w2 = 0, probe = 0, lock = 0;
    for (int i = 0; envp[i]; i++) {
        const char *equal = strchr(envp[i], '=');
        assert(equal);
        size_t length = (size_t)(equal - envp[i]) + 1;
        for (int j = 0; j < i; j++) assert(strncmp(envp[i], envp[j], length) != 0);
        if (strcmp(envp[i], "GL_NO_SCHED=1") == 0) no_sched++;
        if (strcmp(envp[i], "GL_TASK_W2=1") == 0) task_w2++;
        if (strcmp(envp[i], "GL_PROBE=1") == 0) probe++;
        if (strcmp(envp[i], "GL_LOCK=0xffffff8002342840") == 0) lock++;
    }
    assert(no_sched == 1 && lock == 1);
    assert(task_w2 == (active_stage != 2));
    assert(probe == (active_stage != 2));
    executions++;
    if (code) *code = 0;
    return scenario == 4 ? -1 : 0;
}
int main(void) {
    RunnerCfg cfg = {.work="/nonexistent-glt-test", .tag="offline"};
    SymTab table = {.kbase=0xffffffc000000000ULL};
    const int stages[] = {1, 2, 5};
    for (int index = 0; index < 3; index++) {
        active_stage = stages[index];
        for (scenario = 0; scenario < 5; scenario++) {
            reserve_fails = scenario == 1;
            executions = 0; cleanups = 0;
            int rc = active_stage == 1 ? stage1_permissive(&cfg) :
                     active_stage == 2 ? stage2_kptr(&cfg) :
                     stage5_capsroot(&cfg, &table, "0xffffff8001234000");
            assert((rc == 0) == (scenario == 0));
            assert(executions == (scenario == 0 || scenario == 3 || scenario == 4));
            if (scenario == 2 || scenario == 3) assert(rc == RUNNER_CLEANUP_FAILED);
        }
    }
    puts("stage lifecycle contract: 15 scenarios PASS (execution intercepted)");
    return 0;
}
