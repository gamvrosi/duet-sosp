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
	int fd = -1;

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
		fd = open(DUET_DEV_NAME, O_RDWR);

	return fd;
}

void close_duet_dev(int fd)
{
	int ret;
	struct stat st;

	ret = stat(DUET_DEV_NAME, &st);
	if (ret < 0)
		return;

	close(fd);
}

int duet_register(__u8 *tid, int duet_fd, const char *name, __u32 bitrange,
	__u8 evtmask, const char *path)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));

	args.cmd_flags = DUET_REGISTER;
	memcpy(args.name, name, MAX_NAME);
	args.bitrange = bitrange;
	args.evtmask = evtmask;
	memcpy(args.path, path, MAX_PATH);

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

int duet_deregister(int taskid, int duet_fd)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_DEREGISTER;
	args.tid = taskid;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: tasks deregister ioctl error");

	duet_dbg(stdout, "Task with ID %d deregistered successfully.\n",
		args.tid);

	return ret;
}

int duet_fetch(int taskid, int duet_fd, int itmreq, struct duet_item *items, int *num)
{
	int ret=0;
	struct duet_ioctl_fetch_args args;

	if (itmreq > MAX_ITEMS) {
		fprintf(stderr, "duet: requested too many items (%d < %d)\n",
			itmreq, MAX_ITEMS);
		return -1;
	}

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.tid = taskid;
	args.num = itmreq;

	ret = ioctl(duet_fd, DUET_IOC_FETCH, &args);
	if (ret < 0) {
		perror("duet: fetch ioctl error");
		goto out;
	}

	*num = args.num;
	memcpy(items, args.itm, args.num * sizeof(struct duet_item));

out:
	return ret;
}

/* Warning: should only be called with a path that's MAX_PATH or longer */
int duet_getpath(int taskid, int duet_fd, unsigned long ino, char *path)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return 1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_GETPATH;
	args.tid = taskid;
	args.c_ino = ino;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0) {
		perror("duet: getpath ioctl error");
		goto out;
	}

	if (!args.ret)
		memcpy(path, args.cpath, MAX_PATH);

out:
	return args.ret || ret;
}

int duet_mark(int taskid, int duet_fd, __u64 idx, __u32 num)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_MARK;
	args.tid = taskid;
	args.itmidx = idx;
	args.itmnum = num;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: mark ioctl error");

	duet_dbg("Successfully added blocks [%llu, %llu] to task #%d (ret = %u).\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid, args.ret);

	return (ret < 0) ? ret : args.ret;
}

int duet_unmark(int taskid, int duet_fd, __u64 idx, __u32 num)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_UNMARK;
	args.tid = taskid;
	args.itmidx = idx;
	args.itmnum = num;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: unmark ioctl error");

	duet_dbg("Successfully removed blocks [%llu, %llu] to task #%d (ret = %u).\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid, args.ret);

	return (ret < 0) ? ret : args.ret;
}

int duet_check(int taskid, int duet_fd, __u64 idx, __u32 num)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_CHECK;
	args.tid = taskid;
	args.itmidx = idx;
	args.itmnum = num;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: check ioctl error");

	duet_dbg("Blocks [%llu, %llu] in task #%d were %sset.\n",
		args.itmidx, args.itmidx + args.itmnum, args.tid,
		args.ret ? "" : "not ");

	return (ret < 0) ? ret : args.ret;
}

int duet_debug_printbit(int taskid, int duet_fd)
{
	int ret=0;
	struct duet_ioctl_cmd_args args;

	if (duet_fd == -1) {
		fprintf(stderr, "duet: failed to open duet device\n");
		return -1;
	}

	memset(&args, 0, sizeof(args));
	args.cmd_flags = DUET_PRINTBIT;
	args.tid = taskid;

	ret = ioctl(duet_fd, DUET_IOC_CMD, &args);
	if (ret < 0)
		perror("duet: printbit ioctl error");

	fprintf(stdout, "Check dmesg for the BitTree of task #%d.\n",
		args.tid);

	return ret;
}
