/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * VFS Automatic Mounts
 *
 * Authors: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *          Robert Hrusecky <roberth@cs.utexas.edu>
 *          Omar Jamil <omarj2898@gmail.com>
 *          Sachin Beldona <sachinbeldona@utexas.edu>
 *          Sergiu Moga <sergiu@unikraft.io>
 *
 * Copyright (c) 2019, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 * Copyright (c) 2023, Unikraft GmbH. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <sys/mount.h>
#include <uk/assert.h>
#ifdef CONFIG_LIBUKCPIO
#include <uk/cpio.h>
#endif /* CONFIG_LIBUKCPIO */
#include <uk/init.h>
#include <uk/libparam.h>
#include <uk/plat/memory.h>
#include <sys/stat.h>
#include <vfscore/mount.h>

#ifdef CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS
#include <errno.h>
#include <uk/config.h>
#include <uk/arch/types.h>
#endif /* CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS */

#if CONFIG_LIBVFSCORE_FSTAB
#define LIBVFSCORE_FSTAB_VOLUME_ARGS_SEP			':'
#define LIBVFSCORE_FSTAB_UKOPTS_ARGS_SEP			','
#endif /* CONFIG_LIBVFSCORE_FSTAB */

#define LIBVFSCORE_EXTRACT_DRV					"extract"
#define LIBVFSCORE_EXTRACT_DEV_INITRD0				"initrd0"
#define LIBVFSCORE_EXTRACT_DEV_EMBEDDED				"embedded"

struct vfscore_volume {
	/* Volume source device */
	const char *sdev;
	/* Mount point absolute path */
	char *path;
	/* Corresponding filesystem driver name */
	const char *drv;
	/* Mount flags */
	unsigned long flags;
	/* Mount options */
	const char *opts;
#if CONFIG_LIBVFSCORE_FSTAB
	/* Unikraft Mount options, see vfscore_mount_volume() */
	char *ukopts;
#endif /* CONFIG_LIBVFSCORE_FSTAB */
};

#if CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS
#if CONFIG_LIBVFSCORE_ROOTFS_EINITRD
extern const char vfscore_einitrd_start[];
extern const char vfscore_einitrd_end;
#endif /* CONFIG_LIBVFSCORE_ROOTFS_EINITRD */

static int vfscore_extract_volume(const struct vfscore_volume *vv)
{
	const void *vbase = NULL;
	size_t vlen = 0;
	int rc;

	UK_ASSERT(vv);
	UK_ASSERT(vv->path);

	/* Detect which initrd to use */
	/* TODO: Support multiple Initial RAM Disks */
	if (!strcmp(vv->sdev, LIBVFSCORE_EXTRACT_DEV_INITRD0)) {
		struct ukplat_memregion_desc *initrd;

		rc = ukplat_memregion_find_initrd0(&initrd);
		if (unlikely(rc < 0 || initrd->len == 0)) {
			uk_pr_crit("Could not find an initrd!\n");
			return -1;
		}

		vbase = (void *)initrd->vbase;
		vlen = initrd->len;
	}
#if CONFIG_LIBVFSCORE_ROOTFS_EINITRD
	else if (!strcmp(vv->sdev, LIBVFSCORE_EXTRACT_DEV_EMBEDDED)) {
		vbase = (const void *)vfscore_einitrd_start;
		vlen  = (size_t)((uintptr_t)&vfscore_einitrd_end
				 - (uintptr_t)vfscore_einitrd_start);
	}
#endif /* CONFIG_LIBVFSCORE_ROOTFS_EINITRD*/
	else {
		uk_pr_crit("\"%s\" is an invalid or unsupported initrd source!\n",
			   vv->sdev);
		return -EINVAL;
	}

	if (unlikely(vlen == 0))
		uk_pr_warn("Initrd \"%s\" seems to be empty.\n", vv->sdev);

	uk_pr_info("Extracting initrd @ %p (%"__PRIsz" bytes) to %s...\n",
		   vbase, vlen, vv->path);
	rc = ukcpio_extract(vv->path, vbase, vlen);
	if (unlikely(rc)) {
		uk_pr_crit("Failed to extract cpio archive to %s: %d\n",
			   vv->path, rc);
		return -EIO;
	}
	return 0;
}
#endif /* CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS */

