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
#include <linux/slab.h>
#include <linux/uaccess.h>
#include "duet.h"
#include "ioctl.h"

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
	return 0;
}

int duet_shutdown(void)
{
	struct duet_task *task;

	if (atomic_cmpxchg(&duet_env.status, DUET_STATUS_ON, DUET_STATUS_CLEAN)
	    != DUET_STATUS_ON) {
		printk(KERN_WARNING "duet: framework not on, shutdown aborted\n");
		return 1;
	}

	/* Remove all tasks */
	mutex_lock(&duet_env.task_list_mutex);
	while (!list_empty(&duet_env.tasks)) {
		task = list_entry_rcu(duet_env.tasks.next, struct duet_task,
			task_list);
		list_del_rcu(&task->task_list);
		mutex_unlock(&duet_env.task_list_mutex);

		/* Make sure everyone's let go before we free it */
		synchronize_rcu();
		duet_task_dispose(task);

		mutex_lock(&duet_env.task_list_mutex);
	}
	mutex_unlock(&duet_env.task_list_mutex);

	INIT_LIST_HEAD(&duet_env.tasks);
	mutex_destroy(&duet_env.task_list_mutex);
	atomic_set(&duet_env.status, DUET_STATUS_OFF);
	return 0;
}

static int duet_ioctl_status(void __user *arg)
{
	struct duet_ioctl_status_args *sa;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_INFO "duet: size of duet_ioctl_status_args is %zu\n",
		sizeof(*sa));
#endif /* CONFIG_DUET_DEBUG */

	sa = memdup_user(arg, sizeof(*sa));
	if (IS_ERR(sa))
		return PTR_ERR(sa);

	switch (sa->cmd_flags) {
	case DUET_STATUS_START:
		if (duet_bootstrap()) {
			printk(KERN_ERR "duet: failed to start duet\n");
			goto err;
		}
		printk(KERN_INFO "duet: framework enabled\n");
		break;
	case DUET_STATUS_STOP:
		if (duet_shutdown()) {
			printk(KERN_ERR "duet: failed to stop duet\n");
			goto err;
		}
		printk(KERN_INFO "duet: framework disabled\n");
		break;
	default:
		printk(KERN_ERR "duet: unknown status command received\n");
		goto err;
		break;
	}

	kfree(sa);
	return 0;

err:
	kfree(sa);
	return -EINVAL;
}

static int duet_task_sendlist(struct duet_ioctl_tasks_args *ta)
{
	int i=0;
	struct duet_task *cur;

	/* We will only send the first MAX_TASKS, and that's ok */
	rcu_read_lock();
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		ta->taskid[i] = cur->id;
		memcpy(ta->task_names[i], cur->name, TASK_NAME_LEN);
		i++;
		if (i == MAX_TASKS)
			break;
        }
        rcu_read_unlock();

	return 0;
}

static int duet_ioctl_tasks(void __user *arg)
{
	struct duet_ioctl_tasks_args *ta;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

#ifdef CONFIG_DUET_DEBUG
	printk(KERN_INFO "duet: size of duet_ioctl_tasks_args is %zu\n",
		sizeof(*ta));
#endif /* CONFIG_DUET_DEBUG */

	ta = memdup_user(arg, sizeof(*ta));
	if (IS_ERR(ta))
		return PTR_ERR(ta);

	switch (ta->cmd_flags) {
	case DUET_TASKS_LIST:
		if (duet_task_sendlist(ta)) {
			printk(KERN_ERR "duet: failed to send list\n");
			goto err;
		}
		if (copy_to_user(arg, ta, sizeof(*ta))) {
			printk(KERN_ERR "duet: failed to copy out tasklist\n");
			goto err;
		}
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: task list sent\n");
#endif /* CONFIG_DUET_DEBUG */
		break;
	case DUET_TASKS_REGISTER:
		if (duet_task_register(&ta->taskid[0], ta->task_names[0])) {
			printk(KERN_ERR "duet: registration failed\n");
			goto err;
		}
		if (copy_to_user(arg, ta, sizeof(*ta))) {
			printk(KERN_ERR "duet: failed to copy out taskid\n");
			goto err;
		}
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: registered new task under ID %u\n",
			ta->taskid[0]);
#endif /* CONFIG_DUET_DEBUG */
		break;
	case DUET_TASKS_DEREGISTER:
		if (duet_task_deregister(ta->taskid[0])) {
			printk(KERN_ERR "duet: deregistration failed\n");
			goto err;
		}
#ifdef CONFIG_DUET_DEBUG
		printk(KERN_INFO "duet: deregistered task under ID %u\n",
			ta->taskid[0]);
#endif /* CONFIG_DUET_DEBUG */
		break;
	default:
		printk(KERN_INFO "duet: unknown tasks command received\n");
		goto err;
		break;
	}

	kfree(ta);
	return 0;

err:
	kfree(ta);
	return -EINVAL;
}

/* 
 * ioctl handler function; passes control to the proper handling function
 * for the ioctl received.
 */
long duet_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	/* If we're in the process of cleaning up, no ioctls (other
	 * than the one that switches duet on/off) are allowed
	 */
	if (atomic_read(&duet_env.status) != DUET_STATUS_ON &&
	    cmd != DUET_IOC_STATUS) {
		printk(KERN_INFO "duet: non-status ioctls rejected while "
			"duet is not online\n");
		return -EINVAL;
	}

	switch (cmd) {
	case DUET_IOC_STATUS:
		return duet_ioctl_status(argp);
	case DUET_IOC_TASKS:
		return duet_ioctl_tasks(argp);
	}

	return -EINVAL;
}
