/*
 * Copyright (C) 2014 George Amvrosiadis.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>

#include "kerncompat.h"
#include "commands.h"
#include "ioctl.h"
#include "send-utils.h" /* needed for subvol_search */

/* TBD: replace with #include "linux/ioprio.h" in some years */
#if !defined (IOPRIO_H)
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_PRIO_VALUE(class, data) \
		(((class) << IOPRIO_CLASS_SHIFT) | (data))
#define IOPRIO_CLASS_IDLE 3
#endif

#define MAX_SUBV_LEN 1024
static struct btrfs_defrag cancel_defrag;
static char cancel_subvol[MAX_SUBV_LEN];
static int cancel_in_progress = 0;
static int g_verbose = 0;

static const char * const defrag_cmd_group_usage[] = {
	"btrfs defrag <command> [options] <subvol>",
	NULL
};

struct btrfs_defrag {
	int mnt_fd;
	char *root_path;
	struct subvol_uuid_search sus;

	/* Defrag priority state */
	int ioprio_class;
	int ioprio_classdata;
};

static int init_root_path(struct btrfs_defrag *d, const char *subvol)
{
	int ret = 0;

	if (d->root_path)
		goto out;

	ret = find_mount_root(subvol, &d->root_path);
	if (ret < 0) {
		ret = -EINVAL;
		fprintf(stderr, "ERROR: failed to determine mount point "
				"for %s\n", subvol);
		goto out;
	}

	d->mnt_fd = open(d->root_path, O_RDONLY | O_NOATIME);
	if (d->mnt_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: can't open '%s': %s\n", d->root_path,
			strerror(-ret));
		goto out;
	}

	ret = subvol_uuid_search_init(d->mnt_fd, &d->sus);
	if (ret < 0) {
		fprintf(stderr, "ERROR: failed to initialize subvol search. "
				"%s\n", strerror(-ret));
		goto out;
	}

out:
	return ret;
}

static int get_root_id(struct btrfs_defrag *d, const char *path, u64 *root_id)
{
	struct subvol_info *si;

	si = subvol_uuid_search(&d->sus, 0, NULL, 0, path,
			subvol_search_by_path);
	if (!si)
		return -ENOENT;
	*root_id = si->root_id;
	free(si->path);
	free(si);
	return 0;
}

static int do_cancel(struct btrfs_defrag *defrag, char *subvol)
{
	int ret = 0;
	u64 root_id;
	int subvol_fd = -1;
	struct subvol_info *si;

	ret = get_root_id(defrag, get_subvol_name(defrag->root_path, subvol),
			&root_id);
	if (ret < 0) {
		fprintf(stderr, "ERROR: could not resolve "
				"root_id for %s\n", subvol);
		return ret;
	}

	si = subvol_uuid_search(&defrag->sus, root_id, NULL, 0, NULL,
				subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		return ret;
	}

	subvol_fd = openat(defrag->mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		return ret;
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_DEFRAG_CANCEL, NULL);
	if (ret < 0) {
		fprintf(stderr, "ERROR: defrag cancel failed on %s: %s\n",
			subvol,
			errno == ENOTCONN ? "not running" : strerror(errno));
		return ret;
	}

	return ret;
}

static int do_send(struct btrfs_defrag *defrag, u64 root_id)
{
	int ret;
	struct subvol_info *si;
	int subvol_fd = -1;
	struct btrfs_ioctl_defrag_args io_defrag;

	si = subvol_uuid_search(&defrag->sus, root_id, NULL, 0, NULL,
			subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		goto out;
	}

	subvol_fd = openat(defrag->mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		goto out;
	}

	/* before moving on, set the ioprio */
	ret = syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
		IOPRIO_PRIO_VALUE(defrag->ioprio_class, defrag->ioprio_classdata));
	if (ret)
		fprintf(stderr, "WARNING: setting ioprio failed: %s (ignored).\n",
			strerror(errno));

	memset(&io_defrag, 0, sizeof(io_defrag));

	ret = ioctl(subvol_fd, BTRFS_IOC_DEFRAG_START, &io_defrag);

	if (cancel_in_progress) {
		fprintf(stderr, "defrag ioctl terminated\n");
		goto out;
	}

	if (ret) {
		ret = -errno;
		fprintf(stderr, "ERROR: defrag ioctl failed with %d: %s\n",
			ret, strerror(-ret));
		if (ret == -EINVAL)
			fprintf(stderr, "Try upgrading your kernel.\n");
		goto out;
	}

	if (g_verbose > 0)
		fprintf(stderr, "BTRFS_IOC_DEFRAG_START returned %d\n", ret);

	ret = 0;
