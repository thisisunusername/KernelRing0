// SPDX-License-Identifier: GPL-2.0
/*
 * kernelring0d.c - KernelRing0 userspace daemon
 *
 * Modes:
 *   kernelring0d                    run daemon (root only)
 *   kernelring0d add <uid|name>     add uid to allowlist
 *   kernelring0d del <uid|name>     remove uid from allowlist
 *   kernelring0d list               list allowlist
 *
 * The daemon holds /dev/kernelring0 open, listens on a Unix socket
 * at /run/kernelring0d.sock (mode 0600), and persists the allowlist
 * to /etc/kernelring0d/allowlist.conf.
 *
 * Any client that is not permitted to connect, or that cannot open
 * the device, receives "command not found" and exits 127, making
 * the tool look nonexistent to unauthorized users.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <pwd.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define DEV_PATH   "/dev/kernelring0"
#define SOCK_PATH  "/run/kernelring0d.sock"
#define CONF_DIR   "/etc/kernelring0d"
#define CONF_PATH  CONF_DIR "/allowlist.conf"

#define KR0_MAGIC  'K'
#define KR0_MAX_AL 64

struct kr0_al_list {
    int count;
    unsigned int uids[KR0_MAX_AL];
};

#define KR0_AL_ADD   _IOW (KR0_MAGIC, 4, unsigned int)
#define KR0_AL_DEL   _IOW (KR0_MAGIC, 5, unsigned int)
#define KR0_AL_LIST  _IOWR(KR0_MAGIC, 6, struct kr0_al_list)

static volatile sig_atomic_t g_running = 1;
static int g_dev_fd = -1;
static int g_srv_fd = -1;

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

/* ---------------- client helpers ---------------- */

static int resolve_uid(const char *s, unsigned int *out)
{
    char *end;
    long v;
    struct passwd *pw;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno == 0 && *end == '\0' && v >= 0) {
        *out = (unsigned int)v;
        return 0;
    }

    pw = getpwnam(s);
    if (!pw)
        return -1;

    *out = pw->pw_uid;
    return 0;
}

static int connect_server(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "kernelring0d: command not found\n");
        exit(127);
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        fprintf(stderr, "kernelring0d: command not found\n");
        exit(127);
    }
    return fd;
}

static int client_cmd(const char *cmd, const char *arg)
{
    int fd, n;
    char buf[1024];
    unsigned int uid = 0;

    if (arg) {
        if (resolve_uid(arg, &uid) < 0) {
            fprintf(stderr, "kernelring0d: unknown user: %s\n", arg);
            return 1;
        }
    }

    fd = connect_server();

    if (arg)
        n = snprintf(buf, sizeof(buf), "%s %u\n", cmd, uid);
    else
        n = snprintf(buf, sizeof(buf), "%s\n", cmd);

    if (write(fd, buf, n) != n) {
        close(fd);
        fprintf(stderr, "kernelring0d: command not found\n");
        return 1;
    }

    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        fprintf(stderr, "kernelring0d: command not found\n");
        return 1;
    }
    buf[n] = '\0';

    if (strncmp(buf, "OK", 2) == 0) {
        return 0;
    } else if (strncmp(buf, "LIST ", 5) == 0) {
        printf("allowlist: %s\n", buf + 5);
        return 0;
    } else if (strncmp(buf, "LIST_EMPTY", 10) == 0) {
        printf("allowlist: (empty)\n");
        return 0;
    } else if (strncmp(buf, "ERR ", 4) == 0) {
        fprintf(stderr, "kernelring0d: %s\n", buf + 4);
        return 1;
    }

    fprintf(stderr, "kernelring0d: unexpected response: %s\n", buf);
    return 1;
}

/* ---------------- daemon helpers ---------------- */

static int dev_open(void)
{
    int fd = open(DEV_PATH, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "kernelring0d: command not found\n");
        exit(127);
    }
    return fd;
}

static int daemon_al_add(unsigned int uid)
{
    return ioctl(g_dev_fd, KR0_AL_ADD, &uid);
}

static int daemon_al_del(unsigned int uid)
{
    return ioctl(g_dev_fd, KR0_AL_DEL, &uid);
}

static int daemon_al_list(struct kr0_al_list *lst)
{
    return ioctl(g_dev_fd, KR0_AL_LIST, lst);
}

static void persist_allowlist(void)
{
    struct kr0_al_list lst;
    FILE *f;
    int i;

    if (daemon_al_list(&lst) < 0)
        return;

    mkdir(CONF_DIR, 0700);

    f = fopen(CONF_PATH, "w");
    if (!f)
        return;

    for (i = 0; i < lst.count; i++)
        fprintf(f, "%u\n", lst.uids[i]);

    fclose(f);
}

