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

#include "defrag.h"
#include "btrfs_inode.h"
#include "transaction.h"
//#include "ctree.h"
//#include "volumes.h"
//#include "mapping.h"
//#include "raid56.h"
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#include <linux/workqueue.h>
#include <linux/duet.h>
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

struct defrag_ctx {
	struct btrfs_root *defrag_root;
#if 0
	/* current state of the compare_tree call */
	struct btrfs_path *left_path;
	struct btrfs_path *right_path;
	struct btrfs_key *cmp_key;
#endif
	u64 send_progress;

#ifdef CONFIG_BTRFS_DUET_DEFRAG
	spinlock_t wq_lock;
	struct workqueue_struct *syn_wq;
	__u8 taskid;
};

struct defrag_synwork {
	struct work_struct work;
#if 0
	struct block_device *bdev;
	u64 lbn;
	u64 len;
	struct bio *bio;
#endif
	struct defrag_ctx *dctx;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
};

static int process_inode(struct defrag_ctx *dctx)
{
	// TODO: Implement
	return 0;
}

static struct btrfs_path *alloc_path_for_defrag(void)
{
	struct btrfs_path *path;

	path = btrfs_alloc_path();
	if (!path)
		return NULL;
	path->search_commit_root = 1;
	path->skip_locking = 1;
	return path;
}

static int defrag_subvol(struct defrag_ctx *dctx)
{
	int ret, slot;
	struct btrfs_path *path;
	struct btrfs_root *defrag_root = dctx->defrag_root;
	struct btrfs_fs_info *fs_info = defrag_root->fs_info;
	struct btrfs_key found_key;
	struct extent_buffer *eb;
	struct btrfs_key key;
	struct btrfs_trans_handle *trans = NULL;
	u64 start_ctransid, ctransid;

	path = alloc_path_for_defrag();
	if (!path)
		return -ENOMEM;

	spin_lock(&defrag_root->root_item_lock);
	start_ctransid = btrfs_root_ctransid(&defrag_root->root_item);
	spin_unlock(&defrag_root->root_item_lock);

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

join_trans:
	/*
	 * We need to make sure the transaction does not get committed while
	 * we do anything on commit roots. Join a transaction to prevent this.
	 */
	trans = btrfs_join_transaction(defrag_root);
	if (IS_ERR(trans)) {
		ret = PTR_ERR(trans);
		trans = NULL;
		goto out;
	}

	/*
	 * Make sure the tree has not changed after re-joining. We detect this
	 * by comparing start_ctransid and ctransid. They should always match.
	 */
	spin_lock(&defrag_root->root_item_lock);
	ctransid = btrfs_root_ctransid(&defrag_root->root_item);
	spin_unlock(&defrag_root->root_item_lock);

	if (ctransid != start_ctransid) {
		WARN(1, KERN_WARNING "btrfs: the root that you're trying to "
				     "defrag was modified in between. This "
				     "is probably a bug.\n");
		ret = -EIO;
		goto out;
	}

	ret = btrfs_search_slot_for_read(defrag_root, &key, path, 1, 0);
	if (ret)
		goto out;

	while (1) {
		/*
		 * When someone want to commit while we iterate, end the
		 * joined transaction and rejoin.
		 */
		if (btrfs_should_end_transaction(trans, defrag_root)) {
			ret = btrfs_end_transaction(trans, defrag_root);
			trans = NULL;
			if (ret < 0)
				goto out;
			btrfs_release_path(path);
			goto join_trans;
		}

		eb = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(eb, &found_key, slot);

		/* Check if we've been asked to cancel */
		if (atomic_read(&fs_info->defrag_cancel_req))
			return -1;

		/* If we couldn't find an inode, we're done */
		if (found_key.type != BTRFS_INODE_ITEM_KEY)
			goto next;

		ret = process_inode(dctx);
		if (ret < 0)
			goto out;

next:
		key.objectid = found_key.objectid;
		key.type = found_key.type;
		key.offset = found_key.offset + 1;

		ret = btrfs_next_item(defrag_root, path);
		if (ret < 0)
			goto out;
		if (ret) {
			ret  = 0;
			break;
		}
	}

out:
	btrfs_free_path(path);
	if (trans) {
		if (!ret)
			ret = btrfs_end_transaction(trans, defrag_root);
		else
			btrfs_end_transaction(trans, defrag_root);
	}
	return ret;
}