out:
	if (subvol_fd != -1)
		close(subvol_fd);
	if (si) {
		free(si->path);
		free(si);
	}
	return ret;
}

static void send_sigint_terminate(int signal)
{
	int ret;

	fprintf(stderr, "Received SIGINT. Terminating...\n");
	cancel_in_progress = 1;
	ret = do_cancel(&cancel_defrag, cancel_subvol);
	if (ret < 0)
		perror("Defrag cancel failed");
}

static int send_handle_sigint(struct btrfs_defrag *defrag, char *subvol) {
	struct sigaction sa = {
		.sa_handler = defrag == NULL ? SIG_DFL : send_sigint_terminate,
	};

	memset(cancel_subvol, 0, MAX_SUBV_LEN);

	if (defrag)
		memcpy(&cancel_defrag, defrag, sizeof(*defrag));

	if (subvol)
		memcpy(cancel_subvol, subvol, strlen(subvol) < MAX_SUBV_LEN ?
			strlen(cancel_subvol) : MAX_SUBV_LEN - 1);

	return sigaction(SIGINT, &sa, NULL);
}

const char * const cmd_defrag_start_usage[] = {
	"btrfs defrag start [-Bv] [-C <class> -N <classdata>] <subvol>",
	"Run defrag on the given subvolume.",
	"This will call defrag on the whole subvolume. On the kernel side,",
	"defrag is invoked on each inode tied to the subvolume",
	"\n",
	"-B               run send in the background",
	"-v               Enable verbose debug output. Each occurrence of",
	"                 this option increases the verbose level more.",
	"-C <class>       set ioprio class (see ionice(1) manpage)",
	"-N <classdata>   set ioprio classdata (see ionice(1) manpage)",
	NULL
};

int cmd_defrag_start(int argc, char **argv)
{
	int c, pid, ret = 0;
	int do_background = 0;
	struct btrfs_defrag defrag;
	int ioprio_class = IOPRIO_CLASS_IDLE;
	int ioprio_classdata = 0;
	char *subvol = NULL;
	u64 root_id;

	memset(&defrag, 0, sizeof(defrag));

	while ((c = getopt(argc, argv, "BvC:N:")) != -1) {
		switch (c) {
		case 'B':
			do_background = 1;
			break;
		case 'v':
			g_verbose++;
			break;
		case 'C':
			ioprio_class = (int) strtol(optarg, NULL, 10);
			break;
		case 'N':
			ioprio_classdata = (int) strtol(optarg, NULL, 10);
			break;
		case '?':
		default:
			fprintf(stderr, "ERROR: defrag args invalid.\n");
			ret = 1;
			goto out;
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_defrag_start_usage);

	/* Set the IO priorities */
	defrag.ioprio_class = ioprio_class;
	defrag.ioprio_classdata = ioprio_classdata;

	/* use first send subvol to determine mount_root */
	subvol = argv[optind];

	subvol = realpath(argv[optind], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s - %s\n",
			argv[optind], strerror(errno));
		goto out;
	}

	ret = init_root_path(&defrag, subvol);
	if (ret < 0)
		goto out;

	if (do_background) {
		pid = fork();
		if (pid == -1) {
			fprintf(stderr, "ERROR: cannot defrag, fork failed: "
				"%s\n", strerror(errno));
			ret = 1;
			goto out;
		}

		if (pid) {
			printf("defrag started at %s\n", subvol);
			ret = 0;
			goto out;
		}
	}

	send_handle_sigint(&defrag, subvol);

	ret = get_root_id(&defrag, get_subvol_name(defrag.root_path, subvol),
			&root_id);
	if (ret < 0) {
		fprintf(stderr, "ERROR: could not resolve root_id "
			"for %s\n", subvol);
		goto out;
	}

	ret = do_send(&defrag, root_id);
	if (ret < 0)
		goto out;

	send_handle_sigint(NULL, NULL);

	ret = 0;
out:
	free(subvol);
	if (defrag.mnt_fd >= 0)
		close(defrag.mnt_fd);
	free(defrag.root_path);
	subvol_uuid_search_finit(&defrag.sus);
	return !!ret;
}