#if CONFIG_LIBVFSCORE_FSTAB
/* Handle `mkmp` Unikraft Mount Option */
static int vfscore_ukopt_mkmp(char *path)
{
	char *pos, *prev_pos;
	int rc;

	UK_ASSERT(path);
	UK_ASSERT(path[0] == '/');

	pos = path;
	do {
		prev_pos = pos;
		pos = strchr(pos + 1, '/');

		if (pos) {
			if (pos[0] == '\0')
				break;

			/* Zero out the next '/' */
			*pos = '\0';
		}

		/* Do not allow `/./` or `/../` in the path. Also do not allow
		 * overwriting .. or . files
		 */
		if (unlikely(prev_pos[1] == '.' &&
			     /* /../ and /.. */
			     ((prev_pos[2] == '.' &&
			       (prev_pos[3] == '/' || prev_pos[3] == '\0')) ||
				/* OR /./ and /. */
			      (prev_pos[2] == '/' || prev_pos[2] == '\0')
			     ))) {
			uk_pr_err("'.' or '..' are not supported in mount paths.\n");
			return -EINVAL;
		}

		/* mkdir() with S_IRWXU */
		rc = mkdir(path, 0700);
		if (rc && errno != EEXIST)
			return -errno;

		/* Restore current '/' */
		if (pos)
			*pos = '/';

		/* Handle paths with multiple `/` */
		while (pos && pos[1] == '/')
			pos++;
	} while (pos);

	return 0;
}

/**
 * vv->ukopts must follow the pattern below, each option separated by
 * the character defined through LIBVFSCORE_FSTAB_UKOPTS_ARGS_SEP (e.g. with
 * LIBVFSCORE_FSTAB_UKOPTS_ARGS_SEP = ','):
 *	[<ukopt1>,<ukopt2>,<ukopt3>,...,<ukoptN>]
 *
 * Currently implemented, Unikraft Mount options:
 * - mkmp	Make mount point. Ensures that the specified mount point
 *		exists. If it does not exist in the current vfs, the directory
 *		structure is created.
 */
static int vfscore_volume_process_ukopts(const struct vfscore_volume *vv)
{
	const char *o_curr;
	char *o_next;
	int rc;

	UK_ASSERT(vv);
	UK_ASSERT(vv->path);

	o_curr = (const char *) vv->ukopts;
	while (o_curr) {
		o_next = strchr(o_curr, LIBVFSCORE_FSTAB_UKOPTS_ARGS_SEP);
		if (o_next) {
			*o_next = '\0';
			o_next++;
		}

		/* First check is so we do not run `mkmp` on `/` */
		if (!strcmp(o_curr, "mkmp") && vv->path[1] != '\0') {
			rc = vfscore_ukopt_mkmp(vv->path);
			if (unlikely(rc)) {
				uk_pr_err("Failed to process ukopt \"mkmp\": %d\n",
					  rc);
				return rc;
			}
		}

		o_curr = o_next;
	}

	return 0;
}
#endif /* CONFIG_LIBVFSCORE_FSTAB */

static inline int vfscore_mount_volume(const struct vfscore_volume *vv)
{
	int rc;

	UK_ASSERT(vv);
	UK_ASSERT(vv->sdev);
	UK_ASSERT(vv->path);

	uk_pr_debug("vfs.fstab: Mounting: %s:%s:%s:%lo:%s:%s...\n",
		    vv->sdev[0] == '\0' ? "none" : vv->sdev,
		    vv->path, vv->drv, vv->flags,
		    vv->opts == NULL ? "" : vv->opts,
		    vv->ukopts == NULL ? "" : vv->ukopts);
	if (vv->ukopts) {
		rc = vfscore_volume_process_ukopts(vv);
		if (unlikely(rc < 0))
			return rc;
	}

#if CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS
	if (!strcmp(vv->drv, LIBVFSCORE_EXTRACT_DRV)) {
		return vfscore_extract_volume(vv);
	}
#endif /* CONFIG_LIBUKCPIO && CONFIG_LIBRAMFS */
	return mount(vv->sdev, vv->path, vv->drv, vv->flags, vv->opts);
}

#ifdef CONFIG_LIBVFSCORE_FSTAB

static char *vfscore_fstab[CONFIG_LIBVFSCORE_FSTAB_SIZE];

