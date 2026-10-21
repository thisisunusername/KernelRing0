# ============================================================
# KernelRing0 Makefile
#
# Targets:
#   make deps            install build dependencies
#   make                 build module + userspace tools
#   make install         TEMPORARY deploy (must be run as real root):
#                          - insmod kernelring0.ko
#                          - start kernelring0d in background
#                          - launch `kr0ctl root` (drops into root shell)
#   make install-keep    PERSISTENT deploy (must be run as real root):
#                          - install binaries to $(PREFIX)
#                          - install kernel module to $(KOBJDIR)
#                          - install systemd unit + enable autostart
#                          - install /etc/modules-load.d/kernelring0.conf
#                          - enable + start kernelring0d
#                          - launch `kr0ctl root`
#   make load            load the kernel module only
#   make unload          unload the kernel module
#   make uninstall       persistent removal
#   make clean           remove build artifacts
#   make info            print detected system information
#   make help            show this help
# ============================================================

NAME_MODULE := kernelring0
NAME_DAEMON := kernelring0d
NAME_CTL    := kr0ctl
NAME_UNIT   := kernelring0d.service
NAME_MODCONF := $(NAME_MODULE).conf

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)
obj-m += kernelring0.o
CC    ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

SUDO  ?= $(shell if [ "$$(id -u)" -ne 0 ]; then echo sudo; fi)

KVER  := $(shell uname -r)
OS_ID := $(shell if [ -f /etc/os-release ]; then . /etc/os-release && echo "$$ID"; else echo unknown; fi)

PKG_MGR := $(shell \
    if command -v apt-get >/dev/null 2>&1; then echo apt; \
    elif command -v dnf >/dev/null 2>&1; then echo dnf; \
    elif command -v yum >/dev/null 2>&1; then echo yum; \
    elif command -v pacman >/dev/null 2>&1; then echo pacman; \
    elif command -v zypper >/dev/null 2>&1; then echo zypper; \
    elif command -v apk >/dev/null 2>&1; then echo apk; \
    else echo unknown; fi)

PREFIX      ?= /usr/local
BINDIR      ?= $(PREFIX)/bin
SBINDIR     ?= $(PREFIX)/sbin
UNITDIR     ?= /etc/systemd/system
KOBJDIR     ?= /lib/modules/$(KVER)/extra
CONFDIR     ?= /etc/kernelring0d
MODLOADDIR  ?= /etc/modules-load.d
RUNDIR      ?= /run
LOGDIR      ?= /var/log

DAEMON_PID  := $(RUNDIR)/$(NAME_DAEMON).pid
DAEMON_SOCK := $(RUNDIR)/$(NAME_DAEMON).sock
DAEMON_LOG  := $(LOGDIR)/$(NAME_DAEMON).log

.PHONY: all module userspace deps install install-keep install-all \
        uninstall load unload clean info help _root_check _stop_daemon

all: module userspace

# ---------- kernel module ----------
module:
	@echo "[*] Building kernel module against $(KDIR)"
	@test -d "$(KDIR)" || { \
	    echo "ERROR: kernel headers not found at $(KDIR)"; \
	    echo "       run 'make deps' first"; \
	    exit 1; }
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ---------- userspace ----------
userspace: $(NAME_CTL) $(NAME_DAEMON)

$(NAME_CTL): $(NAME_CTL).c
	$(CC) $(CFLAGS) -o $@ $<

$(NAME_DAEMON): $(NAME_DAEMON).c
	$(CC) $(CFLAGS) -o $@ $<

