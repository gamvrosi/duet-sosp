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
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include "ioctl.h"
#include "commands.h"

static const char * const tasks_cmd_group_usage[] = {
	"duet tasks <command> [options]",
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

static const char * const cmd_tasks_reg_usage[] = {
	"duet tasks register [-n name]",
	"Registers a new task with the currently active framework. The task",
	"will be assigned an ID, and will be registered under the provided",
	"name. This command is mainly used for debugging purposes.",
	"",
	"-n     name under which to register the task",
	NULL
};

static const char * const cmd_tasks_dereg_usage[] = {
	"duet tasks deregister [-i taskid]",
	"Deregisters an existing task from the currently active framework.",
	"The task is tracked using the given ID. This command is mainly used",
	"for debugging purposes.",
	"",
	"-i     task ID used to find the task",
	NULL
};

static int cmd_tasks_list(int fd, int argc, char **argv)
{
	int i, ret=0;
	struct duet_ioctl_tasks_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_TASKS_LIST;

	ret = ioctl(fd, DUET_IOC_TASKS, &args);
	if (ret < 0) {
		perror("tasks list ioctl error");
		usage(cmd_tasks_list_usage);
	}

	/* Print out the list we received */
	fprintf(stdout, "Task ID\tTask Name\n"
			"-------\t---------\n");
	for (i=0; i<MAX_TASKS; i++) {
		if (!args.taskid[i])
			break;

		fprintf(stdout, "%7d\t%s\n", args.taskid[i],
			args.task_names[i]);
	}

	return ret;
}

static int cmd_tasks_reg(int fd, int argc, char **argv)
{
	int c, len=0, ret=0;
	struct duet_ioctl_tasks_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_TASKS_REGISTER;

	optind = 1;
	while ((c = getopt(argc, argv, "n:")) != -1) {
		switch (c) {
		case 'n':
			len = strnlen(optarg, TASK_NAME_LEN);
			if (len == TASK_NAME_LEN || !len) {
				fprintf(stderr, "Invalid name (%d)\n", len);
				usage(cmd_tasks_reg_usage);
			}

			memcpy(args.task_names[0], optarg, TASK_NAME_LEN);
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_tasks_reg_usage);
		}
	}

	if (!args.task_names[0][0] || argc != optind)
		usage(cmd_tasks_reg_usage);

	ret = ioctl(fd, DUET_IOC_TASKS, &args);
	if (ret < 0) {
		perror("tasks register ioctl error");
		usage(cmd_tasks_reg_usage);
	}

	fprintf(stdout, "Task '%s' registered successfully under ID %d.\n",
		args.task_names[0], args.taskid[0]);
	return ret;
}

static int cmd_tasks_dereg(int fd, int argc, char **argv)
{
	int c, ret=0;
	struct duet_ioctl_tasks_args args;

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_TASKS_DEREGISTER;

	optind = 1;
	while ((c = getopt(argc, argv, "i:")) != -1) {
		switch (c) {
		case 'i':
			errno = 0;
			args.taskid[0] = (int)strtol(optarg, NULL, 10);
			if (errno) {
				perror("strtol: invalid ID");
				usage(cmd_tasks_dereg_usage);
			}
			break;
		default:
			fprintf(stderr, "Unknown option %c\n", (char)c);
			usage(cmd_tasks_dereg_usage);
		}
	}

	if (!args.taskid[0] || argc != optind)
		usage(cmd_tasks_dereg_usage);

	ret = ioctl(fd, DUET_IOC_TASKS, &args);
	if (ret < 0) {
		perror("tasks deregister ioctl error");
		usage(cmd_tasks_dereg_usage);
	}

	fprintf(stdout, "Task with ID %d deregistered successfully.\n",
		args.taskid[0]);
	return ret;
}

const struct cmd_group tasks_cmd_group = {
	tasks_cmd_group_usage, NULL, {
		{ "list", cmd_tasks_list, cmd_tasks_list_usage, NULL, 0 },
		{ "register", cmd_tasks_reg, cmd_tasks_reg_usage, NULL, 0 },
		{ "deregister", cmd_tasks_dereg, cmd_tasks_dereg_usage, NULL, 0 },
	}
};

int cmd_tasks(int fd, int argc, char **argv)
{
	return handle_command_group(&tasks_cmd_group, fd, argc, argv);
}
