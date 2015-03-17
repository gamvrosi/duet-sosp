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
#include <linux/syscalls.h>
#include <linux/file.h>
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

	spin_lock_init(&duet_env.evtwq_lock);

	/*
	 * Events will be put on this work queue as they arrive. It needs to be
	 * ordered, otherwise state model won't work.
	 * XXX: Would WQ_HIGHPRI make sense in this case?
	 */
	duet_env.evtwq = alloc_ordered_workqueue("duet-evtwq", 0);
	if (!duet_env.evtwq) {
		printk(KERN_ERR "duet: failed to allocate event work queue\n");
		atomic_set(&duet_env.status, DUET_STATUS_OFF);
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
	struct workqueue_struct *tmp_wq = NULL;

	if (atomic_cmpxchg(&duet_env.status, DUET_STATUS_ON, DUET_STATUS_CLEAN)
	    != DUET_STATUS_ON) {
		printk(KERN_WARNING "duet: framework off, shutdown aborted\n");
		return 1;
	}

	rcu_assign_pointer(duet_hook_cache_fp, NULL);
	synchronize_rcu();

	/* Flush and destroy work queue */
	spin_lock_irq(&duet_env.evtwq_lock);
	tmp_wq = duet_env.evtwq;
	duet_env.evtwq = NULL;
	spin_unlock_irq(&duet_env.evtwq_lock);
	flush_workqueue(tmp_wq);
	destroy_workqueue(tmp_wq);

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

/* Scan through the page cache, and populate the task's tree. */
static int find_get_inode(struct super_block *sb, unsigned long c_ino,
	struct inode **c_inode)
{
	unsigned int loop;
	struct hlist_head *head;
	struct inode *inode = NULL;

	*c_inode = NULL;
	for (loop = 0; loop < (1U << *duet_i_hash_shift); loop++) {
		head = *duet_inode_hashtable + loop;
		spin_lock(duet_inode_hash_lock);

		/* Process this hash bucket */
		hlist_for_each_entry(inode, head, i_hash) {
			if (inode->i_sb != sb)
				continue;

			spin_lock(&inode->i_lock);
			if (!*c_inode && inode->i_ino == c_ino) {
				__iget(inode);
				*c_inode = inode;
				spin_unlock(&inode->i_lock);
				spin_unlock(duet_inode_hash_lock);
				return 0;
			}
			spin_unlock(&inode->i_lock);
		}
		spin_unlock(duet_inode_hash_lock);
	}

	/* We shouldn't get here unless we failed */
	return 1;
}

static int duet_getpath(__u8 tid, unsigned long c_ino, char *cpath)
{
	int len, ret = 0;
	struct duet_task *task = duet_find_task(tid);
	struct inode *c_inode;
	char *p, *buf;

	if (!task) {
		printk(KERN_ERR "duet_getpath: invalid taskid (%d)\n", tid);
		return 1;	
	}

	if (!task->p_inode) {
		printk(KERN_ERR "duet_getpath: task was not registered under a parent inode\n");
		return 1;
	}

	/* First, we need to find struct inode for child and parent */
	if (find_get_inode(task->f_sb, c_ino, &c_inode)) {
		printk(KERN_ERR "duet_getpath: failed to find child inode\n");
		ret = 1;
		goto done;
	}

	printk(KERN_INFO "duet_getpath: parent inode has ino %lu, sb %p\n",
		task->p_inode->i_ino, task->p_inode->i_sb);
	printk(KERN_INFO "duet_getpath: child inode has ino %lu, sb %p\n",
		c_inode->i_ino, c_inode->i_sb);

	/* Now get the path */
	len = MAX_PATH;
	buf = kmalloc(MAX_PATH, GFP_NOFS);
	if (!buf) {
		printk(KERN_ERR "duet_getpath: failed to allocate path buf\n");
		ret = 1;
		goto done_put;
	}

	p = d_get_path(c_inode, task->p_inode, buf, len);
	if (IS_ERR(p)) {
		printk(KERN_INFO "duet_getpath: parent dentry not found\n");
		ret = 1;
		cpath[0] = '\0';
	} else if (!p) {
		duet_dbg(KERN_INFO "duet_getpath: no connecting path found\n");
		ret = 0;
		cpath[0] = '\0';
	} else {
		printk(KERN_INFO "duet_getpath: got %s\n", p);
		p++;
		memcpy(cpath, p, len - (p - buf) + 1);
	}

	kfree(buf);
done_put:
	iput(c_inode);
done:
	/* decref and wake up cleaner if needed */
	if (atomic_dec_and_test(&task->refcount))
		wake_up(&task->cleaner_queue);

	return ret;
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
	struct file *file;
	mm_segment_t old_fs;
	int fd;

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
		/* First, open the path we were given, if it's there */
		old_fs = get_fs();
		set_fs(KERNEL_DS);

		fd = sys_open(ca->path, O_RDONLY, 0644);
		if (fd < 0) {
			printk(KERN_ERR "duet: failed to open %s\n", ca->path);
			goto reg_done;
		}

		file = fget(fd);
		if (!file) {
			printk(KERN_ERR "duet: failed to get %s\n", ca->path);
			goto reg_close;
		}

		if (!file->f_inode) {
			printk(KERN_ERR "duet: no inode for %s\n", ca->path);
			goto reg_put;
		}

		if (!S_ISDIR(file->f_inode->i_mode)) {
			printk(KERN_ERR "duet: must register a dir\n");
			goto reg_put;
		}

		ca->ret = duet_register(&ca->tid, ca->name, ca->evtmask,
					ca->bitrange, file->f_inode->i_sb,
					file->f_inode);
		printk(KERN_INFO "duet: registered under %s, ino %lu, sb %p\n",
			ca->path, file->f_inode->i_ino, file->f_inode->i_sb);

reg_put:
		fput(file);
reg_close:
		sys_close(fd);
reg_done:
		set_fs(old_fs);
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

	case DUET_GETPATH:
		ca->ret = duet_getpath(ca->tid, ca->c_ino, ca->cpath);
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
		la->evtmask[i] = cur->evtmask;
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
