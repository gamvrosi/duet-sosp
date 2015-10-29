/*
 * Copyright (C) 2015 George Amvrosiadis.  All rights reserved.
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
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include "ioctl.h"

#define DUET_DEV_NAME   "/dev/duet"

int open_duet_dev(void)
{
	int ret;
	struct stat st;
	int duet_fd = -1;

	ret = stat(DUET_DEV_NAME, &st);
	if (ret < 0) {
		ret = system("modprobe duet");
		if (ret == -1)
			return -1;

		ret = stat(DUET_DEV_NAME, &st);
		if (ret < 0)
			return -1;
	}

	if (S_ISCHR(st.st_mode))
		duet_fd = open(DUET_DEV_NAME, O_RDWR);

	return duet_fd;
}

void close_duet_dev(int duet_fd)
{
	int ret;
	struct stat st;

	ret = stat(DUET_DEV_NAME, &st);
	if (ret < 0)
		return;

	close(duet_fd);
}

int duet_register(int duet_fd, const char *path, __u8 evtmask, __u32 bitrange,
	const char *name, int *tid)
{
	int ret = 0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));

	args.cmd_flags = DUET_REGISTER;
	memcpy(args.name, name, DUET_MAX_NAME);
	args.bitrange = bitrange;
	args.evtmask = evtmask;
	memcpy(args.path, path, DUET_MAX_PATH);

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("duet: tasks register ioctl error");
		goto out;
	}

	*tid = args.tid;

	duet_dbg(stdout, "Task '%s' registered successfully under ID %d.\n",
		args.name, args.tid);

out:
	return ret;
}

int duet_deregister(int duet_fd, int tid)
{
	int ret = 0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEREGISTER;
	args.tid = tid;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: tasks deregister ioctl error");

	duet_dbg(stdout, "Task with ID %d deregistered successfully.\n",
		args.tid);

	return ret;
}

int duet_fetch(int duet_fd, int tid, struct duet_item *items, int *count)
{
	int ret = 0;
	struct duet_ioctl_fetch_args args;

	if (*count > DUET_MAX_ITEMS) {
		fprintf(stderr, "duet: requested too many items (%d > %d)\n",
			*count, DUET_MAX_ITEMS);
		return -1;
	}

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.tid = tid;
	args.num = *count;

	ret = ioctl(duet_fd, DUET_IOC_FETCH, &args);
	if (ret < 0) {
		//perror("duet: fetch ioctl error");
		goto out;
	}

	*count = args.num;
	memcpy(items, args.itm, args.num * sizeof(struct duet_item));

out:
	return ret;
}

int duet_check_done(int duet_fd, int tid, __u64 idx, __u32 count)
{
	int ret = 0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_CHECK_DONE;
	args.tid = tid;
	args.itmidx = idx;
	args.itmnum = count;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: check ioctl error");

	duet_dbg("Blocks [%llu, %llu] in task #%d were %sset.\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid,
		args.ret ? "" : "not ");

	return (ret < 0) ? ret : args.ret;
}

int duet_set_done(int duet_fd, int tid, __u64 idx, __u32 count)
{
	int ret = 0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_SET_DONE;
	args.tid = tid;
	args.itmidx = idx;
	args.itmnum = count;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: mark ioctl error");

	duet_dbg("Added blocks [%llu, %llu] to task #%d (ret = %u)\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid, args.ret);

	return (ret < 0) ? ret : args.ret;
}

int duet_unset_done(int duet_fd, int tid, __u64 idx, __u32 count)
{
	int ret = 0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_UNSET_DONE;
	args.tid = tid;
	args.itmidx = idx;
	args.itmnum = count;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: unmark ioctl error");

	duet_dbg("Removed blocks [%llu, %llu] to task #%d (ret = %u).\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid, args.ret);

	return (ret < 0) ? ret : args.ret;
}

/* Warning: should only be called with a path that's DUET_MAX_PATH or longer */
int duet_get_path(int duet_fd, int tid, unsigned long ino, char *path)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return 1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_GET_PATH;
	args.tid = tid;
	args.c_ino = ino;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("duet: getpath ioctl error");
		goto out;
	}

	if (!args.ret)
		memcpy(path, args.cpath, DUET_MAX_PATH);

out:
	return args.ret || ret;
}

int duet_debug_printbit(int duet_fd, int tid)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_PRINTBIT;
	args.tid = tid;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: printbit ioctl error");

	fprintf(stdout, "Check dmesg for the BitTree of task #%d.\n",
		args.tid);

	return ret;
}

int duet_task_list(int duet_fd)
{
	int i, ret=0;
	struct duet_ioctl_list_args args;

	memset(&args, 0, sizeof(args));

	ret = ioctl(duet_fd, DUET_IOC_TLIST, &args);
	if (ret < 0)
		perror("duet: task list ioctl failed");

	/* Print out the list we received */
	fprintf(stdout, "ID\tTask Name\tBit range\tEvt. mask\n"
			"--\t---------\t---------\t---------\n");
	for (i=0; i<DUET_MAX_TASKS; i++) {
		if (!args.tid[i])
			break;

		fprintf(stdout, "%2d\t%9s\t%9u\t%9u\n",
			args.tid[i], args.tnames[i], args.bitrange[i],
			args.evtmask[i]);
	}

	return ret;
}
