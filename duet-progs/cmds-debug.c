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
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "ioctl.h"
#include "commands.h"

static const char * const debug_cmd_group_usage[] = {
	"duet debug <command> [options]",
	NULL
};

static const char * const cmd_debug_addblk_usage[] = {
	"duet debug addblk [-i id] [-o offset] [-l len]",
	"Marks a block range for a specific task.",
	"Finds and marks the given block range (in bytes), in the bitmaps",
	"of the task with the given id.",
	"",
	"-i     the id of the task",
	"-o     the offset denoting the beginning of the range in bytes",
	"-l     the number of bytes denoting the length of the range",
	NULL
};

static const char * const cmd_debug_rmblk_usage[] = {
	"duet debug rmblk [-i id] [-o offset] [-l len]",
	"Unmarks a block range for a specific task.",
	"Finds and unmarks the given block range (in bytes), in the bitmaps",
	"of the task wit the given id.",
	"",
	"-i     the id of the task",
	"-o     the offset denoting the beginning of the range in bytes",
	"-l     the number of bytes denoting the length of the range",
	NULL
};

static const char * const cmd_debug_chkblk_usage[] = {
	"duet debug chkblk [-i id] [-o offset] [-l len] [-z]",
	"Checks if a block range for a specific task is marked or not.",
	"Finds and checks if the given block range (in bytes) is marked or not",
	"in the bitmaps of the task wit the given id.",
	"",
	"-i     the id of the task",
	"-o     the offset denoting the beginning of the range in bytes",
	"-l     the number of bytes denoting the length of the range",
	"-z     if specified, we check if the range is unmarked",
	NULL
};

static const char * const cmd_debug_printrbt_usage[] = {
	"duet debug printrbt [-i taskid]",
	"Prints a summary of the red-black bitmap tree for a task.",
	"Instructs the framework to print a summary of the red-black bitmap",
	"tree for the given task."
	"",
	"-i     the id of the task",
	NULL
};

static int cmd_debug_addblk(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_debug_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEBUG_ADDBLK;

	optind = 1;
	while ((c = getopt(argc, argv, "i:o:l:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.taskid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_debug_addblk_usage);
			}
			break;
		case 'o':
			errno = 0;
			args.offset = (__u64)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid offset");
				usage(cmd_debug_addblk_usage);
			}
			break;
		case 'l':
			errno = 0;
			args.len = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid length");
				usage(cmd_debug_addblk_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_debug_addblk_usage);
		}
	}

	if (!args.taskid || !args.len || argc != optind)
		usage(cmd_debug_addblk_usage);

	ret = ioctl(fd, DUET_IOC_DEBUG, &args);
	if (ret < 0) {
		perror("debug addblk ioctl error");
		usage(cmd_debug_addblk_usage);
	}

	fprintf(stdout, "Successfully added blocks [%llu, %llu] to task #%d.\n",
		args.offset, args.offset + args.len, args.taskid);
	return ret;
}

static int cmd_debug_rmblk(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_debug_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEBUG_RMBLK;

	optind = 1;
	while ((c = getopt(argc, argv, "i:o:l:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.taskid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_debug_rmblk_usage);
			}
			break;
		case 'o':
			errno = 0;
			args.offset = (__u64)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid offset");
				usage(cmd_debug_rmblk_usage);
			}
			break;
		case 'l':
			errno = 0;
			args.len = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid length");
				usage(cmd_debug_rmblk_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_debug_rmblk_usage);
		}
	}

	if (!args.taskid || !args.len || argc != optind)
		usage(cmd_debug_rmblk_usage);

	ret = ioctl(fd, DUET_IOC_DEBUG, &args);
	if (ret < 0) {
		perror("debug rmblk ioctl error");
		usage(cmd_debug_rmblk_usage);
	}

	fprintf(stdout, "Successfully removed blocks [%llu, %llu] to task #%d.\n",
		args.offset, args.offset + args.len, args.taskid);
	return ret;
}

static int cmd_debug_chkblk(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_debug_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEBUG_CHKBLK;

	optind = 1;
	while ((c = getopt(argc, argv, "i:o:l:z")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.taskid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_debug_chkblk_usage);
			}
			break;
		case 'o':
			errno = 0;
			args.offset = (__u64)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtoll: invalid offset");
				usage(cmd_debug_chkblk_usage);
			}
			break;
		case 'l':
			errno = 0;
			args.len = (__u32)strtoll(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid length");
				usage(cmd_debug_chkblk_usage);
			}
			break;
		case 'z':
			args.unset = 1;
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_debug_chkblk_usage);
		}
	}

	if (!args.taskid || !args.len || argc != optind)
		usage(cmd_debug_chkblk_usage);

	ret = ioctl(fd, DUET_IOC_DEBUG, &args);
	if (ret < 0) {
		perror("debug chkblk ioctl error");
		usage(cmd_debug_chkblk_usage);
	}

	fprintf(stdout, "Blocks [%llu, %llu] in task #%d were %s%s.\n",
		args.offset, args.offset + args.len, args.taskid,
		args.ret ? "not " : " ", args.unset ? "unset" : "set");
	return ret;
}

static int cmd_debug_printrbt(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_debug_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEBUG_PRINTRBT;

	optind = 1;
	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.taskid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_debug_printrbt_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_debug_printrbt_usage);
		}
	}

	if (!args.taskid || argc != optind)
		usage(cmd_debug_printrbt_usage);

	ret = ioctl(fd, DUET_IOC_DEBUG, &args);
	if (ret < 0) {
		perror("debug printrbt ioctl error");
		usage(cmd_debug_printrbt_usage);
	}

	fprintf(stdout, "Check dmesg for the red-black bitmap tree of task #%d.\n",
		args.taskid);
	return ret;
}

const struct cmd_group debug_cmd_group = {
	debug_cmd_group_usage, NULL, {
		{ "addblk", cmd_debug_addblk, cmd_debug_addblk_usage, NULL, 0 },
		{ "rmblk", cmd_debug_rmblk, cmd_debug_rmblk_usage, NULL, 0 },
		{ "chkblk", cmd_debug_chkblk, cmd_debug_chkblk_usage, NULL, 0 },
		{ "printrbt", cmd_debug_printrbt, cmd_debug_printrbt_usage, NULL, 0 },
	}
};

int cmd_debug(int fd, int argc, char **argv)
{
	return handle_command_group(&debug_cmd_group, fd, argc, argv);
}
