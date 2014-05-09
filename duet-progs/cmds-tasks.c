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
//#include "utils.h"

static const char * const tasks_cmd_group_usage[] = {
	"duet tasks <command>",
	NULL
};

static const char * const cmd_tasks_list_usage[] = {
	"duet tasks list",
	"List tasks registered with the duet framework.",
	"Requests and prints a list of all the tasks that are currently",
	"registered with the duet framework. For each task, we print the",
	"name with which it was registered.",
	NULL
};

static int cmd_tasks_list(int fd, int argc, char **argv)
{
	int ret = 0;
	struct duet_ioctl_send_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_TASKS_LIST;

	ret = ioctl(fd, DUET_IOC_TASKS, args);
	if (ret < 0)
		usage(cmd_tasks_list_usage);
	return ret;
}

const struct cmd_group tasks_cmd_group = {
	tasks_cmd_group_usage, NULL, {
		{ "list", cmd_tasks_list, cmd_tasks_list_usage, NULL, 0 },
	}
};

int cmd_tasks(int fd, int argc, char **argv)
{
	return handle_command_group(&tasks_cmd_group, fd, argc, argv);
}