UK_LIBPARAM_PARAM_ARR_ALIAS(fstab, &vfscore_fstab, charp,
			    CONFIG_LIBVFSCORE_FSTAB_SIZE,
			"Automount table: dev:path:fs[:flags[:opts[:ukopts]]]");

/**
 * Expected command-line argument format:
 *	vfs.fstab=[
 *		"<src_dev>:<mntpoint>:<fsdriver>[:<flags>:<opts>:<ukopts>]"
 *		"<src_dev>:<mntpoint>:<fsdriver>[:<flags>:<opts>:<ukopts>]"
 *		...
 *	]
 * These list elements are expected to be separated by whitespaces.
 * Mount options, flags and Unikraft mount options are optional.
 */
static char *next_volume_arg(char **argptr)
{
	char *nsep;
	char *arg;

	UK_ASSERT(argptr);

	if (!*argptr || (*argptr)[0] == '\0') {
		/* We likely got called again after we already
		 * returned the last argument
		 */
		*argptr = NULL;
		return NULL;
	}

	arg = *argptr;
	nsep = strchr(*argptr, LIBVFSCORE_FSTAB_VOLUME_ARGS_SEP);
	if (!nsep) {
		/* No next separator, we hit the last argument */
		*argptr = NULL;
		goto out;
	}

	/* Split C string by overwriting the separator */
	nsep[0] = '\0';
	/* Move argptr to next argument */
	*argptr = nsep + 1;

out:
	/* Return NULL for empty arguments */
	if (*arg == '\0')
		return NULL;
	return arg;
}

static int vfscore_parse_volume(char *v, struct vfscore_volume *vv)
{
	const char *strflags;
	char *pos;

	UK_ASSERT(v);
	UK_ASSERT(vv);

	pos = v;
	vv->sdev   = next_volume_arg(&pos);
	vv->path   = next_volume_arg(&pos);
	vv->drv    = next_volume_arg(&pos);
	strflags   = next_volume_arg(&pos);
	vv->opts   = next_volume_arg(&pos);
	vv->ukopts = next_volume_arg(&pos);

	/* path and drv are mandatory */
	if (unlikely(!vv->path || !vv->drv)) {
		uk_pr_err("vfs.fstab: Incomplete entry: Require mountpoint and filesystem driver\n");
		return -EINVAL;
	}

	/* Fill source device with empty string if missing */
	if (!vv->sdev)
		vv->sdev = "";

	/* Check that given path is absolute */
	if (unlikely(vv->path[0] != '/')) {
		uk_pr_err("vfs.fstab: Mountpoint \"%s\" is not absolute\n",
			  vv->path);
		return -EINVAL;
	}

	/* Parse flags */
	if (strflags && strflags[0] != '\0')
		vv->flags = strtol(strflags, NULL, 0);
	else
		vv->flags = 0;

	uk_pr_debug("vfs.fstab: Parsed: %s:%s:%s:%lx:%s:%s\n",
		    vv->sdev[0] == '\0' ? "none" : vv->sdev,
		    vv->path, vv->drv, vv->flags,
		    vv->opts == NULL ? "" : vv->opts,
		    vv->ukopts == NULL ? "" : vv->ukopts);
	return 0;
}
#endif /* CONFIG_LIBVFSCORE_FSTAB */

