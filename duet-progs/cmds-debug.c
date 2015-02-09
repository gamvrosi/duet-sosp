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

static const char * const cmd_debug_printbit_usage[] = {
	"duet debug printbit [-i taskid]",
	"Prints the BitTree for a task.",
	"Instructs the framework to print the BitTree for the given task.",
	"",
	"-i     the id of the task",
	NULL
};

static const char * const cmd_debug_printitm_usage[] = {
	"duet debug printitm [-i taskid]",
	"Prints the ItemTree for a task.",
	"Instructs the framework to print the ItemTree for the given task.",
	"",
	"-i     the id of the task",
	NULL
};

static int cmd_debug_printbit(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_PRINTBIT;

	optind = 1;
	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_debug_printbit_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_debug_printbit_usage);
		}
	}

	if (!args.tid || argc != optind)
		usage(cmd_debug_printbit_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("debug printrbt ioctl error");
		usage(cmd_debug_printbit_usage);
	}

	fprintf(stdout, "Check dmesg for the BitTree of task #%d.\n",
		args.tid);
	return ret;
}

static int cmd_debug_printitm(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_PRINTITEM;

	optind = 1;
	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.tid = (__u8)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_debug_printbit_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_debug_printbit_usage);
		}
	}

	if (!args.tid || argc != optind)
		usage(cmd_debug_printbit_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("debug printrbt ioctl error");
		usage(cmd_debug_printbit_usage);
	}

	fprintf(stdout, "Check dmesg for the ItemTree of task #%d.\n",
		args.tid);
	return ret;
}

const struct cmd_group debug_cmd_group = {
	debug_cmd_group_usage, NULL, {
		{ "printbit", cmd_debug_printbit, cmd_debug_printbit_usage, NULL, 0 },
		{ "printitm", cmd_debug_printitm, cmd_debug_printitm_usage, NULL, 0 },
	}
};

int cmd_debug(int fd, int argc, char **argv)
{
	return handle_command_group(&debug_cmd_group, fd, argc, argv);
}