static const char * const cmd_defrag_cancel_usage[] = {
	"btrfs defrag cancel <subvol>",
	"Cancel a running defrag",
	NULL
};

static int cmd_defrag_cancel(int argc, char **argv)
{
	int ret;
	char *subvol;
	struct btrfs_defrag defrag;

	memset(&defrag, 0, sizeof(defrag));

	if (check_argc_exact(argc, 2))
		usage(cmd_defrag_cancel_usage);

	/* use first send subvol to determine mount_root */
	subvol = realpath(argv[1], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s\n", argv[optind]);
		goto out;
	}

	ret = init_root_path(&defrag, subvol);
	if (ret < 0)
		goto out;

	ret = do_cancel(&defrag, subvol);
	if (ret < 0)
		goto out;

	printf("defrag cancelled\n");

out:
	free(subvol);
	if (defrag.mnt_fd >= 0)
		close(defrag.mnt_fd);
	free(defrag.root_path);
	subvol_uuid_search_finit(&defrag.sus);
	return ret;
}

static const char * const cmd_defrag_status_usage[] = {
	"btrfs defrag status <subvol>",
	"Show status of running or finished filesystem defrag",
	NULL
};

static int cmd_defrag_status(int argc, char **argv)
{
	int ret = 0;
	char *subvol;
	int subvol_fd = -1;
	struct btrfs_defrag defrag;
	struct subvol_info *si;
	struct btrfs_ioctl_defrag_args da;
	u64 root_id;

	memset(&defrag, 0, sizeof(defrag));

	if (check_argc_exact(argc, 2))
		usage(cmd_defrag_status_usage);

	/* use first send subvol to determine mount_root */
	subvol = realpath(argv[1], NULL);
	if (!subvol) {
		ret = -errno;
		fprintf(stderr, "ERROR: unable to resolve %s\n", argv[optind]);
		goto out;
	}

	ret = init_root_path(&defrag, subvol);
	if (ret < 0)
		goto out;

	ret = get_root_id(&defrag, get_subvol_name(defrag.root_path, subvol),
			&root_id);
	if (ret < 0) {
		fprintf(stderr, "ERROR: could not resolve root_id for %s\n",
			subvol);
		goto out;
	}

	si = subvol_uuid_search(&defrag.sus, root_id, NULL, 0, NULL,
				subvol_search_by_root_id);
	if (!si) {
		ret = -ENOENT;
		fprintf(stderr, "ERROR: could not find subvol info for %llu",
				root_id);
		goto out;
	}

	subvol_fd = openat(defrag.mnt_fd, si->path, O_RDONLY | O_NOATIME);
	if (subvol_fd < 0) {
		ret = -errno;
		fprintf(stderr, "ERROR: open %s failed. %s\n", si->path,
				strerror(-ret));
		goto out;
	}

	ret = ioctl(subvol_fd, BTRFS_IOC_DEFRAG_PROGRESS, &da);
	if (ret < 0) {
		fprintf(stderr, "ERROR: defrag status failed on %s: %s\n", subvol,
			strerror(errno));
		ret = 1;
		goto out;
	}

	printf("Defragged %llu bytes, %s %u sec.\n"
		"Defragged %llu bytes out of order.\n",
		da.progress.bytes_total,
		da.progress.running ? "running for" : "finished after",
		da.progress.elapsed_time, da.progress.bytes_best_effort);

	ret = 0;
out:
	free(subvol);
	if (defrag.mnt_fd >= 0)
		close(defrag.mnt_fd);
	free(defrag.root_path);
	subvol_uuid_search_finit(&defrag.sus);
	return ret;
}

const struct cmd_group defrag_cmd_group = {
	defrag_cmd_group_usage, NULL, {
		{ "start", cmd_defrag_start, cmd_defrag_start_usage, NULL, 0 },
		{ "cancel", cmd_defrag_cancel, cmd_defrag_cancel_usage, NULL, 0 },
		{ "status", cmd_defrag_status, cmd_defrag_status_usage, NULL, 0 },
		NULL_CMD_STRUCT
	}
};

int cmd_defrag(int argc, char **argv)
{
	return handle_command_group(&defrag_cmd_group, argc, argv);
}
