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

#include "duet.h"
#include "ioctl.h"

static int duet_ioctl_status(void __user *arg)
{
	printk(KERN_INFO "duet: received status ioctl\n");
	return 0;
}

static int duet_ioctl_tasks(void __user *arg)
{
	printk(KERN_INFO "duet: received tasks ioctl\n");
	return 0;
}

/* 
 * ioctl handler function; passes control to the proper handling function
 * for the ioctl received.
 */
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case DUET_IOC_STATUS:
		return duet_ioctl_status(argp);
	case DUET_IOC_TASKS:
		return duet_ioctl_tasks(argp);
	}

	return -ENOTTY;
}