#if CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS
static int vfscore_automount_rootfs(void)
{
	/* Convert to `struct vfscore_volume` */
	struct vfscore_volume vv = {
#ifdef CONFIG_LIBVFSCORE_ROOTDEV
		.sdev = CONFIG_LIBVFSCORE_ROOTDEV,
#else
		.sdev = "",
#endif /* CONFIG_LIBVFSCORE_ROOTDEV */
		.path = "/",
#if CONFIG_LIBVFSCORE_ROOTFS_INITRD || CONFIG_LIBVFSCORE_ROOTFS_EINITRD
		.drv = "ramfs",
#elif defined CONFIG_LIBVFSCORE_ROOTFS
		.drv = CONFIG_LIBVFSCORE_ROOTFS,
#else
		.drv = "",
#endif
#ifdef CONFIG_LIBVFSCORE_ROOTFLAGS
		.flags = CONFIG_LIBVFSCORE_ROOTFLAGS,
#else
		.flags = 0,
#endif /* CONFIG_LIBVFSCORE_ROOTFLAGS */
#ifdef CONFIG_LIBVFSCORE_ROOTOPTS
		.opts = CONFIG_LIBVFSCORE_ROOTOPTS,
#else
		.opts = "",
#endif /* CONFIG_LIBVFSCORE_ROOTOPTS */
	};

#if CONFIG_LIBVFSCORE_ROOTFS_INITRD || CONFIG_LIBVFSCORE_ROOTFS_EINITRD
	struct vfscore_volume vv2 = {
#if CONFIG_LIBVFSCORE_ROOTFS_INITRD
		.sdev = LIBVFSCORE_EXTRACT_DEV_INITRD0,
#elif CONFIG_LIBVFSCORE_ROOTFS_EINITRD
		.sdev = LIBVFSCORE_EXTRACT_DEV_EMBEDDED,
#else
		.sdev = "",
#endif
		.path = "/",
		.drv = LIBVFSCORE_EXTRACT_DRV,
,		.flags = 0,
		.opts = "",
	};
#if /* CONFIG_LIBVFSCORE_ROOTFS_INITRD || CONFIG_LIBVFSCORE_ROOTFS_EINITRD */
	int rc;

	/*
	 * Initialization of the root filesystem '/'
	 * NOTE: Any additional sub mount points (like '/dev' with devfs)
	 * have to be mounted later.
	 *
	 * Silently return 0, as user might not have configured implicit rootfs.
	 */
	if (!vv.drv || vv.drv[0] == '\0')
		return 0;

	rc = vfscore_mount_volume(&vv);
	if (unlikely(rc)) {
		uk_pr_crit("Failed to mount %s (%s) at /: %d\n", vv.sdev,
			   vv.drv, rc);
		return rc;
	}

#if CONFIG_LIBVFSCORE_ROOTFS_INITRD || CONFIG_LIBVFSCORE_ROOTFS_EINITRD
	rc = vfscore_mount_volume(&vv2);
	if (unlikely(rc)) {
		uk_pr_crit("Failed to extract %s (%s) to /: %d\n", vv2.sdev,
			   vv2.drv, rc);
		return rc;
	}
#endif /* CONFIG_LIBVFSCORE_ROOTFS_INITRD || CONFIG_LIBVFSCORE_ROOTFS_EINITRD */

	return rc;
}
#else /* !CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS */
static int vfscore_automount_rootfs(void)
{
	return 0;
}
#endif /* !CONFIG_LIBVFSCORE_AUTOMOUNT_ROOTFS */

#ifdef CONFIG_LIBVFSCORE_FSTAB
static int vfscore_automount_fstab_volumes(void)
{
	struct vfscore_volume vv;
	int rc, i;

	for (i = 0; i < CONFIG_LIBVFSCORE_FSTAB_SIZE && vfscore_fstab[i]; i++) {
		vfscore_parse_volume(vfscore_fstab[i], &vv);
		if (unlikely(rc))
			return rc;

		rc = vfscore_mount_volume(&vv);
		if (unlikely(rc)) {
			uk_pr_err("Failed to mount %s: error %d\n", vv.sdev,
				  rc);

			return rc;
		}
	}

	return 0;
}
#else /* CONFIG_LIBVFSCORE_FSTAB */
static int vfscore_automount_fstab_volumes(void)
{
	return 0;
}
#endif /* !CONFIG_LIBVFSCORE_FSTAB */

static int vfscore_automount(struct uk_init_ctx *ictx __unused)
{
	int rc;

	rc = vfscore_automount_rootfs();
	if (unlikely(rc < 0))
		return rc;

	return vfscore_automount_fstab_volumes();
}

extern struct uk_list_head mount_list;

static void vfscore_autoumount(const struct uk_term_ctx *tctx __unused)
{
	struct mount *mp;
	int rc;

	uk_list_for_each_entry_reverse(mp, &mount_list, mnt_list) {
		/* For now, flags = 0 is enough. */
		rc = VFS_UNMOUNT(mp, 0);
		if (unlikely(rc))
			uk_pr_err("Failed to unmount %s: error %d.\n",
				  mp->m_path, rc);
	}
}

uk_rootfs_initcall_prio(vfscore_automount, vfscore_autoumount, 4);