# ---------- dependency installation ----------
deps:
	@echo "[*] distro=$(OS_ID) pkg-mgr=$(PKG_MGR) kernel=$(KVER)"
	@case "$(PKG_MGR)" in \
	apt) \
	    $(SUDO) apt-get update; \
	    $(SUDO) apt-get install -y \
	        build-essential \
	        linux-headers-$(KVER) \
	        libelf-dev libssl-dev flex bison libncurses-dev ;; \
	dnf) \
	    $(SUDO) dnf install -y \
	        gcc make kernel-devel-$(KVER) \
	        elfutils-libelf-devel openssl-devel \
	        flex bison ncurses-devel ;; \
	yum) \
	    $(SUDO) yum install -y \
	        gcc make kernel-devel-$(KVER) \
	        elfutils-libelf-devel openssl-devel \
	        flex bison ncurses-devel ;; \
	pacman) \
	    $(SUDO) pacman -Sy --needed --noconfirm \
	        base-devel linux-headers ;; \
	zypper) \
	    $(SUDO) zypper install -y \
	        gcc make kernel-devel \
	        libelf-devel libopenssl-devel \
	        flex bison ncurses-devel ;; \
	apk) \
	    $(SUDO) apk add --no-cache \
	        build-base linux-headers \
	        libelf-dev openssl-dev \
	        flex bison ncurses-dev ;; \
	*) \
	    echo "ERROR: no supported package manager detected."; \
	    echo "       install gcc, make, kernel headers and libelf manually."; \
	    exit 1 ;; \
	esac

# ---------- root check helper ----------
_root_check:
	@if [ "$$(id -u)" -ne 0 ]; then \
	    echo "ERROR: this target must be run as real root, not via sudo."; \
	    echo "       try:  sudo -i"; \
	    echo "             cd $(PWD)"; \
	    echo "             make $${MAKECMDGOALS:-install}"; \
	    exit 1; \
	fi

# ---------- stop any running daemon ----------
_stop_daemon:
	@echo "[*] Stopping $(NAME_DAEMON) (if any)"
	@-systemctl stop $(NAME_DAEMON) 2>/dev/null || true
	@-if [ -f $(DAEMON_PID) ]; then \
	    kill $$(cat $(DAEMON_PID)) 2>/dev/null || true; \
	    rm -f $(DAEMON_PID); \
	fi
	@-pkill -x $(NAME_DAEMON) 2>/dev/null || true
	@sleep 0.3

# ---------- temporary deploy ----------
install: _root_check all _stop_daemon
	@echo "[*] Loading kernel module"
	@if lsmod | grep -q "^$(NAME_MODULE) "; then \
	    echo "    $(NAME_MODULE) already loaded, skipping insmod"; \
	else \
	    insmod $(PWD)/$(NAME_MODULE).ko || { \
	        echo "ERROR: insmod $(NAME_MODULE).ko failed"; \
	        dmesg | tail -n 10; \
	        exit 1; \
	    }; \
	fi
	@test -c /dev/$(NAME_MODULE) || { \
	    echo "ERROR: /dev/$(NAME_MODULE) was not created"; \
	    exit 1; \
	}
	@echo "[*] Starting $(NAME_DAEMON) in background"
	@nohup $(PWD)/$(NAME_DAEMON) >$(DAEMON_LOG) 2>&1 < /dev/null & \
	    echo $$! > $(DAEMON_PID)
	@sleep 0.5
	@test -S $(DAEMON_SOCK) || { \
	    echo "ERROR: $(NAME_DAEMON) socket missing, log:"; \
	    cat $(DAEMON_LOG) 2>/dev/null; \
	    exit 1; \
	}
	@echo "[*] Daemon running, pid=$$(cat $(DAEMON_PID))"
	@echo "[*] Launching $(NAME_CTL) root (exit shell to finish install)"
	$(PWD)/$(NAME_CTL) root

