/*
 * Copyright (C) 2012 Red Hat, Inc. All rights reserved.
 *
 * This file is part of the device-mapper userspace tools.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU General Public License v.2.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>		/* For PATH_MAX for musl libc */
#include "lvm2app.h"

#define KMSG_DEV_PATH        "/dev/kmsg"
#define LVM_CONF_USE_LVMETAD "global/use_lvmetad"

#define DEFAULT_UNIT_DIR      "/tmp"
#define UNIT_TARGET_LOCAL_FS  "local-fs.target"
#define UNIT_TARGET_REMOTE_FS "remote-fs.target"

static char unit_path[PATH_MAX];
static char target_path[PATH_MAX];
static char message[PATH_MAX];
static int kmsg_fd = -1;

enum {
	UNIT_EARLY,
	UNIT_MAIN,
	UNIT_NET
};

static const char *unit_names[] = {
	[UNIT_EARLY] = "lvm2-activation-early.service",
	[UNIT_MAIN] = "lvm2-activation.service",
	[UNIT_NET] = "lvm2-activation-net.service"
};

__attribute__ ((format(printf, 1, 2)))
static void kmsg(const char *format, ...)
{
	va_list ap;
	int n;

	va_start(ap, format);
	n = vsnprintf(message, sizeof(message), format, ap);
	va_end(ap);

	if (kmsg_fd < 0 || (n < 0 || ((unsigned) n + 1 > sizeof(message))))
		return;

	(void) write(kmsg_fd, message, n + 1);
}

static int lvm_uses_lvmetad(void)
{
	lvm_t lvm;
	int r;

	if (!(lvm = lvm_init(NULL))) {
		kmsg("LVM: Failed to initialize library context for activation generator.\n");
		return 0;
	}
	r = lvm_config_find_bool(lvm, LVM_CONF_USE_LVMETAD, 0);
	lvm_quit(lvm);

	return r;
}

static int register_unit_with_target(const char *dir, const char *unit, const char *target)
{
	int r = 1;

	if (dm_snprintf(target_path, PATH_MAX, "%s/%s.wants", dir, target) < 0) {
		r = 0; goto out;
	}
	(void) dm_prepare_selinux_context(target_path, S_IFDIR);
	if (mkdir(target_path, 0755) < 0 && errno != EEXIST) {
		kmsg("LVM: Failed to create target directory %s: %m.\n", target_path);
		r = 0; goto out;
	}

	if (dm_snprintf(target_path, PATH_MAX, "%s/%s.wants/%s", dir, target, unit) < 0) {
		r = 0; goto out;
	}
	(void) dm_prepare_selinux_context(target_path, S_IFLNK);
	if (symlink(unit_path, target_path) < 0) {
		kmsg("LVM: Failed to create symlink for unit %s: %m.\n", unit);
		r = 0;
	}
out:
	dm_prepare_selinux_context(NULL, 0);
	return r;
}

static int generate_unit(const char *dir, int unit)
{
	FILE *f;
	const char *unit_name = unit_names[unit];
	const char *target_name = unit == UNIT_NET ? UNIT_TARGET_REMOTE_FS : UNIT_TARGET_LOCAL_FS;

	if (dm_snprintf(unit_path, PATH_MAX, "%s/%s", dir, unit_name) < 0)
		return 0;

	if (!(f = fopen(unit_path, "wxe"))) {
		kmsg("LVM: Failed to create unit file %s: %m.\n", unit_name);
		return 0;
	}

	fputs("# Automatically generated by lvm2-activation-generator.\n"
	      "#\n"
	      "# This unit is responsible for direct activation of LVM2 logical volumes\n"
	      "# if lvmetad daemon is not used (global/use_lvmetad=0 lvm.conf setting),\n"
	      "# hence volume autoactivation is not applicable.\n"
	      "# Direct LVM2 activation requires udev to be settled!\n\n"
	      "[Unit]\n"
	      "Description=Activation of LVM2 logical volumes\n"
	      "Documentation=man:lvm(8) man:vgchange(8)\n"
	      "SourcePath=/etc/lvm/lvm.conf\n"
	      "DefaultDependencies=no\n", f);

	if (unit == UNIT_NET) {
		fputs("After=iscsi.service fcoe.service\n"
		      "Before=remote-fs.target shutdown.target\n", f);
	} else {
		if (unit == UNIT_EARLY) {
			fputs("After=systemd-udev-settle.service\n", f);
			fputs("Before=cryptsetup.target\n", f);
		} else
			fputs("After=lvm2-activation-early.service cryptsetup.target\n", f);

		fputs("Before=local-fs.target shutdown.target\n"
		      "Wants=systemd-udev-settle.service\n\n", f);
	}

	fputs("[Service]\n"
	      "ExecStart=/usr/sbin/lvm vgchange -aay --sysinit\n"
	      "Type=oneshot\n", f);

	if (fclose(f) < 0) {
		kmsg("LVM: Failed to write unit file %s: %m.\n", unit_name);
		return 0;
	}

	if (!register_unit_with_target(dir, unit_name, target_name)) {
		kmsg("LVM: Failed to register unit %s with target %s.\n", unit_name, target_name);
		return 0;
	}

	return 1;
}

int main(int argc, char *argv[])
{
	const char *dir;
	int r = EXIT_SUCCESS;

	kmsg_fd = open(KMSG_DEV_PATH, O_WRONLY|O_NOCTTY);

	if (argc > 1 && argc != 4) {
		kmsg("LVM: Activation generator takes three or no arguments.\n");
		r = EXIT_FAILURE; goto out;
	}

	/* If lvmetad used, rely on autoactivation instead of direct activation. */
	if (lvm_uses_lvmetad()) {
		kmsg("LVM: Logical Volume autoactivation enabled.\n");
		goto out;
	}

	dir = argc > 1 ? argv[1] : DEFAULT_UNIT_DIR;

	if (!generate_unit(dir, UNIT_EARLY) ||
	    !generate_unit(dir, UNIT_MAIN) ||
	    !generate_unit(dir, UNIT_NET))
		r = EXIT_FAILURE;
out:
	kmsg("LVM: Activation generator %s.\n", r ? "failed" : "successfully completed");
	if (kmsg_fd != -1)
		(void) close(kmsg_fd);
	return r;
}
