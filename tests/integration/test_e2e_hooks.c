/**
 * @file test_e2e_hooks.c
 * @brief End-to-end test: drive the real ./zyterm binary in --dump mode
 *        against a TCP listener and verify event hooks fire correctly.
 *
 * Exercises the full production path that the unit tests can't reach:
 *   - getopt CLI parsing of --on-match / --on-connect / --on-disconnect
 *   - TCP transport bring-up
 *   - run_dump line-accumulator that splits RX into lines and calls
 *     hooks_on_line()
 *   - fork+exec of /bin/sh -c with stdin redirected to /dev/null
 *   - graceful shutdown firing DISCONNECT hooks before exit
 *
 * @author  Iskandar Putra (www.iskandarputra.com)
 * @copyright Copyright (c) 2026 Iskandar Putra. All rights reserved.
 * @license MIT — see LICENSE for details.
 */
#define _GNU_SOURCE 1
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_pass, g_fail;

#define ASSERT(cond, msg)                                                                 \
    do {                                                                                  \
        if (cond) { g_pass++; fprintf(stderr, "  ok    %s\n", (msg)); }                   \
        else      { g_fail++; fprintf(stderr, "  FAIL  %s (%s:%d)\n",                     \
                                            (msg), __FILE__, __LINE__); }                 \
    } while (0)

#define SECTION(name) fprintf(stderr, "\n=== %s ===\n", (name))

/* Bind to a free TCP port on 127.0.0.1; return listen-fd, set *port. */
static int listen_local(uint16_t *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = {0};
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    sa.sin_port = 0; /* let kernel pick */
    if (bind(s, (struct sockaddr *) &sa, sizeof sa) < 0) { close(s); return -1; }
    socklen_t len = sizeof sa;
    if (getsockname(s, (struct sockaddr *) &sa, &len) < 0) { close(s); return -1; }
    *port = ntohs(sa.sin_port);
    if (listen(s, 1) < 0) { close(s); return -1; }
    return s;
}

/* Fork a child that accepts one connection and feeds it `lines`. */
static pid_t fork_listener(int lfd, const char **lines, int n) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int c = accept(lfd, NULL, NULL);
        if (c < 0) _exit(1);
        for (int i = 0; i < n; i++) {
            ssize_t w = write(c, lines[i], strlen(lines[i]));
            (void) w;
            usleep(150 * 1000);
        }
        usleep(300 * 1000);
        close(c);
        close(lfd);
        _exit(0);
    }
    return pid;
}

/* Wait up to `ms` for `path` to appear. Returns true if it did. */
static bool wait_for_file(const char *path, int ms) {
    for (int i = 0; i < ms / 10; i++) {
        if (access(path, F_OK) == 0) return true;
        usleep(10 * 1000);
    }
    return false;
}

/* Locate ./zyterm relative to repo root. The integration binary runs
 * from tests/build/integration/, so binary lives at ../../../zyterm.
 * Reject paths that are directories (the project folder itself is
 * also called "zyterm"). */
