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

static const char * const cmd_task_reg_usage[] = {
	"duet task register [-n name] [-b bitrange] [-t itemtype] [-h mask]",
	"Registers a new task with the currently active framework. The task",
	"will be assigned an ID, and will be registered under the provided",
	"name. The bitmaps that keep information on what has been processed",
	"can be customized with a specific block size per bit, and a specific",
	"size for each bitmap kept. Small (but not too small) bitmaps can",
	"save space by being omitted when not needed. The default echo event",
	"handler will be used for the task, with the event type mask provided",
	"This command is mainly used for debugging purposes.",
	"",
	"-n     name under which to register the task",
	"-b     range of items/bytes per bitmap bit",
	"-t     type of items expected by task (e.g. 'block', 'inode', 'page')",
	"-h     event type mask describing the codes wired to the handler",
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
	fprintf(stdout, "ID\tTask Name\tBit range\tBmap size\tEvent mask\tItem type\n"
			"--\t---------\t---------\t---------\t----------\t---------\n");
	for (i=0; i<MAX_TASKS; i++) {
		if (!args.tid[i])
			break;

		fprintf(stdout, "%2d\t%9s\t%9u\t%9u\t  %08x\t%9u\n",
			args.tid[i], args.tnames[i], args.bitrange[i],
			args.bmapsize[i], args.evtmask[i], args.itmtype[i]);
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
	while ((c = getopt(argc, argv, "n:b:t:h:")) != -1) {
		switch (c) {
		case 'n':
			len = strnlen(optarg, TASK_NAME_LEN);
			if (len == TASK_NAME_LEN || !len) {
				fprintf(stderr, "Invalid name (%d)\n", len);
				usage(cmd_task_reg_usage);
			}

			memcpy(args.tname, optarg, TASK_NAME_LEN);
			break;
		case 'b':
			errno = 0;
			args.bitrange = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid block size");
				usage(cmd_task_reg_usage);
			}
			break;
		case 't':
			if (!strncmp(optarg, "block", 5))
				args.itmtype = DUET_ITM_BLOCK;
			else if (!strncmp(optarg, "inode", 5))
				args.itmtype = DUET_ITM_INODE;
			else if (!strncmp(optarg, "page", 4))
				args.itmtype = DUET_ITM_PAGE;
			else {
				fprintf(stderr, "error: invalid item type '%s'\n",
					optarg);
				usage(cmd_task_reg_usage);
			}
			break;
		case 'h':
			errno = 0;
			args.evtmask = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid event mask");
				usage(cmd_task_reg_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_task_reg_usage);
		}
	}

	if (!args.tname[0] || argc != optind)
		usage(cmd_task_reg_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("tasks register ioctl error");
		usage(cmd_task_reg_usage);
	}

	fprintf(stdout, "Task '%s' registered successfully under ID %d.\n",
		args.tname, args.tid);
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
	}
};

int cmd_task(int fd, int argc, char **argv)
{
	return handle_command_group(&task_cmd_group, fd, argc, argv);
}
