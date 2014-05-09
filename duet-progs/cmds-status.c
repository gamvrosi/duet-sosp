/*
 * Copyright (C) 2012 STRATO.  All rights reserved.
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

#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>

#include "ioctl.h"
#include "commands.h"
#include "utils.h"

static const char * const status_cmd_group_usage[] = {
	"duet status <command>",
	NULL
};

static const char * const cmd_status_start_usage[] = {
	"duet status start",
	"Enable the duet framework.",
	"Initializes and enables the duet framework. Only tasks registered",
	"after running this command will be monitored by the framework.",
	NULL
};

static const char * const cmd_status_stop_usage[] = {
	"duet status stop",
	"Disable the duet framework.",
	"Terminates and cleans up any metadata kept by the duet framework.",
	"Any tasks running will no longer be monitored by the framework,",
	"but will continue to function.",
	NULL
};

static int cmd_status_start(int fd, int argc, char **argv)
{
	int ret = 0;
	struct duet_ioctl_recv_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_STATUS_START;

	ret = ioctl(fd, DUET_IOC_STATUS, args);
	if (ret < 0)
		usage(cmd_status_start_usage);
	return ret;
}

static int cmd_status_stop(int fd, int argc, char **argv)
{
	int ret = 0;
	struct duet_ioctl_recv_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_STATUS_STOP;

	ret = ioctl(fd, DUET_IOC_STATUS, args);
	if (ret < 0)
		usage(cmd_status_stop_usage);

	close_dev(fd);
	ret = system("rmmod duet");
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
