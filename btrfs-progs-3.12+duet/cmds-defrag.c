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
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>

#include "commands.h"
#include "kerncompat.h"
#include "utils.h"
#include "ioctl.h"

/* TBD: replace with #include "linux/ioprio.h" in some years */
#if !defined (IOPRIO_H)
#define IOPRIO_WHO_PROCESS 1
#define IOPRIO_CLASS_SHIFT 13
#define IOPRIO_PRIO_VALUE(class, data) \
		(((class) << IOPRIO_CLASS_SHIFT) | (data))
#define IOPRIO_CLASS_IDLE 3
#endif

static int cancel_in_progress = 0;
static struct btrfs_defrag cancel_defrag;
static int g_verbose = 0;

static const char * const defrag_cmd_group_usage[] = {
	"btrfs defrag <command> [options] <path|device>",
	NULL
};

struct btrfs_defrag {
	int fdmnt;
	char *path;
	struct btrfs_ioctl_defrag_range_args range;
};

static int do_cancel(struct btrfs_defrag *defrag)
{
	int ret = 0;

	ret = ioctl(defrag->fdmnt, BTRFS_IOC_DEFRAG_CANCEL, NULL);
	if (ret < 0) {
		fprintf(stderr, "ERROR: defrag cancel failed on %s: %s\n",
			defrag->path,
			errno == ENOTCONN ? "not running" : strerror(errno));
	}

	return ret;
}

static int do_defrag(struct btrfs_defrag *defrag)
{
	int ret;
	struct btrfs_ioctl_defrag_args da;

	memset(&da, 0, sizeof(da));
	memcpy(&da.range, &defrag->range, sizeof(da.range));

	ret = ioctl(defrag->fdmnt, BTRFS_IOC_DEFRAG_START, &da);

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
	return ret;
}

static void defrag_sigint_terminate(int signal)
{
	int ret;

	fprintf(stderr, "Received SIGINT. Terminating...\n");
	cancel_in_progress = 1;
	ret = do_cancel(&cancel_defrag);
	if (ret < 0)
		perror("Defrag cancel failed");
}

static int defrag_handle_sigint(struct btrfs_defrag *defrag) {
	struct sigaction sa = {
		.sa_handler = defrag == NULL ? SIG_DFL :
				defrag_sigint_terminate,
	};

	if (defrag)
		memcpy(&cancel_defrag, defrag, sizeof(*defrag));

	return sigaction(SIGINT, &sa, NULL);
}

const char * const cmd_defrag_start_usage[] = {
	"btrfs defrag start [-Bv] [-C <class> -N <classdata>] <path|device>",
	"Run defrag on the given file system.",
	"This will call defrag on the whole subvolume. On the kernel side,",
	"defrag is invoked on each inode tied to the file system",
	"\n",
	"-B               run send in the background",
	"-v               Enable verbose debug output. Each occurrence of",
	"                 this option increases the verbose level more.",
	"-c[zlib,lzo]     compress the file while defragmenting",
	"-f               flush data to disk immediately after defragmenting",
	"-s start         defragment only from byte 'start' onward",
	"-l len           defragment only up to 'len' bytes",
	"-t size          max size of file to be considered for defragmenting",
	"-C <class>       set ioprio class (see ionice(1) manpage)",
	"-N <classdata>   set ioprio classdata (see ionice(1) manpage)",
	NULL
};