# ---------- persistent deploy ----------
install-keep: _root_check all _stop_daemon
	@echo "[*] Installing binaries to $(PREFIX)"
	install -d -m 0755 $(BINDIR) $(SBINDIR)
	install -m 0755 $(NAME_CTL)    $(BINDIR)/$(NAME_CTL)
	install -m 0755 $(NAME_DAEMON) $(SBINDIR)/$(NAME_DAEMON)

	@echo "[*] Installing kernel module to $(KOBJDIR)"
	install -d -m 0755 $(KOBJDIR)
	install -m 0644 $(NAME_MODULE).ko $(KOBJDIR)/$(NAME_MODULE).ko

	@echo "[*] Installing systemd unit to $(UNITDIR)"
	install -d -m 0755 $(UNITDIR)
	install -m 0644 $(NAME_UNIT) $(UNITDIR)/$(NAME_UNIT)

	@echo "[*] Creating config directory $(CONFDIR)"
	install -d -m 0700 $(CONFDIR)
	@if [ ! -f $(CONFDIR)/allowlist.conf ]; then \
	    install -m 0600 /dev/null $(CONFDIR)/allowlist.conf; \
	fi

	@echo "[*] Enabling module autoload via $(MODLOADDIR)/$(NAME_MODCONF)"
	install -d -m 0755 $(MODLOADDIR)
	echo "$(NAME_MODULE)" > $(MODLOADDIR)/$(NAME_MODCONF)

	@echo "[*] Running depmod"
	depmod -a

	@echo "[*] Reloading systemd"
	systemctl daemon-reload

	@echo "[*] Enabling and starting $(NAME_DAEMON)"
	systemctl enable $(NAME_DAEMON)
	systemctl restart $(NAME_DAEMON)
	@sleep 1
	@systemctl is-active --quiet $(NAME_DAEMON) || { \
	    echo "ERROR: $(NAME_DAEMON) failed to start"; \
	    systemctl status $(NAME_DAEMON) --no-pager || true; \
	    journalctl -u $(NAME_DAEMON) -n 30 --no-pager || true; \
	    exit 1; \
	}
	@echo "[*] $(NAME_DAEMON) is active and enabled on boot"
	@echo "[*] Launching $(NAME_CTL) root (exit shell to finish install-keep)"
	$(BINDIR)/$(NAME_CTL) root

# ---------- install shortcut ----------
install-all: install

# ---------- module load / unload ----------
load: module
	@echo "[*] Loading $(NAME_MODULE).ko"
	$(SUDO) insmod $(PWD)/$(NAME_MODULE).ko
	@ls -l /dev/$(NAME_MODULE)

unload:
	$(SUDO) rmmod $(NAME_MODULE) 2>/dev/null || true

# ---------- uninstall ----------
uninstall: _root_check
	@echo "[*] Disabling and stopping $(NAME_DAEMON)"
	-systemctl disable --now $(NAME_DAEMON) 2>/dev/null || true
	@echo "[*] Removing installed files"
	rm -f $(BINDIR)/$(NAME_CTL)
	rm -f $(SBINDIR)/$(NAME_DAEMON)
	rm -f $(UNITDIR)/$(NAME_UNIT)
	rm -f $(KOBJDIR)/$(NAME_MODULE).ko
	rm -f $(MODLOADDIR)/$(NAME_MODCONF)
	rm -f $(DAEMON_PID) $(DAEMON_SOCK)
	@echo "[*] Removing kernel module from running kernel"
	rmmod $(NAME_MODULE) 2>/dev/null || true
	@echo "[*] Refreshing depmod and systemd"
	-depmod -a
	-systemctl daemon-reload
	@echo "[*] Uninstall complete (config kept at $(CONFDIR))"

# ---------- cleanup ----------
clean:
	-$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(NAME_CTL) $(NAME_DAEMON) \
	      *.o *.ko *.mod *.mod.c *.symvers *.order .*.cmd \
	      modules.order Module.symvers

# ---------- info ----------
info:
	@echo "Kernel version   : $(KVER)"
	@echo "Kernel headers   : $(KDIR)"
	@echo "Distro ID        : $(OS_ID)"
	@echo "Package manager  : $(PKG_MGR)"
	@echo "Root helper      : $(if $(SUDO),$(SUDO),'(running as root)')"
	@echo "Install prefix   : $(PREFIX)"
	@echo "Module dir       : $(KOBJDIR)"
	@echo "Config dir       : $(CONFDIR)"

help:
	@echo "KernelRing0 build targets:"
	@echo "  make deps         install build dependencies for this distro"
	@echo "  make              build kernel module and userspace tools"
	@echo "  make install      TEMPORARY deploy: insmod + start daemon + root shell"
	@echo "                    (must be run as real root)"
	@echo "  make install-keep PERSISTENT deploy: files + systemd + autoload + root shell"
	@echo "                    (must be run as real root)"
	@echo "  make load         load the kernel module only"
	@echo "  make unload       unload the kernel module"
	@echo "  make uninstall    remove persistent deployment (keeps allowlist.conf)"
	@echo "  make clean        remove build artifacts"
	@echo "  make info         show detected system information"
