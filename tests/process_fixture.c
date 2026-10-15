/* SPDX-License-Identifier: Apache-2.0
 * Ordinary idle processes for lifecycle tests. No device operations.
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/prctl.h>

static void record(const char *file, pid_t pid) {
    FILE *f = fopen(file, "w");
    assert(f);
    fprintf(f, "%d\n", (int)pid);
    assert(fclose(f) == 0);
}
int main(int argc, char **argv) {
    assert(argc >= 3);
    if (strcmp(argv[1], "background") == 0 || strcmp(argv[1], "detached") == 0) {
        pid_t child = fork(); assert(child >= 0);
        if (child) return 7;
        if (strcmp(argv[1], "detached") == 0) assert(setsid() >= 0);
    }
    if (argc > 3) assert(prctl(PR_SET_NAME, argv[3]) == 0);
    record(argv[2], getpid());
    for (;;) pause();
}
