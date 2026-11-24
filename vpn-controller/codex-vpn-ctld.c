/*
 * codex-vpn-ctld.c - Minimal allowlist Unix-domain socket controller & client
 * for Samsung CU8000 (.122) Lightway VPN integration with Living Room Hub.
 *
 * Security invariants:
 * - Zero HTTP listeners; Unix domain socket /run/codex-vpn/ctl.sock only.
 * - Strict SO_PEERCRED enforcement: ONLY uid 0 (root) or uid 5001 (Living Room Hub).
 * - Strict 7-verb allowlist; zero shell interpolation; execv() to fixed ctl.sh.
 * - Zero credential access or credential output.
 *
 * Build (static ARMv7 soft-float EABI5):
 *   zig cc -target arm-linux-musleabi -mfloat-abi=softfp -Os -static \
 *          -Wall -Wextra -o tools/vpn_tv/codex-vpn-ctld tools/vpn_tv/codex-vpn-ctld.c
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define RUN_DIR    "/run/codex-vpn"
#define SOCK_PATH  "/run/codex-vpn/ctl.sock"
#define CTL_SCRIPT "/opt/usr/etc/codex-vpn/ctl.sh"
#define ALLOWED_UI_UID 5001u
#define MAX_REQ    128
#define MAX_RESP   16384

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static bool is_valid_loc_id(const char *s) {
    size_t n = strlen(s);
    if (n < 1 || n > 6) return false;
    for (size_t i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

static void write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)n;
    }
}

static int run_ctl(const char *verb, const char *arg, char *out, size_t out_cap) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        close(pipefd[0]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            close(devnull);
        }
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char *argv[4];
        argv[0] = (char *)CTL_SCRIPT;
        argv[1] = (char *)verb;
        if (arg && arg[0]) {
            argv[2] = (char *)arg;
            argv[3] = NULL;
        } else {
            argv[2] = NULL;
        }
        execv(CTL_SCRIPT, argv);
        _exit(127);
    }

    close(pipefd[1]);
    size_t used = 0;
    while (used + 1 < out_cap) {
        ssize_t n = read(pipefd[0], out + used, out_cap - 1 - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        used += (size_t)n;
    }
    out[used] = '\0';
    close(pipefd[0]);

    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    if (WIFEXITED(st)) return WEXITSTATUS(st);
    return 1;
}

static void handle_client(int cfd) {
    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 10;
    setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct ucred cred;
    socklen_t clen = sizeof(cred);
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &clen) != 0 ||
        clen != sizeof(cred) ||
        (cred.uid != 0u && cred.uid != ALLOWED_UI_UID)) {
        const char *deny = "{\"ok\":false,\"error\":\"peer_uid_denied\"}\n";
        write_all(cfd, deny, strlen(deny));
        return;
    }

    char req[MAX_REQ];
    size_t rlen = 0;
    while (rlen + 1 < sizeof(req)) {
        ssize_t n = read(cfd, req + rlen, sizeof(req) - 1 - rlen);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        rlen += (size_t)n;
        if (memchr(req, '\n', rlen) != NULL) break;
    }
    req[rlen] = '\0';
    while (rlen > 0 && (req[rlen - 1] == '\n' || req[rlen - 1] == '\r' || req[rlen - 1] == ' ')) {
        req[--rlen] = '\0';
    }

    const char *ctl_verb = NULL;
    char arg[16] = {0};

    if (strcmp(req, "STATUS") == 0) {
        ctl_verb = "status";
    } else if (strcmp(req, "TOGGLE") == 0) {
        ctl_verb = "toggle";
    } else if (strcmp(req, "DISCONNECT") == 0) {
        ctl_verb = "disconnect";
    } else if (strcmp(req, "SELECT_NEXT") == 0) {
        ctl_verb = "select-next";
    } else if (strncmp(req, "SELECT ", 7) == 0 && is_valid_loc_id(req + 7)) {
        ctl_verb = "select";
        snprintf(arg, sizeof(arg), "%s", req + 7);
    } else if (strncmp(req, "CONNECT ", 8) == 0 && is_valid_loc_id(req + 8)) {
        ctl_verb = "connect";
        snprintf(arg, sizeof(arg), "%s", req + 8);
    } else if (strncmp(req, "SWITCH ", 7) == 0 && is_valid_loc_id(req + 7)) {
        ctl_verb = "switch";
        snprintf(arg, sizeof(arg), "%s", req + 7);
    } else {
        const char *bad = "{\"ok\":false,\"error\":\"forbidden_verb\"}\n";
        write_all(cfd, bad, strlen(bad));
        return;
    }

    char resp[MAX_RESP];
    int rc = run_ctl(ctl_verb, arg[0] ? arg : NULL, resp, sizeof(resp));
    if (resp[0] == '\0') {
        snprintf(resp, sizeof(resp), "{\"ok\":%s,\"rc\":%d}\n", rc == 0 ? "true" : "false", rc);
    }
    write_all(cfd, resp, strlen(resp));
}

static int run_daemon(int group_override) {
    if (geteuid() != 0) {
        fprintf(stderr, "codex-vpn-ctld: daemon requires root\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    mkdir(RUN_DIR, 0755);
    chmod(RUN_DIR, 0755);
    unlink(SOCK_PATH);

    int sfd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sfd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);

    umask(0117);
    if (bind(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        close(sfd);
        return 1;
    }

    gid_t sock_gid = 100; /* users */
    if (group_override >= 0) {
        sock_gid = (gid_t)group_override;
    } else {
        struct group *gr = getgrnam("users");
        if (gr != NULL) sock_gid = gr->gr_gid;
    }
    (void)chown(SOCK_PATH, 0, sock_gid);
    chmod(SOCK_PATH, 0660);

    if (listen(sfd, 8) != 0) {
        perror("listen");
        unlink(SOCK_PATH);
        close(sfd);
        return 1;
    }

    while (!g_stop) {
        int cfd = accept4(sfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;
        }
        handle_client(cfd);
        close(cfd);
    }

    unlink(SOCK_PATH);
    close(sfd);
    return 0;
}

