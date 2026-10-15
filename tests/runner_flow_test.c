/* SPDX-License-Identifier: Apache-2.0
 * Exercise the real coordinator with every device stage replaced by a stub.
 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include "../runner/runner.h"

static int scenario, calls[10], starts[16], count;
int fixture_entry(int argc, char **argv);
uid_t fixture_uid(void) { return scenario == 5 ? 0 : 2000; }
int fixture_enforce(void) { return 0; }
int fixture_seccomp(void) { return 0; }
int fixture_gate(const RunnerCfg *cfg) { (void)cfg; calls[0]++; return 0; }
int fixture_one(const RunnerCfg *cfg) { (void)cfg; calls[1]++; return scenario == 3 ? RUNNER_CLEANUP_FAILED : 0; }
int fixture_two(const RunnerCfg *cfg) { (void)cfg; calls[2]++; return 0; }
int fixture_three(const RunnerCfg *cfg, SymTab *symbols) { (void)cfg; (void)symbols; calls[3]++; return 0; }
int fixture_wait(const RunnerCfg *cfg) { (void)cfg; calls[4]++; return 0; }
int fixture_worker(const RunnerCfg *cfg, char *buffer, size_t size) {
    (void)cfg; (void)buffer; (void)size; calls[5]++;
    if (scenario == 1) return RUNNER_CLEANUP_FAILED;
    return scenario == 4 && calls[5] == 1 ? -1 : 0;
}
int fixture_five(const RunnerCfg *cfg, const SymTab *symbols, const char *candidates) {
    (void)cfg; (void)symbols; (void)candidates; calls[6]++;
    return scenario == 2 ? RUNNER_CLEANUP_FAILED : 0;
}
int fixture_six(const RunnerCfg *cfg, const SymTab *symbols) { (void)cfg; (void)symbols; calls[7]++; return 0; }
int fixture_seven(const RunnerCfg *cfg) { (void)cfg; calls[8]++; return 0; }
int fixture_existing(const RunnerCfg *cfg) { (void)cfg; calls[9]++; return 0; }
void fixture_event(const RunnerCfg *cfg, const char *event, int result) {
    (void)cfg; (void)result;
    int index;
    if (strstr(event, "/start") && sscanf(event, "stage-%d/start", &index) == 1) starts[count++] = index;
}
int main(void) {
    char *args[] = {"coordinator-test", "work/tests", "offline", NULL};
    for (scenario = 0; scenario < 6; scenario++) {
        memset(calls, 0, sizeof(calls)); count = 0;
        int result = fixture_entry(3, args);
        assert((result == 0) == (scenario == 0 || scenario == 4 || scenario == 5));
        if (scenario == 0 || scenario == 4) {
            assert(count == 9);
            for (int i = 0; i < 9; i++) assert(starts[i] == i);
        }
        if (scenario == 1) assert(calls[5] == 1 && calls[6] == 0);
        if (scenario == 2) assert(calls[6] == 1 && calls[7] == 0);
        if (scenario == 3) assert(calls[1] == 1 && calls[2] == 0);
        if (scenario == 4) assert(calls[5] == 2 && calls[6] == 1);
        if (scenario == 5) assert(calls[9] == 1 && calls[1] == 0 && calls[5] == 0);
    }
    puts("coordinator flow: 6 scenarios PASS (all device stages intercepted)");
    return 0;
}
