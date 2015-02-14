/*
 * Copyright (C) 2014-2015 George Amvrosiadis.  All rights reserved.
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
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "ioctl.h"
#include "commands.h"

static const char * const task_cmd_group_usage[] = {
	"duet task <command> [options]",
	NULL
};

static const char * const cmd_task_list_usage[] = {
	"duet task list",
	"List tasks registered with the duet framework.",
	"Requests and prints a list of all the tasks that are currently",
	"registered with the duet framework. For each task, we print the",
	"name with which it was registered.",
	NULL
};

static const char * const cmd_task_fetch_usage[] = {
	"duet task fetch [-i taskid] [-n num]",
	"Fetched up to num items for task with ID taskid, and prints them.",
	"",
	"-i	task ID used to find the task",
	"-n	number of events, up to MAX_ITEMS (check ioctl.h)",
	NULL
};

static const char * const cmd_task_reg_usage[] = {
	"duet task register [-n name] [-b bitrange] [-m nmodel]",
	"Registers a new task with the currently active framework. The task",
	"will be assigned an ID, and will be registered under the provided",
	"name. The bitmaps that keep information on what has been processed",
	"can be customized to store a given range of numbers per bit. The",
	"notification model must also be specified as one from:",
	"'add', 'rem', 'both', 'diff', 'axs'.",
	"",
	"-n     name under which to register the task",
	"-b     range of items/bytes per bitmap bit",
	"-m     notification model for task",
	NULL
};

static const char * const cmd_task_dereg_usage[] = {
	"duet task deregister [-i taskid]",
	"Deregisters an existing task from the currently active framework.",
	"The task is tracked using the given ID. This command is mainly used",
	"for debugging purposes.",
	"",
	"-i     task ID used to find the task",
	NULL
};

static const char * const cmd_task_mark_usage[] = {
	"duet task mark [-i id] [-o offset] [-l len]",
	"Marks a block range for a specific task.",
	"Finds and marks the given block range (in bytes), in the bitmaps",
	"of the task with the given id.",
	"",
	"-i     the id of the task",
	"-o     the offset denoting the beginning of the range in bytes",
	"-l     the number of bytes denoting the length of the range",
	NULL
};

static const char * const cmd_task_unmark_usage[] = {
	"duet task unmark [-i id] [-o offset] [-l len]",
	"Unmarks a block range for a specific task.",
	"Finds and unmarks the given block range (in bytes), in the bitmaps",
	"of the task wit the given id.",
	"",
	"-i     the id of the task",
	"-o     the offset denoting the beginning of the range in bytes",
	"-l     the number of bytes denoting the length of the range",
	NULL
};

static const char * const cmd_task_check_usage[] = {
	"duet task check [-i id] [-o offset] [-l len]",
	"Checks if a block range for a specific task is marked or not.",
	"Finds and checks if the given block range (in bytes) is marked or not",
	"in the bitmaps of the task wit the given id.",
	"",
	"-i     the id of the task",
	"-o     the offset denoting the beginning of the range in bytes",
	"-l     the number of bytes denoting the length of the range",
	NULL
};

static int cmd_task_fetch(int fd, int argc, char **argv)
{
	int i, c, ret=0;
	struct duet_ioctl_fetch_args args;

	memset(&args, 0, sizeof(args));

	optind = 1;
	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (int)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_task_fetch_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_fetch_usage);
		}
	}

	if (!args.tid || argc != optind)
		usage(cmd_task_fetch_usage);

	ret = ioctl(fd, DUET_IOC_FETCH, &args);
	if (ret < 0) {
		perror("tasks list ioctl error");
		usage(cmd_task_fetch_usage);
	}

	if (args.num == 0) {
		fprintf(stdout, "Received no items.\n");
		return ret;
	}

	/* Print out the list we received */
	fprintf(stdout, "Inode number\tOffset      \tEvent   \n"
			"------------\t------------\t--------\n");
	for (i=0; i<args.num; i++) {
		fprintf(stdout, "%12lu\t%12lu\t%8x\n",
			args.itm[i].ino, args.itm[i].idx << 12,
			args.itm[i].evt);
	}

	return ret;
}

static int cmd_task_list(int fd, int argc, char **argv)
{
	int i, ret=0;
	struct duet_ioctl_list_args args;

	memset(&args, 0, sizeof(args));

	ret = ioctl(fd, DUET_IOC_TLIST, &args);
	if (ret < 0) {
		perror("tasks list ioctl error");
		usage(cmd_task_list_usage);
	}

	/* Print out the list we received */
	fprintf(stdout, "ID\tTask Name\tBit range\tNot. model\n"
			"--\t---------\t---------\t----------\n");
	for (i=0; i<MAX_TASKS; i++) {
		if (!args.tid[i])
			break;

		fprintf(stdout, "%2d\t%9s\t%9u\t%10u\n",
			args.tid[i], args.tnames[i], args.bitrange[i],
			args.nmodel[i]);
	}

	return ret;
}