long btrfs_ioctl_defrag_start(struct file *file, void __user *arg_)
{
	int ret = 0;
	struct btrfs_fs_info *fs_info;
	struct btrfs_ioctl_defrag_args *arg = NULL;
	struct btrfs_root *defrag_root;
	struct defrag_ctx *dctx = NULL;
#ifdef CONFIG_BTRFS_DUET_DEFRAG
	struct workqueue_struct *tmp_wq = NULL;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	defrag_root = BTRFS_I(file_inode(file))->root;
	fs_info = defrag_root->fs_info;

	/* Check if a defrag is already running, and terminate if so */
	mutex_lock(&fs_info->defrag_lock);
	if (atomic_read(&fs_info->defrag_fs_running)) {
		mutex_unlock(&fs_info->defrag_lock);
		return -EINPROGRESS;
	}
	atomic_inc(&fs_info->defrag_fs_running);
	mutex_unlock(&fs_info->defrag_lock);

	/*
	 * This is done when we lookup the root, it should already be complete
	 * by the time we get here.
	 */
	WARN_ON(defrag_root->orphan_cleanup_state != ORPHAN_CLEANUP_DONE);

	/*
	 * If we just created this root we need to make sure that the orphan
	 * cleanup has been done and committed since we search the commit root,
	 * so check its commit root transid with our otransid and if they match
	 * commit the transaction to make sure everything is updated.
	 */
	down_read(&defrag_root->fs_info->extent_commit_sem);
	if (btrfs_header_generation(defrag_root->commit_root) ==
	    btrfs_root_otransid(&defrag_root->root_item)) {
		struct btrfs_trans_handle *trans;

		up_read(&defrag_root->fs_info->extent_commit_sem);

		trans = btrfs_attach_transaction_barrier(defrag_root);
		if (IS_ERR(trans)) {
			if (PTR_ERR(trans) != -ENOENT) {
				ret = PTR_ERR(trans);
				goto out;
			}
			/* ENOENT means theres no transaction */
		} else {
			ret = btrfs_commit_transaction(trans, defrag_root);
			if (ret)
				goto out;
		}
	} else {
		up_read(&defrag_root->fs_info->extent_commit_sem);
	}

	arg = memdup_user(arg_, sizeof(*arg));
	if (IS_ERR(arg)) {
		ret = PTR_ERR(arg);
		arg = NULL;
		goto out;
	}

	dctx = kzalloc(sizeof(struct defrag_ctx), GFP_NOFS);
	if (!dctx) {
		ret = -ENOMEM;
		goto out;
	}

	dctx->defrag_root = defrag_root;

	/* Store context in fs_info */
	mutex_lock(&fs_info->defrag_lock);
	atomic64_set(&fs_info->defrag_bytes_total, 0);
	atomic64_set(&fs_info->defrag_bytes_best_effort, 0);
	atomic64_set(&fs_info->defrag_start_jiffies, jiffies);
	fs_info->cur_defrag = dctx;
	/* Were we asked to cancel already? */
	if (atomic_read(&fs_info->defrag_cancel_req)) {
		mutex_unlock(&fs_info->defrag_lock);
		goto out;
	}
	mutex_unlock(&fs_info->defrag_lock);

#ifdef CONFIG_BTRFS_DUET_DEFRAG
	spin_lock_init(&dctx->wq_lock);

	/* Out-of-order inode defrag will be put on this work queue */
	dctx->syn_wq = alloc_workqueue("duet-defrag", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!dctx->syn_wq) {
		printk(KERN_ERR "defrag: failed to allocate work queue\n");
		ret = -EFAULT;
		goto out;
	}

	/* Register the task with the Duet framework */
	if (duet_task_register(&dctx->taskid, "btrfs-defrag",
			fs_info->sb->s_blocksize, 32768,
			DUET_EVENT_CACHE_INSERT | DUET_EVENT_CACHE_REMOVE,
			btrfs_defrag_duet_handler, (void *)dctx)) {
		printk(KERN_ERR "defrag: failed to register with the duet framework\n");
		ret = -EFAULT;
		goto out;
	}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

	ret = defrag_subvol(dctx);
	if (ret < 0) {
		if (atomic_read(&fs_info->defrag_cancel_req))
			ret = -ESHUTDOWN;
		goto out;
	}

out:
	kfree(arg);

	/* Clear context from fs_info */
	mutex_lock(&fs_info->defrag_lock);
	fs_info->cur_defrag = NULL;
	atomic_dec(&fs_info->defrag_fs_running);
	atomic64_set(&fs_info->defrag_start_jiffies,
		jiffies - atomic64_read(&fs_info->defrag_start_jiffies));
	mutex_unlock(&fs_info->defrag_lock);
	wake_up(&fs_info->defrag_cancel_wait);

#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	/* Let's first print out the bitmaps */
	duet_print_rbt(dctx->taskid);
	printk(KERN_DEBUG "defrag: total bytes defragged = %ld\n",
			atomic64_read(&fs_info->defrag_bytes_total));
	printk(KERN_DEBUG "defrag: bytes defragged best-effort: %ld\n",
			atomic64_read(&fs_info->defrag_bytes_best_effort));
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

#ifdef CONFIG_BTRFS_DUET_DEFRAG
	/* Flush and destroy work queue */
	spin_lock(&dctx->wq_lock);
	tmp_wq = dctx->syn_wq;
	dctx->syn_wq = NULL;
	spin_unlock(&dctx->wq_lock);
	flush_workqueue(tmp_wq);
	destroy_workqueue(tmp_wq);

	/* Deregister the task from the Duet framework */
	if (duet_task_deregister(dctx->taskid))
		printk(KERN_ERR "defrag: failed to deregister from duet framework\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

	if (dctx)
		kfree(dctx);
	return ret;
}

long btrfs_ioctl_defrag_cancel(struct btrfs_root *root, void __user *arg)
{
	struct btrfs_fs_info *fs_info;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	fs_info = root->fs_info;

	mutex_lock(&fs_info->defrag_lock);
	/* First check if we're running */
	if (!atomic_read(&fs_info->defrag_fs_running)) {
		mutex_unlock(&fs_info->defrag_lock);
		return -ENOTCONN;
	}

	atomic_inc(&fs_info->defrag_cancel_req);
	while (atomic_read(&fs_info->defrag_fs_running)) {
		mutex_unlock(&fs_info->defrag_lock);
		wait_event(fs_info->defrag_cancel_wait,
			atomic_read(&fs_info->defrag_fs_running) == 0);
		mutex_lock(&fs_info->defrag_lock);
	}
	atomic_dec(&fs_info->defrag_cancel_req);

	mutex_unlock(&fs_info->defrag_lock);
	return 0;
}

long btrfs_ioctl_defrag_progress(struct btrfs_root *root, void __user *arg)
{
	struct btrfs_ioctl_defrag_args *da;
	struct btrfs_fs_info *fs_info;
	int ret = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	da = memdup_user(arg, sizeof(*da));
	if (IS_ERR(da))
		return PTR_ERR(da);

	fs_info = root->fs_info;

	mutex_lock(&fs_info->defrag_lock);

	/* If we're not running, we return the results from last time */
	if (!atomic64_read(&fs_info->defrag_start_jiffies)) {
		da->progress.running = 0;
		da->progress.elapsed_time = 0;
	} else if (atomic_read(&fs_info->defrag_fs_running)) {
		da->progress.running = 1;
		da->progress.elapsed_time = (jiffies -
			atomic64_read(&fs_info->defrag_start_jiffies)) / HZ;
	} else {
		da->progress.running = 0;
		da->progress.elapsed_time =
			atomic64_read(&fs_info->defrag_start_jiffies) / HZ;
	}

	da->progress.bytes_total = atomic64_read(&fs_info->defrag_bytes_total);
	da->progress.bytes_best_effort =
			atomic64_read(&fs_info->defrag_bytes_best_effort);

	mutex_unlock(&fs_info->defrag_lock);

	if (copy_to_user(arg, da, sizeof(*da)))
		ret = -EFAULT;

	kfree(da);
	return ret;
}
