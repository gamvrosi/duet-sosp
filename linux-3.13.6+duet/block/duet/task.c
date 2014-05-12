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

/* Properly dispose of a task struct */
void duet_task_dispose(struct duet_task *task)
{
	kfree(task);
}

int duet_task_register(__u8 *taskid, const char *name)
{
	struct list_head *last;
	struct duet_task *cur, *task;

	if (strnlen(name, TASK_NAME_LEN) == TASK_NAME_LEN) {
		printk(KERN_ERR "duet: error -- task name too long\n");
		return -EINVAL;
	}

	/* Allocate and init the task structure */
	task = kzalloc(sizeof(*task), GFP_NOFS);
	if (!task)
		return -ENOMEM;

	INIT_LIST_HEAD(&task->task_list);
	memcpy(task->name, name, TASK_NAME_LEN);
	task->id = 1;

	/* Find a free task id for the new task.
	 * Tasks are sorted by id, so that we can find the smallest
	 * free id in one traversal (just look for a gap). We don't
	 * want other ioctls touching this list, so we lock it
	 * exclusively; this should be fine just for addition. */
	mutex_lock(&duet_env.task_list_mutex);
	last = &duet_env.tasks;
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->id == task->id)
			(task->id)++;
		else if (cur->id > task->id)
			break;

		last = &cur->task_list;
	}
	list_add_rcu(&task->task_list, last);
	mutex_unlock(&duet_env.task_list_mutex);

	*taskid = task->id;
	return 0;
}

int duet_task_deregister(__u8 taskid)
{
	struct duet_task *cur;

	/* Find the task in the list, then dispose of it */
	mutex_lock(&duet_env.task_list_mutex);
	list_for_each_entry_rcu(cur, &duet_env.tasks, task_list) {
		if (cur->id == taskid) {
			list_del_rcu(&cur->task_list);
			mutex_unlock(&duet_env.task_list_mutex);
			synchronize_rcu();
			duet_task_dispose(cur);
			return 0;
		}
	}
	mutex_unlock(&duet_env.task_list_mutex);

	return -ENOENT;
}
