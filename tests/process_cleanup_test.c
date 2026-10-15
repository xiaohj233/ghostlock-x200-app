/* SPDX-License-Identifier: Apache-2.0
 * Test only idle fixtures created under the test directory.
 */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
static int signal_fault;
static int test_signal(pid_t pid, int number);
#define kill test_signal
#include "../runner/util.c"
#undef kill
#include "../runner/stages.c"

static int test_signal(pid_t pid, int number) {
    if (signal_fault == 1) { errno = EPERM; return -1; }
    if (signal_fault == 2) return 0;
    return kill(pid, number);
}
static int alive(pid_t pid) {
    ProcIdentity current;
    return proc_identity(pid, &current) == 0 && current.state != 'Z' && current.state != 'X';
}
static pid_t read_pid(const char *file) {
    for (int retry = 0; retry < 200; retry++) {
        FILE *f = fopen(file, "r");
        int pid = 0;
        if (f) { int got = fscanf(f, "%d", &pid); fclose(f); if (got == 1 && pid > 1) return (pid_t)pid; }
        usleep(5000);
    }
    assert(!"fixture did not become ready"); return -1;
}
static pid_t spawn(const char *path, const char *file, const char *name) {
    unlink(file);
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { execl(path, path, "idle", file, name, (char *)NULL); _exit(127); }
    assert(read_pid(file) == pid);
    return pid;
}
static void reap(pid_t pid) {
    int status;
    pid_t result = waitpid(pid, &status, WNOHANG);
    assert(result == pid || (result < 0 && errno == ECHILD));
}
int main(int argc, char **argv) {
    assert(argc == 4);
    const char *path = argv[1], *other = argv[2], *work = argv[3];
    char marker[512]; snprintf(marker, sizeof(marker), "%s/pid", work);
    int cases = 0;
    assert(cleanup_exec(path, 500) == 0); cases++;

    pid_t child = spawn(path, marker, "changed-name");
    assert(cleanup_exec(path, 1000) == 0 && !alive(child)); reap(child); cases++;

    child = spawn(path, marker, "glt_esync");
    char second[512]; snprintf(second, sizeof(second), "%s/other-pid", work);
    pid_t unrelated = spawn(other, second, "glt_esync");
    assert(cleanup_exec(path, 1000) == 0 && !alive(child) && alive(unrelated));
    reap(child); cases++;

    for (int detached = 0; detached < 2; detached++) {
        unlink(marker);
        char *args[] = {(char *)path, detached ? "detached" : "background", marker, "old-name", NULL};
        int code = -1;
        assert(exec_timeout(path, args, NULL, NULL, 3, &code) == 0 && code == 7);
        child = read_pid(marker);
        assert(alive(child)); /* Generic execution must preserve intended background services. */
        assert(cleanup_exec(path, 1000) == 0 && !alive(child)); cases++;
    }

    child = spawn(path, marker, "glt_esync");
    signal_fault = 1;
    assert(cleanup_exec(path, 100) < 0 && alive(child));
    signal_fault = 0;
    assert(cleanup_exec(path, 1000) == 0); reap(child); cases++;

    child = spawn(path, marker, "glt_esync");
    signal_fault = 2;
    assert(cleanup_exec(path, 100) < 0 && errno == EBUSY && alive(child));
    signal_fault = 0;
    assert(cleanup_exec(path, 1000) == 0); reap(child); cases++;

    unlink(marker);
    char *args[] = {(char *)path, "idle", marker, "glt_esync", NULL};
    int code = 0;
    assert(exec_timeout(path, args, NULL, NULL, 1, &code) == 1 && code == -1);
    child = read_pid(marker);
    assert(cleanup_exec(path, 1000) == 0 && !alive(child)); cases++;

    /* A stale PID file must not select the still-alive unrelated fixture. */
    char pidfile[512]; snprintf(pidfile, sizeof(pidfile), "%s/w2host.pid", work);
    FILE *f = fopen(pidfile, "w"); assert(f); fprintf(f, "%d\n", (int)unrelated); fclose(f);
    RunnerCfg cfg = {.work=work, .bindir=work, .tag="test"};
    assert(kill_w2host(&cfg) == 0 && alive(unrelated)); cases++;

    /* Replacing a file must not hide a previous running image. */
    child = spawn(path, marker, "old-name");
    char replacement[512]; snprintf(replacement, sizeof(replacement), "%s/replacement", work);
    FILE *source = fopen(other, "rb"), *dest = fopen(replacement, "wb"); assert(source && dest);
    char bytes[8192]; size_t n;
    while ((n = fread(bytes, 1, sizeof(bytes), source))) assert(fwrite(bytes, 1, n, dest) == n);
    fclose(source); fclose(dest); assert(chmod(replacement, 0755) == 0);
    assert(rename(replacement, path) == 0);
    assert(cleanup_exec(path, 1000) == 0 && !alive(child)); reap(child); cases++;

    assert(cleanup_exec(other, 1000) == 0 && !alive(unrelated)); reap(unrelated);
    assert(cleanup_exec("/nonexistent/managed-tool", 100) < 0); cases++;
    printf("process lifecycle: %d scenarios PASS\n", cases);
    return 0;
}
