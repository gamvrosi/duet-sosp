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

#include "ioctl.h"
#include "commands.h"

static const char * const status_cmd_group_usage[] = {
	"duet status <command>",
	NULL
};

static const char * const cmd_status_start_usage[] = {
	"duet status start [-n tasks]",
	"Enable the duet framework.",
	"Initializes and enables the duet framework. Only tasks registered",
	"after running this command will be monitored by the framework.",
	"Ensure the framework is off, otherwise this command will fail.",
	"",
	"-n	max number of concurrently running tasks (default: 8)",
	NULL
};

static const char * const cmd_status_stop_usage[] = {
	"duet status stop",
	"Disable the duet framework.",
	"Terminates and cleans up any metadata kept by the duet framework.",
	"Any tasks running will no longer be monitored by the framework,",
	"but will continue to function. Ensure the framework is on,",
	"otherwise this command will fail.",
	NULL
};

static int cmd_status_start(int fd, int argc, char **argv)
{
	int c, ret = 0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_START;

	optind = 1;
	while ((c = getopt(argc, argv, "n:")) != -1) {
		switch (c) {
		case 'n':
			errno = 0;
			args.numtasks = (__u8)strtoul(optarg, NULL, 10);
			if (errno) {
				perror("strtoul: invalid number of tasks");
				usage(cmd_status_start_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_status_start_usage);
		}
	}

	if (argc != optind)
		usage(cmd_status_start_usage);

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("status start ioctl error");
		usage(cmd_status_start_usage);
	}
	return ret;
}

static int cmd_status_stop(int fd, int argc, char **argv)
{
	int ret = 0;
	struct duet_ioctl_cmd_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_STOP;

	ret = ioctl(fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("status stop ioctl error");
		usage(cmd_status_stop_usage);
	}

	//close_dev(fd);
	//ret = system("rmmod duet");
	return ret;
}

const struct cmd_group status_cmd_group = {
	status_cmd_group_usage, NULL, {
		{ "start", cmd_status_start, cmd_status_start_usage, NULL, 0 },
		{ "stop", cmd_status_stop, cmd_status_stop_usage, NULL, 0 },
	}
};

int cmd_status(int fd, int argc, char **argv)
{
	return handle_command_group(&status_cmd_group, fd, argc, argv);
}