static const char *find_binary(void) {
    static const char *candidates[] = {
        "../../../zyterm",
        "../../zyterm",
        "../zyterm",
        "./zyterm",
        NULL,
    };
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0 && S_ISREG(st.st_mode)
            && (st.st_mode & S_IXUSR)) {
            return candidates[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
static void test_e2e_on_connect_and_disconnect(const char *bin) {
    SECTION("e2e: --on-connect / --on-disconnect");

    char conn[64], disc[64];
    snprintf(conn, sizeof conn, "/tmp/zt_e2e_conn_%d", getpid());
    snprintf(disc, sizeof disc, "/tmp/zt_e2e_disc_%d", getpid());
    unlink(conn); unlink(disc);

    uint16_t port = 0;
    int lfd = listen_local(&port);
    ASSERT(lfd >= 0, "listener bound");
    if (lfd < 0) return;

    const char *lines[] = {"hello\n"};
    pid_t lp = fork_listener(lfd, lines, 1);
    ASSERT(lp > 0, "listener child forked");

    char url[64], conn_act[160], disc_act[160];
    snprintf(url, sizeof url, "tcp://127.0.0.1:%u", port);
    snprintf(conn_act, sizeof conn_act, "touch %s", conn);
    snprintf(disc_act, sizeof disc_act, "touch %s", disc);

    pid_t zp = fork();
    if (zp == 0) {
        /* silence zyterm's noisy stdout/stderr */
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, 1); dup2(dn, 2); close(dn);
        execl(bin, "zyterm",
              "--on-connect",    conn_act,
              "--on-disconnect", disc_act,
              "--dump", "2", url, (char *) NULL);
        _exit(127);
    }
    ASSERT(zp > 0, "zyterm forked");

    /* Wait for both sentinels (3s budget — disc only fires at exit). */
    bool seen_conn = wait_for_file(conn, 3000);
    int  status    = 0;
    waitpid(zp, &status, 0);
    waitpid(lp, NULL, 0);
    bool seen_disc = wait_for_file(disc, 1000);

    ASSERT(seen_conn, "--on-connect sentinel was created");
    ASSERT(seen_disc, "--on-disconnect sentinel was created");

    unlink(conn); unlink(disc);
    close(lfd);
}

/* ------------------------------------------------------------------ */
static void test_e2e_on_match_dump(const char *bin) {
    SECTION("e2e: --on-match in --dump mode");

    char fired[64], envcap[64];
    snprintf(fired, sizeof fired, "/tmp/zt_e2e_match_%d", getpid());
    snprintf(envcap, sizeof envcap, "/tmp/zt_e2e_env_%d", getpid());
    unlink(fired); unlink(envcap);

    uint16_t port = 0;
    int lfd = listen_local(&port);
    ASSERT(lfd >= 0, "listener bound");
    if (lfd < 0) return;

    const char *lines[] = {"boot ok\n", "PANIC bad ptr 0xdead\n", "recovery\n"};
    pid_t lp = fork_listener(lfd, lines, 3);

    char url[64], action[256];
    snprintf(url, sizeof url, "tcp://127.0.0.1:%u", port);
    /* Action exercises BOTH the file sentinel AND env-var capture
     * (the latter regression-tests ZYTERM_LINE / ZYTERM_PORT plumbing). */
    snprintf(action, sizeof action,
             "/PANIC/=touch %s && env | grep -E '^ZYTERM_(LINE|PORT|BAUD)=' > %s",
             fired, envcap);

    pid_t zp = fork();
    if (zp == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, 1); dup2(dn, 2); close(dn);
        execl(bin, "zyterm", "--on-match", action,
              "--dump", "2", url, (char *) NULL);
        _exit(127);
    }

    bool ok = wait_for_file(fired, 3000);
    int  status = 0;
    waitpid(zp, &status, 0);
    waitpid(lp, NULL, 0);
    /* Give the forked /bin/sh another beat to finish the env redir. */
    (void) wait_for_file(envcap, 500);

    ASSERT(ok, "PANIC line fired the --on-match action");

    /* Inspect env capture. */
    FILE *fp = fopen(envcap, "r");
    char buf[2048] = {0};
    if (fp) {
        size_t rd = fread(buf, 1, sizeof buf - 1, fp);
        (void) rd;
        fclose(fp);
    }
    ASSERT(strstr(buf, "ZYTERM_LINE=") != NULL,
           "child saw ZYTERM_LINE in env");
    ASSERT(strstr(buf, "PANIC") != NULL,
           "ZYTERM_LINE contains the matched text");
    ASSERT(strstr(buf, "ZYTERM_PORT=tcp://127.0.0.1:") != NULL,
           "child saw ZYTERM_PORT pointing at the TCP url");

    unlink(fired); unlink(envcap);
    close(lfd);
}

/* ------------------------------------------------------------------ */
static void test_e2e_bad_match_spec_does_not_crash(const char *bin) {
    SECTION("e2e: malformed --on-match spec exits gracefully");

    /* No '/=' delimiter — used to silently log and continue, but
     * we want to make sure the binary doesn't segfault on the bad
     * spec path. */
    pid_t zp = fork();
    if (zp == 0) {
        int dn = open("/dev/null", O_WRONLY);
        dup2(dn, 1); dup2(dn, 2); close(dn);
        execl(bin, "zyterm", "--no-reconnect",
              "--on-match", "garbage-no-delim",
              "--dump", "1", "tcp://127.0.0.1:1", (char *) NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(zp, &status, 0);
    /* Whatever the exit code, we just want NOT a crash signal. */
    ASSERT(!WIFSIGNALED(status) || WTERMSIG(status) == SIGTERM,
           "malformed spec did not crash zyterm");
}

/* ------------------------------------------------------------------ */
int main(void) {
    signal(SIGPIPE, SIG_IGN);

    const char *bin = find_binary();
    if (!bin) {
        fprintf(stderr, "  SKIP  ./zyterm binary not found — build it first\n");
        return 0;
    }
    fprintf(stderr, "  using binary: %s\n", bin);

    test_e2e_on_connect_and_disconnect(bin);
    test_e2e_on_match_dump(bin);
    test_e2e_bad_match_spec_does_not_crash(bin);

    fprintf(stderr, "\n========================================\n");
    fprintf(stderr, "%d passed, %d failed\n", g_pass, g_fail);
    fprintf(stderr, "========================================\n");
    return g_fail == 0 ? 0 : 1;
}