static int run_send(const char *cmd, int timeout_sec) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"socket_create_failed\"}\n");
        return 1;
    }

    struct timeval tv;
    tv.tv_sec = timeout_sec > 0 ? timeout_sec : 90;
    tv.tv_usec = 0;
    setsockopt(sfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    tv.tv_sec = 5;
    setsockopt(sfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", SOCK_PATH);

    if (connect(sfd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"ctl_socket_unavailable\",\"errno\":%d}\n", errno);
        close(sfd);
        return 1;
    }

    char line[MAX_REQ + 2];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    if (n <= 0 || (size_t)n >= sizeof(line)) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"command_too_long\"}\n");
        close(sfd);
        return 1;
    }
    write_all(sfd, line, (size_t)n);
    shutdown(sfd, SHUT_WR);

    char resp[MAX_RESP];
    size_t used = 0;
    while (used + 1 < sizeof(resp)) {
        ssize_t r = read(sfd, resp + used, sizeof(resp) - 1 - used);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        used += (size_t)r;
    }
    resp[used] = '\0';
    close(sfd);

    if (used == 0) {
        fprintf(stdout, "{\"ok\":false,\"error\":\"empty_response\"}\n");
        return 1;
    }
    fwrite(resp, 1, used, stdout);
    if (resp[used - 1] != '\n') fputc('\n', stdout);
    return (strstr(resp, "\"ok\":true") != NULL) ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "--help") == 0) {
        fprintf(stdout, "Usage: codex-vpn-ctld --daemon [--gid GID] | --send CMD [--timeout SEC]\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "--daemon") == 0) {
        int gid = -1;
        if (argc == 4 && strcmp(argv[2], "--gid") == 0) gid = atoi(argv[3]);
        return run_daemon(gid);
    }
    if (argc >= 3 && strcmp(argv[1], "--send") == 0) {
        int timeout_sec = 90;
        if (argc == 5 && strcmp(argv[3], "--timeout") == 0) timeout_sec = atoi(argv[4]);
        return run_send(argv[2], timeout_sec);
    }
    fprintf(stderr, "Usage: codex-vpn-ctld --daemon [--gid GID] | --send CMD [--timeout SEC]\n");
    return 64;
}
