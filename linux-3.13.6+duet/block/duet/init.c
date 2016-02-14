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

#include <linux/module.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include "common.h"

#define DUET_DEVNAME "duet"

struct file_operations duet_fops = {
	.owner =		THIS_MODULE,
	.unlocked_ioctl =	duet_ioctl,
};

/* Duet globals */
dev_t duet_dev;
struct cdev *duet_cdev = NULL;
struct class *duet_class = NULL;
struct duet_info duet_env;

static int duet_create_chrdev(void)
{
	int ret;
	struct device *devcp = NULL;

	/* Dynamically register the character device */
	ret = alloc_chrdev_region(&duet_dev, 0, 1, DUET_DEVNAME);
	if (ret < 0) {
		printk(KERN_WARNING "duet: character device alloc failed\n");
		return ret;
	}

	/* Create device class */
	duet_class = class_create(THIS_MODULE, DUET_DEVNAME);
	if (!duet_class) {
		printk(KERN_WARNING "duet: failed to create device class\n");
		goto bail_reg;
	}

	devcp = device_create(duet_class, NULL, duet_dev, NULL, DUET_DEVNAME);
	if (!devcp) {
		printk(KERN_WARNING "duet: failed to create device node\n");
		goto bail_cl;
	}

	duet_cdev = cdev_alloc();
	if (!duet_cdev) {
		printk(KERN_WARNING "duet: failed to alloc char device\n");
		goto bail_dev;
	}

	cdev_init(duet_cdev, &duet_fops);
	duet_cdev->owner = THIS_MODULE;
	ret = cdev_add(duet_cdev, duet_dev, 1);
	if (ret) {
		printk(KERN_WARNING "duet: failed to add character device\n");
		goto bail_alloc;
	}

	return 0;

bail_alloc:
	kfree(duet_cdev);
bail_dev:
	device_destroy(duet_class, duet_dev);
bail_cl:
	class_destroy(duet_class);
bail_reg:
	unregister_chrdev_region(duet_dev, 1);
	return -1;
}

static void duet_destroy_chrdev(void)
{
	/* Remove the character device */
	cdev_del(duet_cdev);
	kfree(duet_cdev);
	device_destroy(duet_class, duet_dev);
	class_destroy(duet_class);

	/* Unregister the character device */
	unregister_chrdev_region(duet_dev, 1);
}

static int __init duet_init(void)
{
	int ret;

	/*
	 * This is the first and only time we'll zero out duet_env.
	 * After the character device is online, we need synchronization
	 * primitives to touch it.
	 */
	memset(&duet_env, 0, sizeof(duet_env));
	atomic_set(&duet_env.status, DUET_STATUS_OFF);

	ret = duet_create_chrdev();
	if (ret)
		return ret;

	printk(KERN_INFO "Duet device initialized successfully.\n");
	return 0;
}

static void __exit duet_exit(void)
{
	duet_shutdown();
	duet_destroy_chrdev();
	printk(KERN_INFO "Duet terminated successfully.\n");
}

MODULE_AUTHOR("George Amvrosiadis <gamvrosi@gmail.com>");
MODULE_DESCRIPTION("Duet framework module");
MODULE_LICENSE("GPL");

module_init(duet_init);
module_exit(duet_exit);