static void load_allowlist(void)
{
    FILE *f;
    char line[64];
    struct kr0_al_list cur;
    int i;

    mkdir(CONF_DIR, 0700);

    f = fopen(CONF_PATH, "r");
    if (!f)
        return;

    /* Snapshot current kernel state so we don't double-add. */
    if (daemon_al_list(&cur) < 0)
        cur.count = 0;

    while (fgets(line, sizeof(line), f)) {
        unsigned int uid;
        char *end;
        long v;
        int dup = 0;

        line[strcspn(line, "\r\n")] = '\0';
        if (line[0] == '\0' || line[0] == '#')
            continue;

        errno = 0;
        v = strtol(line, &end, 10);
        if (errno != 0 || *end != '\0' || v < 0)
            continue;
        uid = (unsigned int)v;

        for (i = 0; i < cur.count; i++) {
            if (cur.uids[i] == uid) {
                dup = 1;
                break;
            }
        }
        if (!dup)
            daemon_al_add(uid);
    }

    fclose(f);
}

static void handle_client(int cfd)
{
    char buf[128];
    ssize_t n;

    n = read(cfd, buf, sizeof(buf) - 1);
    if (n <= 0)
        return;
    buf[n] = '\0';
    buf[strcspn(buf, "\r\n")] = '\0';

    if (strncmp(buf, "ADD ", 4) == 0) {
        unsigned int uid = (unsigned int)strtoul(buf + 4, NULL, 10);
        int ret = daemon_al_add(uid);
        if (ret == 0) {
            persist_allowlist();
            write(cfd, "OK\n", 3);
        } else {
            char out[64];
            snprintf(out, sizeof(out), "ERR add failed (%d)\n", ret);
            write(cfd, out, strlen(out));
        }
    } else if (strncmp(buf, "DEL ", 4) == 0) {
        unsigned int uid = (unsigned int)strtoul(buf + 4, NULL, 10);
        int ret = daemon_al_del(uid);
        if (ret == 0) {
            persist_allowlist();
            write(cfd, "OK\n", 3);
        } else {
            char out[64];
            snprintf(out, sizeof(out), "ERR del failed (%d)\n", ret);
            write(cfd, out, strlen(out));
        }
    } else if (strcmp(buf, "LIST") == 0) {
        struct kr0_al_list lst;
        char out[512];
        int i, off = 0;

        if (daemon_al_list(&lst) < 0) {
            write(cfd, "ERR list failed\n", 16);
            return;
        }

        if (lst.count == 0) {
            write(cfd, "LIST_EMPTY\n", 11);
            return;
        }

        off += snprintf(out + off, sizeof(out) - off, "LIST ");
        for (i = 0; i < lst.count; i++)
            off += snprintf(out + off, sizeof(out) - off, "%u%s",
                            lst.uids[i], i + 1 < lst.count ? " " : "\n");
        write(cfd, out, off);
    } else {
        write(cfd, "ERR unknown command\n", 20);
    }
}

static int run_daemon(void)
{
    struct sigaction sa;
    struct sockaddr_un addr;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGHUP,  &sa, NULL);

    g_dev_fd = dev_open();

    /* Restore allowlist from disk, if present. */
    load_allowlist();

    unlink(SOCK_PATH);

    g_srv_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_srv_fd < 0) {
        fprintf(stderr, "kernelring0d: socket() failed: %s\n", strerror(errno));
        close(g_dev_fd);
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(g_srv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "kernelring0d: bind(%s) failed: %s\n",
                SOCK_PATH, strerror(errno));
        close(g_srv_fd);
        close(g_dev_fd);
        return 1;
    }

    chmod(SOCK_PATH, 0600);

    if (listen(g_srv_fd, 8) < 0) {
        fprintf(stderr, "kernelring0d: listen() failed: %s\n", strerror(errno));
        close(g_srv_fd);
        close(g_dev_fd);
        unlink(SOCK_PATH);
        return 1;
    }

    fprintf(stderr, "kernelring0d: started, socket %s (mode 0600)\n", SOCK_PATH);

    while (g_running) {
        int cfd = accept(g_srv_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        handle_client(cfd);
        close(cfd);
    }

    close(g_srv_fd);
    close(g_dev_fd);
    unlink(SOCK_PATH);
    fprintf(stderr, "kernelring0d: stopped\n");
    return 0;
}

/* ---------------- entry ---------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command>\n"
        "Commands:\n"
        "  (no args)          run daemon (must be root)\n"
        "  add <uid|name>     add uid to allowlist\n"
        "  del <uid|name>     remove uid from allowlist\n"
        "  list               show current allowlist\n",
        prog);
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return run_daemon();

    if (geteuid() != 0) {
        /* Daemon management requires root; behave as if missing. */
        fprintf(stderr, "kernelring0d: command not found\n");
        return 127;
    }

    if (argc == 3 && strcmp(argv[1], "add") == 0)
        return client_cmd("ADD", argv[2]);
    if (argc == 3 && strcmp(argv[1], "del") == 0)
        return client_cmd("DEL", argv[2]);
    if (argc == 2 && strcmp(argv[1], "list") == 0)
        return client_cmd("LIST", NULL);

    usage(argv[0]);
    return 1;
}