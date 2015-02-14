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
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/duet.h>
#include "ioctl.h"

int duet_online(void)
{
	return (atomic_read(&duet_env.status) == DUET_STATUS_ON);
}
EXPORT_SYMBOL_GPL(duet_online);

int duet_bootstrap(void)
{
	if (atomic_cmpxchg(&duet_env.status, DUET_STATUS_OFF, DUET_STATUS_INIT)
	    != DUET_STATUS_OFF) {
		printk(KERN_WARNING "duet: framework not off, bootstrap aborted\n");
		return 1;
	}

	INIT_LIST_HEAD(&duet_env.tasks);
	mutex_init(&duet_env.task_list_mutex);
	atomic_set(&duet_env.status, DUET_STATUS_ON);

	rcu_assign_pointer(duet_hook_cache_fp, duet_hook);
	synchronize_rcu();
	return 0;
}

int duet_shutdown(void)
{
	struct duet_task *task;

	if (atomic_cmpxchg(&duet_env.status, DUET_STATUS_ON, DUET_STATUS_CLEAN)
	    != DUET_STATUS_ON) {
		printk(KERN_WARNING "duet: framework off, shutdown aborted\n");
		return 1;
	}

	rcu_assign_pointer(duet_hook_cache_fp, NULL);
	synchronize_rcu();

	/* Remove all tasks */
	mutex_lock(&duet_env.task_list_mutex);
	while (!list_empty(&duet_env.tasks)) {
		task = list_entry_rcu(duet_env.tasks.next, struct duet_task,
			task_list);
		list_del_rcu(&task->task_list);
		mutex_unlock(&duet_env.task_list_mutex);

		/* Make sure everyone's let go before we free it */
		synchronize_rcu();
		wait_event(task->cleaner_queue,
			atomic_read(&task->refcount) == 0);
		duet_task_dispose(task);

		mutex_lock(&duet_env.task_list_mutex);
	}
	mutex_unlock(&duet_env.task_list_mutex);

	INIT_LIST_HEAD(&duet_env.tasks);
	mutex_destroy(&duet_env.task_list_mutex);
	atomic_set(&duet_env.status, DUET_STATUS_OFF);
	return 0;
}

static int duet_ioctl_fetch(void __user *arg)
{
	struct duet_ioctl_fetch_args *fa;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	fa = memdup_user(arg, sizeof(*fa));
	if (IS_ERR(fa))
		return PTR_ERR(fa);

	if (fa->num > MAX_ITEMS)
		fa->num = MAX_ITEMS;

	if (duet_fetch(fa->tid, fa->num, fa->itm, &fa->num)) {
		printk(KERN_ERR "duet: failed to fetch for user\n");
		goto err;
	}

	if (copy_to_user(arg, fa, sizeof(*fa))) {
		printk(KERN_ERR "duet: failed to copy out args\n");
		goto err;
	}

	kfree(fa);
	return 0;

err:
	kfree(fa);
	return -EINVAL;
}

static int duet_ioctl_cmd(void __user *arg)
{
	struct duet_ioctl_cmd_args *ca;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	ca = memdup_user(arg, sizeof(*ca));
	if (IS_ERR(ca))
		return PTR_ERR(ca);

	/* If we're in the process of cleaning up, no ioctls (other
	 * than the one that switches duet on/off) are allowed */
	if (atomic_read(&duet_env.status) != DUET_STATUS_ON &&
	    ca->cmd_flags != DUET_START && ca->cmd_flags != DUET_STOP) {
		printk(KERN_INFO "duet: ioctl rejected - duet is offline\n");
		goto err;
	}

	switch (ca->cmd_flags) {
	case DUET_START:
		ca->ret = duet_bootstrap();

		if (ca->ret)
			printk(KERN_ERR "duet: failed to enable framework\n");
		else
			printk(KERN_INFO "duet: framework enabled\n");

		break;

	case DUET_STOP:
		ca->ret = duet_shutdown();

		if (ca->ret)
			printk(KERN_ERR "duet: failed to disable framework\n");
		else
			printk(KERN_INFO "duet: framework disabled\n");

		break;

	case DUET_REGISTER:
		/* XXX: We do not pass an owner for this one. Future work. */
		ca->ret = duet_register(&ca->tid, ca->name, ca->nmodel,
					ca->bitrange, NULL);
		break;

	case DUET_DEREGISTER:
		ca->ret = duet_deregister(ca->tid);
		break;

	case DUET_MARK:
		ca->ret = duet_mark(ca->tid, ca->itmidx, ca->itmnum);
		break;

	case DUET_UNMARK:
		ca->ret = duet_unmark(ca->tid, ca->itmidx, ca->itmnum);
		break;

	case DUET_CHECK:
		ca->ret = duet_check(ca->tid, ca->itmidx, ca->itmnum);
		break;

	case DUET_PRINTBIT:
		ca->ret = duet_print_bittree(ca->tid);
		break;

	case DUET_PRINTITEM:
		ca->ret = duet_print_itmtree(ca->tid);
		break;

	default:
		printk(KERN_INFO "duet: unknown tasks command received\n");
		goto err;
		break;
	}

	if (copy_to_user(arg, ca, sizeof(*ca))) {
		printk(KERN_ERR "duet: failed to copy out args\n");
		goto err;
	}

	kfree(ca);
	return 0;

err:
	kfree(ca);
	return -EINVAL;
}

static int duet_ioctl_tlist(void __user *arg)
{
	int i=0;
	struct duet_task *cur;
	struct duet_ioctl_list_args *la;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	la = memdup_user(arg, sizeof(*la));
	if (IS_ERR(la))
		return PTR_ERR(la);

	/* We will only send the first MAX_TASKS, and that's ok */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		la->tid[i] = cur->id;
		memcpy(la->tnames[i], cur->name, MAX_NAME);
		la->bitrange[i] = cur->bitrange;
		la->nmodel[i] = cur->nmodel;
		i++;
		if (i == MAX_TASKS)
			break;
        }
        rcu_read_unlock();

	if (copy_to_user(arg, la, sizeof(*la))) {
		printk(KERN_ERR "duet: failed to copy out tasklist\n");
		kfree(la);
		return -EINVAL;
	}

	duet_dbg(KERN_INFO "duet: task list sent\n");

	kfree(la);
	return 0;
}

/* 
 * ioctl handler function; passes control to the proper handling function
 * for the ioctl received.
 */
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	/* If we're in the process of cleaning up, no ioctls (other
	 * than the one that switches duet on/off) are allowed */
	if (atomic_read(&duet_env.status) != DUET_STATUS_ON &&
	    cmd != DUET_IOC_CMD) {
		printk(KERN_INFO "duet: ioctl rejected - duet is offline\n");
		return -EINVAL;
	}

	switch (cmd) {
	case DUET_IOC_CMD:
		return duet_ioctl_cmd(argp);
	case DUET_IOC_TLIST:
		return duet_ioctl_tlist(argp);
	case DUET_IOC_FETCH:
		return duet_ioctl_fetch(argp);
	}

	return -EINVAL;
}
