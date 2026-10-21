# KernelRing0

**The code was written by Deepseek AI**

**THIS PROJECT IS LINUX ONLY,ANDROID LINUX KERNEL UNSUPPORTED,DO NOT INSMOD**

> A Ring 0 privilege gateway for Linux, implemented as a loadable kernel
> module with a userspace daemon and a CLI tool. Inspired by Android
> KernelSU's LKM mode.

Version: 1.3 · License: GPL-2.0

---

## Table of Contents

1. [Introduction](#1-introduction)
2. [Concept Clarification](#2-concept-clarification)
3. [Features](#3-features)
4. [Architecture](#4-architecture)
5. [Directory Layout](#5-directory-layout)
6. [Requirements](#6-requirements)
7. [Quick Start](#7-quick-start)
8. [Install Targets](#8-install-targets)
9. [Commands](#9-commands)
10. [Allowlist](#10-allowlist)
11. [Security Warnings](#11-security-warnings)
12. [Compatibility](#12-compatibility)
13. [Design Notes](#13-design-notes)
14. [Comparison with KernelSU](#14-comparison-with-kernelsu)
15. [Troubleshooting](#15-troubleshooting)
16. [Uninstall](#16-uninstall)
17. [License](#17-license)
18. [Disclaimer](#18-disclaimer)

---

## 1. Introduction

KernelRing0 is a demonstration-grade kernel privilege tool composed of
three parts:

| Component | Type | Role |
|---|---|---|
| `kernelring0.ko` | Kernel module | Exposes `/dev/kernelring0` and ioctl interfaces |
| `kernelring0d` | Userspace daemon | Holds the device fd, manages the allowlist, persists state |
| `kr0ctl` | CLI tool | Requests escalation, reads credentials, hides the module |

The project answers a specific question: **does Linux have an equivalent
of KernelSU's "gain Ring 0 capability via a kernel module" approach?**
KernelRing0 is the minimal viable answer to that question on generic
Linux.

---

## 2. Concept Clarification

1. **The Linux kernel always runs in Ring 0**. Userspace processes always
   run in Ring 3 on x86, or EL0 on ARM (kernel in EL1). There is no
   mechanism to keep a userspace process in Ring 0 permanently.
2. **Plain root is already very powerful**, but root is still constrained
   by kernel LSM (SELinux / AppArmor), capability boundaries, and
   `no_new_privs`. A kernel module can bypass these because module code
   itself executes in kernel mode.
3. **What KernelRing0 actually does**: after the module is loaded, a
   userspace tool issues an ioctl asking the kernel module to perform a
   privileged operation on its behalf (modify cred, hide the module,
   etc.). The effect is equivalent to temporarily possessing Ring 0
   capability.
4. **The device is created with mode 0666**, but access control is
   enforced inside the ioctl handler via the allowlist. Non-allowlisted
   callers receive `-ENOENT` and the tool appears nonexistent.
5. **KernelRing0 is not a "one-click root for unprivileged users" tool.**
   It is a "root user gains the ability to bypass userspace security
   mechanisms via a kernel module" tool. The allowlist is the mechanism
   that decides who may use it.

---

## 3. Features

- **Standard escalation path**: `prepare_creds()` / `commit_creds()`, no
  `sys_call_table` hooking, no CR0 tricks.
- **Module hiding**: `KR0_HIDE` removes the module from
  `THIS_MODULE->list`, so `lsmod` no longer shows it.
- **Allowlist**: KernelSU-style UID allowlist managed by the daemon.
  Non-allowlisted callers get `command not found` and exit 127.
- **Daemon-managed persistence**: allowlist stored in
  `/etc/kernelring0d/allowlist.conf`.
- **Unix socket management API**: daemon listens on
  `/run/kernelring0d.sock` (mode 0600) for `add` / `del` / `list`.
- **One-shot temporary deploy**: `make install` loads the module, starts
  the daemon in the background, and drops into a root shell.
- **Persistent deploy**: `make install-keep` installs binaries, the
  kernel module, the systemd unit, and enables autostart on boot.
- **Auto-detecting Makefile**: Debian/Ubuntu, Fedora/RHEL/CentOS, Arch,
  openSUSE, Alpine.
- **All source comments in English**.

---

## 4. Architecture

    +---------------------------------------------+
    |  Userspace (Ring 3)                         |
    |                                             |
    |   +----------+        +------------------+  |
    |   |  kr0ctl  |        |  kernelring0d    |  |
    |   | CLI tool |        |  daemon          |  |
    |   +----+-----+        +--------+---------+  |
    |        |                       |            |
    |        |      ioctl            | open/fd    |
    |        |                       |            |
    |        |              +--------v---------+  |
    |        |              | Unix socket      |  |
    |        |              | /run/...sock     |  |
    |        |              +------------------+  |
    +--------+------------------------------------+
             |
    +--------v------------------------------------+
    |  Kernel (Ring 0)                            |
    |                                             |
    |   /dev/kernelring0  (misc device, 0666)     |
    |     +- KR0_ESCALATE   escalate caller       |
    |     +- KR0_READ_CRED  read caller creds     |
    |     +- KR0_HIDE       hide module           |
    |     +- KR0_AL_ADD     allowlist add (root)  |
    |     +- KR0_AL_DEL     allowlist del (root)  |
    |     +- KR0_AL_LIST    allowlist list (root) |
    |                                             |
    |   prepare_creds() / commit_creds()          |
    +---------------------------------------------+

Communication uses **misc device + ioctl** rather than netlink because:

- ioctl operates on the **caller process** (`current`) directly, no PID
  passing, no PID-reuse races.
- Misc device auto-creates a `/dev` node with controllable permissions.
- Simpler than netlink, smaller attack surface.

---

## 5. Directory Layout

    kernelring0/
    +-- kernelring0.c          # kernel module source
    +-- kernelring0d.c         # userspace daemon source
    +-- kr0ctl.c               # CLI control tool source
    +-- kernelring0d.service   # systemd unit file
    +-- Makefile               # build / install / uninstall automation
    +-- LICENSE                # GPL-2.0 full text
    +-- README.md              # this file

---

## 6. Requirements

### Mandatory

- **C compiler**: `gcc` or `clang`
- **GNU Make**
- **Kernel headers**: version must match `uname -r` exactly
  - Debian/Ubuntu: `linux-headers-$(uname -r)`
  - RHEL/CentOS/Fedora: `kernel-devel-$(uname -r)`
- **libelf**: kernels 4.15+ default to `CONFIG_UNWINDER_ORC=y`, which
  requires libelf to link `.ko` files
  - Debian/Ubuntu: `libelf-dev`
  - RHEL/Fedora: `elfutils-libelf-devel`

### Optional

- `libssl-dev` / `openssl-devel` (if kernel enables module signing)
- `flex`, `bison`, `libncurses-dev` (kernel helper tools)

### One-shot dependency install

    make deps

The Makefile detects the distribution via `/etc/os-release` and the
package manager via `command -v`. Supported: `apt`, `dnf`, `yum`,
`pacman`, `zypper`, `apk`.

---

## 7. Quick Start

### Build only

    cd kernelring0/
    make deps
    make
    make info

### Temporary deploy (one-shot, disappears after reboot)

    sudo -i
    cd /path/to/kernelring0
    make install

This target:

1. Requires real root (`id -u == 0`)
2. Builds the module and userspace tools
3. Runs `insmod ./kernelring0.ko`
4. Starts `./kernelring0d` in the background
   (PID at `/run/kernelring0d.pid`, log at `/var/log/kernelring0d.log`)
5. Launches `./kr0ctl root` and drops you into a root shell

Exiting the shell completes the install. Nothing survives a reboot.

### Persistent deploy

    sudo -i
    cd /path/to/kernelring0
    make install-keep

Additionally:

- Installs `kr0ctl` to `/usr/local/bin/`
- Installs `kernelring0d` to `/usr/local/sbin/`
- Installs `kernelring0.ko` to `/lib/modules/$(uname -r)/extra/`
- Installs the systemd unit to `/etc/systemd/system/`
- Creates `/etc/kernelring0d/` (mode 0700) and `allowlist.conf` (0600)
- Registers module in `/etc/modules-load.d/kernelring0.conf`
- Runs `depmod -a` and `systemctl daemon-reload`
- Enables and starts `kernelring0d`
- Launches `/usr/local/bin/kr0ctl root`

After reboot the module is auto-loaded by `systemd-modules-load` and the
daemon is auto-started by systemd.

### Authorize a user

    sudo kernelring0d add alice
    sudo kernelring0d add 1000
    sudo kernelring0d list

---

## 8. Install Targets

| Step | `make install` (temporary) | `make install-keep` (persistent) |
|---|---|---|
| Requires real root | yes | yes |
| Build | yes | yes |
| Install binaries to `/usr/local/{bin,sbin}` | no | yes |
| Install module to `/lib/modules/.../extra` | no | yes |
| Install systemd unit | no | yes |
| Register boot-time module autoload | no | yes |
| Load module | yes (`insmod`) | yes (via systemd) |
| Start daemon | yes (`nohup`) | yes (`systemctl enable --now`) |
| Survives reboot | no | yes |
| Final action | `./kr0ctl root` | `/usr/local/bin/kr0ctl root` |

### Why `install` checks for "real root"

`sudo make install` runs make as root and `id -u` returns 0, so the check
passes. But `sudo` clears environment variables (`PATH`, `HOME`, etc.)
which can break `nohup`, `setsid`, `insmod` path resolution, and
`systemctl` inside containers. `_root_check` therefore only verifies
`id -u == 0` and recommends `sudo -i`. To strictly forbid `sudo make`,
replace `_root_check` with:

    _root_check:
        @if [ "$$(id -u)" -ne 0 ] || [ -n "$${SUDO_USER:-}" ]; then \
            echo "ERROR: run as real root (sudo -i), not via sudo make"; \
            exit 1; \
        fi

`SUDO_USER` being non-empty indicates the process was escalated via
`sudo`, so this rejects `sudo make install` outright.

---

## 9. Commands

### `kr0ctl`

    Usage: kr0ctl <command>

    Commands:
      status   print current process credentials via the module
      root     escalate this process to uid=0 and exec a shell
      hide     remove the module from lsmod (ring0 hide)

Examples:

    $ sudo kr0ctl status
    pid=1234 tgid=1234 uid=0 euid=0 gid=0 egid=0 comm=kr0ctl

    $ sudo kr0ctl root
    kr0ctl: escalated, spawning root shell...
    # id
    uid=0(root) gid=0(root) groups=0(root)

    $ sudo kr0ctl hide
    kr0ctl: hide requested, check 'lsmod | grep kernelring0'
    $ lsmod | grep kernelring0
    $   # empty, module is hidden

If the caller is not authorized, or the device does not exist, every
command prints `kr0ctl: command not found` and exits 127.

### `kernelring0d`

    Usage: kernelring0d <command>

    Commands:
      (no args)          run daemon (must be root)
      add <uid|name>     add uid to allowlist
      del <uid|name>     remove uid from allowlist
      list               show current allowlist

Examples:

    sudo kernelring0d add 1000
    sudo kernelring0d add alice
    sudo kernelring0d del alice
    sudo kernelring0d list

Non-root users attempting any management command get
`kernelring0d: command not found` and exit 127.

---

## 10. Allowlist

### Semantics

- **uid 0 (root) is always allowed**, no need to add it explicitly.
- **Allowlisted uids** may invoke `kr0ctl` privileged commands.
- **Non-allowlisted uids** receive `command not found` and exit 127 from
  both `kr0ctl` and `kernelring0d`.
- **Allowlist management** (`kernelring0d add/del/list`) requires root
  (`id -u == 0`).

### Management

    sudo kernelring0d add 1000      # add uid 1000
    sudo kernelring0d add alice     # add user alice
    sudo kernelring0d del alice     # remove user alice
    sudo kernelring0d list          # list current allowlist

### Persistence

Daemon writes allowlist to `/etc/kernelring0d/allowlist.conf` (one uid
per line, `#` starts a comment). Loaded automatically at daemon startup.
Directory mode 0700, file mode 0600.

### Device permission model

`/dev/kernelring0` is mode **0666** so allowlisted non-root users can
open it. Access control is enforced entirely inside the ioctl handler:

- Non-allowlisted uid: all privileged ioctls return `-ENOENT`.
- Non-root uid: management ioctls also return `-ENOENT`.
- Root uid: everything allowed.

This mirrors KernelSU's "hide yourself via kernel-side checks" approach.
File permissions themselves do not carry access control responsibility.

---

## 11. Security Warnings

**KernelRing0 is a demonstration / research project, not a production
tool.**

1. **`kr0ctl root` escalates the current process and execs a root shell.**
   Misuse may damage the system.
2. **`kr0ctl hide` breaks unload semantics.** `rmmod` will fail after
   hiding. Recovery requires a **reboot**.
3. **Module hiding is not thorough.** `lsmod` is fooled, but
   `/sys/module/kernelring0/` and `/proc/kallsyms` entries remain.
4. **LSM is not bypassed.** SELinux / AppArmor still apply after
   escalation.
5. **Do not use on production systems**, especially with Secure Boot and
   forced module signing.
6. **Legal compliance**: using this tool on systems you do not own or
   are not authorized to administer may violate local law and platform
   terms.

---

## 12. Compatibility

### Kernel versions

| Kernel | Status | Notes |
|---|---|---|
| 5.x / 6.x | Recommended | Most thoroughly tested |
| 4.14+ | Supported | Requires libelf |
| 4.4 - 4.13 | Untested | May need adjustments |
| < 4.4 | Unsupported | `prepare_creds` semantics differ |

### Key API stability

- `misc_register()` / `misc_deregister()`: stable since 2.6
- `prepare_creds()` / `commit_creds()`: stable since 2.6.28
- `from_kuid()` / `from_kgid()`: handle `uid_t` -> `kuid_t` transition
- `cap_set_full()` / `cap_clear()`: ancient, maximally portable
- `struct module->list`: exists in 4.x - 6.x

### Distributions

| Distro | Package manager | Status |
|---|---|---|
| Debian / Ubuntu | apt | Supported |
| Fedora / RHEL / CentOS | dnf / yum | Supported |
| Arch Linux | pacman | Supported |
| openSUSE | zypper | Supported |
| Alpine | apk | Supported |

### Architectures

x86_64 and aarch64 both build and run. The kernel module is
architecture-independent as long as the target kernel exports
`prepare_creds` and `commit_creds`.

---

## 13. Design Notes

### Why `prepare_creds` / `commit_creds`?

Kernel's canonical credential modification interface:

1. `prepare_creds()` copies the current cred into a new `struct cred`
2. Modify the new cred's uid/gid/capability
3. `commit_creds()` atomically replaces `current->cred`

Advantages:

- No CR0 write-protect bit modification
- No `sys_call_table` hooking
- RCU and refcounting handled automatically
- Compatible with 4.x - 6.x

Directly writing `current->cred` is RCU-protected in newer kernels and
would trigger `BUG_ON` or memory corruption.

### Why does ioctl escalate the caller?

`KR0_ESCALATE` targets `current`, not `find_task_by_vpid(pid)`:

- No PID passed from userspace
- No PID-reuse race
- Clear semantics: "I request escalation for myself"
- After escalation, `kr0ctl` calls `execl("/bin/sh")` so the shell
  inherits root

### Why is the device mode 0666?

Allowlisted non-root users must be able to open it. Access control is
enforced inside the ioctl handler via the allowlist. A non-allowlisted
user can open the device, but every ioctl returns `-ENOENT`, so the
tool behaves as if it does not exist.

### Limitations of module hiding

`kr0_hide_module()` only does `list_del_init(&THIS_MODULE->list)`:

- `lsmod | grep kernelring0` shows nothing
- `/sys/module/kernelring0/` still exists
- Symbols in `/proc/kallsyms` still exist
- `rmmod` fails

Full hiding requires hooking `kallsyms`, `sysfs`, and more - rootkit
territory, out of scope.

---

## 14. Comparison with KernelSU

| KernelSU LKM | KernelRing0 |
|---|---|
| `kernelsu.ko` loaded via `insmod` | `kernelring0.ko` loaded via `insmod` |
| Anonymous inode + `prctl` supercall | Misc device + ioctl |
| `ksud` Rust daemon | `kernelring0d` C daemon |
| Kernel `execve` hook redirects `su` | `kr0ctl root` self-escalates then exec shell |
| Allowlist / SELinux policy management | Allowlist via `kernelring0d add/del/list` |
| Module hiding (full hook) | `list_del_init` only |
| Android GKI 5.10+ | Generic Linux 4.14+ |

KernelRing0 is a **minimal demonstration** of KernelSU's LKM idea on
generic Linux. To approach KernelSU's full capabilities, add a kprobe
hook on `execve` and full module hiding.

---

## 15. Troubleshooting

### `make deps` reports "no supported package manager detected"

Your distro is unsupported. Manually install: `gcc`, `make`, kernel
headers matching `uname -r`, and `libelf-dev` / `elfutils-libelf-devel`.
Then run `make`.

### `make` reports "kernel headers not found at /lib/modules/.../build"

    uname -r
    ls /lib/modules/$(uname -r)/build

If `build` is a dangling symlink, reinstall matching headers.

### `insmod` reports "Invalid module format"

- Headers mismatch running kernel
- GCC version differs too much from kernel build
- Module signing enforced (`CONFIG_MODULE_SIG_FORCE=y`)

Check `dmesg | tail -n 20`.

### `insmod` reports "Operation not permitted"

- Secure Boot + unsigned module
- `CONFIG_MODULE_SIG_FORCE=y`
- Container without `CAP_SYS_MODULE`

### `make install` reports "this target must be run as real root"

Run `sudo -i` first, then `make install`.

### `kr0ctl: command not found`

Either the module is not loaded, `/dev/kernelring0` does not exist, or
your uid is not in the allowlist. Check:

    lsmod | grep kernelring0
    ls -l /dev/kernelring0
    sudo kernelring0d list

### `kernelring0d: command not found`

You are not root. Management commands require `id -u == 0`.

### `rmmod` reports "Module kernelring0 is not currently loaded"

You ran `kr0ctl hide`. **Reboot to recover.**

### `kr0ctl root` escalates but some operations still denied

`KR0_ESCALATE` only modifies cred. It does not change:

- LSM state (SELinux / AppArmor still apply)
- Namespaces (container escalation is still bounded)
- `no_new_privs` flag

### Daemon fails to start in persistent mode

    systemctl status kernelring0d --no-pager
    journalctl -u kernelring0d -n 50 --no-pager
    dmesg | tail -n 20

Common causes: module not loaded, `/dev/kernelring0` missing, or
permission error on `/run/kernelring0d.sock`.

---

## 16. Uninstall

### Persistent uninstall

    sudo -i
    cd /path/to/kernelring0
    make uninstall

This stops and disables `kernelring0d`, removes `kr0ctl`,
`kernelring0d`, `kernelring0.ko`, the systemd unit, the
`modules-load.d` entry, PID file and socket, then unloads the module.
`depmod -a` and `systemctl daemon-reload` are run.

**`/etc/kernelring0d/allowlist.conf` is preserved.** Delete it manually
for a clean wipe.

### Temporary unload

    sudo make unload

Or:

    sudo rmmod kernelring0

If you previously ran `kr0ctl hide`, `rmmod` will fail - reboot to
recover.

---

## 17. License

Kernel module: **GPL-2.0** (`MODULE_LICENSE("GPL")`), because it uses
GPL-only symbols such as `prepare_creds` and `commit_creds`.

Userspace tools (`kr0ctl.c`, `kernelring0d.c`) and build scripts:
**GPL-2.0** for consistency. See the `LICENSE` file for the full text.

---

## 18. Disclaimer

This project is intended for **kernel module development learning, Linux
privilege model research, and security education**. Do not use it on:

- Systems you do not own or are not authorized to administer
- Production environments
- Any use that violates local law or platform terms

The authors accept no liability for any direct or indirect damage caused
by using this project. By using it, you acknowledge that you have read
the security warnings and accept full responsibility for all risks.