static int cmd_task_reg(int fd, int argc, char **argv)
{
	int c, len=0, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_REGISTER;

	optind = 1;
	while ((c = getopt(argc, argv, "n:b:m:")) != -1) {
		switch (c) {
		case 'n':
			len = strnlen(optarg, MAX_NAME);
			if (len == MAX_NAME || !len) {
				fprintf(stderr, "Invalid name (%d)\n", len);
				usage(cmd_task_reg_usage);
			}

			memcpy(args.name, optarg, MAX_NAME);
			break;
		case 'b':
			errno = 0;
			args.bitrange = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid block size");
				usage(cmd_task_reg_usage);
			}
			break;
		case 'm':
			if (!strncmp(optarg, "add", 3))
				args.nmodel = MODEL_ADD;
			else if (!strncmp(optarg, "rem", 3))
				args.nmodel = MODEL_REM;
			else if (!strncmp(optarg, "both", 4))
				args.nmodel = MODEL_BOTH;
			else if (!strncmp(optarg, "diff", 4))
				args.nmodel = MODEL_DIFF;
			else if (!strncmp(optarg, "axs", 3))
				args.nmodel = MODEL_AXS;
			else {
				fprintf(stderr, "error: invalid model '%s'\n",
					optarg);
				usage(cmd_task_reg_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_reg_usage);
		}
	}

	if (!args.name[0] || argc != optind)
		usage(cmd_task_reg_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("tasks register ioctl error");
		usage(cmd_task_reg_usage);
	}

	fprintf(stdout, "Task '%s' registered successfully under ID %d.\n",
		args.name, args.tid);
	return ret;
}

static int cmd_task_dereg(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEREGISTER;

	optind = 1;
	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (int)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_task_dereg_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_dereg_usage);
		}
	}

	if (!args.tid || argc != optind)
		usage(cmd_task_dereg_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("tasks deregister ioctl error");
		usage(cmd_task_dereg_usage);
	}

	fprintf(stdout, "Task with ID %d deregistered successfully.\n",
		args.tid);
	return ret;
}

static int cmd_task_mark(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_MARK;

	optind = 1;
	while ((c = getopt(argc, argv, "i:o:l:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_task_mark_usage);
			}
			break;
		case 'o':
			errno = 0;
			args.itmidx = (__u64)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid offset");
				usage(cmd_task_mark_usage);
			}
			break;
		case 'l':
			errno = 0;
			args.itmnum = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid length");
				usage(cmd_task_mark_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_mark_usage);
		}
	}

	if (!args.tid || !args.itmnum || argc != optind)
		usage(cmd_task_mark_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("debug addblk ioctl error");
		usage(cmd_task_mark_usage);
	}

	fprintf(stdout, "Successfully added blocks [%llu, %llu] to task #%d.\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid);
	return ret;
}

static int cmd_task_unmark(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_UNMARK;

	optind = 1;
	while ((c = getopt(argc, argv, "i:o:l:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_task_unmark_usage);
			}
			break;
		case 'o':
			errno = 0;
			args.itmidx = (__u64)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid offset");
				usage(cmd_task_unmark_usage);
			}
			break;
		case 'l':
			errno = 0;
			args.itmnum = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid length");
				usage(cmd_task_unmark_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_unmark_usage);
		}
	}

	if (!args.tid || !args.itmnum || argc != optind)
		usage(cmd_task_unmark_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("debug rmblk ioctl error");
		usage(cmd_task_unmark_usage);
	}

	fprintf(stdout, "Successfully removed blocks [%llu, %llu] to task #%d.\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid);
	return ret;
}

static int cmd_task_check(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_CHECK;

	optind = 1;
	while ((c = getopt(argc, argv, "i:o:l:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_task_check_usage);
			}
			break;
		case 'o':
			errno = 0;
			args.itmidx = (__u64)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid offset");
				usage(cmd_task_check_usage);
			}
			break;
		case 'l':
			errno = 0;
			args.itmnum = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid length");
				usage(cmd_task_check_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_check_usage);
		}
	}

	if (!args.tid || !args.itmnum || argc != optind)
		usage(cmd_task_check_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("debug chkblk ioctl error");
		usage(cmd_task_check_usage);
	}

	fprintf(stdout, "Blocks [%llu, %llu] in task #%d were %sset.\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid,
		args.ret ? "not " : " ");
	return ret;
}

const struct cmd_group task_cmd_group = {
	task_cmd_group_usage, NULL, {
		{ "list", cmd_task_list, cmd_task_list_usage, NULL, 0 },
		{ "register", cmd_task_reg, cmd_task_reg_usage, NULL, 0 },
		{ "deregister", cmd_task_dereg, cmd_task_dereg_usage, NULL, 0 },
		{ "mark", cmd_task_mark, cmd_task_mark_usage, NULL, 0 },
		{ "unmark", cmd_task_unmark, cmd_task_unmark_usage, NULL, 0 },
		{ "check", cmd_task_check, cmd_task_check_usage, NULL, 0 },
		{ "fetch", cmd_task_fetch, cmd_task_fetch_usage, NULL, 0 },
	}
};

int cmd_task(int fd, int argc, char **argv)
{
	return handle_command_group(&task_cmd_group, fd, argc, argv);
}