int cmd_defrag_start(int argc, char **argv)
{
	int c, pid, ret = 0;
	char *path;
	int fdmnt = -1;
	DIR *dirstream = NULL;
	int do_background = 0;
	int ioprio_class = IOPRIO_CLASS_IDLE;
	int ioprio_classdata = 0;
	struct btrfs_defrag defrag;

	memset(&defrag, 0, sizeof(defrag));

	/* Fill in the default defrag parameters */
	defrag.range.compress_type = BTRFS_COMPRESS_NONE;
	defrag.range.len = (u64)-1;
	defrag.range.extent_thresh = (u32)-1;

	while ((c = getopt(argc, argv, "Bvc:fs:l:t:C:N:")) != -1) {
		switch (c) {
		case 'B':
			do_background = 1;
			break;
		case 'v':
			g_verbose++;
			break;
		case 'c':
			defrag.range.flags |= BTRFS_DEFRAG_RANGE_COMPRESS;
			defrag.range.compress_type = BTRFS_COMPRESS_ZLIB;

			if (!optarg)
				break;

			if (!strcmp(optarg, "zlib"))
				defrag.range.compress_type = BTRFS_COMPRESS_ZLIB;
			else if (!strcmp(optarg, "lzo"))
				defrag.range.compress_type = BTRFS_COMPRESS_LZO;
			else {
				fprintf(stderr, "Unknown compress type %s\n",
					optarg);
				usage(cmd_defrag_start_usage);
			}
			break;
		case 'f':
			defrag.range.flags |= BTRFS_DEFRAG_RANGE_START_IO;
			break;
		case 's':
			defrag.range.start = parse_size(optarg);
			break;
		case 'l':
			defrag.range.len = parse_size(optarg);
			break;
		case 't':
			defrag.range.extent_thresh = parse_size(optarg);
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
			usage(cmd_defrag_start_usage);
		}
	}

	if (check_argc_exact(argc - optind, 1))
		usage(cmd_defrag_start_usage);

	path = argv[optind];

	fdmnt = open_path_or_dev_mnt(path, &dirstream);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: could not open %s: %s\n",
			path, strerror(errno));
		ret = 1;
		goto out;
	}

	if (do_background) {
		pid = fork();
		if (pid == -1) {
			fprintf(stderr, "ERROR: cannot defrag, fork failed: "
				"%s\n", strerror(errno));
			ret = 1;
			goto out;
		}

		if (pid) {
			printf("defrag started at %s\n", path);
			ret = 0;
			goto out;
		}
	}

	/* Populate defrag structure */
	defrag.fdmnt = fdmnt;
	defrag.path = path;

	defrag_handle_sigint(&defrag);

	/* Set the IO priorities before moving on */
	ret = syscall(SYS_ioprio_set, IOPRIO_WHO_PROCESS, 0,
		IOPRIO_PRIO_VALUE(ioprio_class, ioprio_classdata));
	if (ret)
		fprintf(stderr, "WARNING: setting ioprio failed: %s (ignored).\n",
			strerror(errno));

	ret = do_defrag(&defrag);
	if (ret < 0)
		goto out;

	defrag_handle_sigint(NULL);

	ret = 0;
out:
	close_file_or_dir(fdmnt, dirstream);
	return ret;
}

static const char * const cmd_defrag_cancel_usage[] = {
	"btrfs defrag cancel <path|device>",
	"Cancel a running defrag",
	NULL
};

static int cmd_defrag_cancel(int argc, char **argv)
{
	char *path;
	int ret = 0;
	int fdmnt = -1;
	DIR *dirstream = NULL;
	struct btrfs_defrag defrag;

	if (check_argc_exact(argc, 2))
		usage(cmd_defrag_cancel_usage);

	path = argv[1];

	fdmnt = open_path_or_dev_mnt(path, &dirstream);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: could not open %s: %s\n",
			path, strerror(errno));
		ret = 1;
		goto out;
	}

	/* Populate defrag structure */
	defrag.fdmnt = fdmnt;
	defrag.path = path;

	ret = do_cancel(&defrag);
	if (ret)
		goto out;

	printf("defrag cancelled\n");
	ret = 0;
out:
	close_file_or_dir(fdmnt, dirstream);
	return ret;
}

static const char * const cmd_defrag_status_usage[] = {
	"btrfs defrag status <path|device>",
	"Show status of running or finished filesystem defrag",
	NULL
};

static int cmd_defrag_status(int argc, char **argv)
{
	char *path;
	int ret = 0;
	int fdmnt = -1;
	DIR *dirstream = NULL;
	struct btrfs_ioctl_defrag_args da;

	memset(&da, 0, sizeof(da));

	if (check_argc_exact(argc, 2))
		usage(cmd_defrag_status_usage);

	path = argv[1];

	fdmnt = open_path_or_dev_mnt(path, &dirstream);
	if (fdmnt < 0) {
		fprintf(stderr, "ERROR: could not open %s: %s\n",
			path, strerror(errno));
		ret = 1;
		goto out;
	}

	ret = ioctl(fdmnt, BTRFS_IOC_DEFRAG_PROGRESS, &da);
	if (ret < 0) {
		fprintf(stderr, "ERROR: defrag status failed on %s: %s\n",
			path, strerror(errno));
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
	close_file_or_dir(fdmnt, dirstream);
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

int cmd_fs_defrag(int argc, char **argv)
{
	return handle_command_group(&defrag_cmd_group, argc, argv);
}
