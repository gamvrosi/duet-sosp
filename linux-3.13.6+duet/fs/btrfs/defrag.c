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
#include "defrag.h"
#include "btrfs_inode.h"
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#include <linux/duet.h>
#include "mapping.h"
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
#include <linux/ktime.h>
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */

#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
#define defrag_dbg(...)	printk(__VA_ARGS__)
#else
#define defrag_dbg(...)
#endif

struct defrag_ctx {
	struct btrfs_root *defrag_root;
	u64 defrag_progress;
	struct btrfs_ioctl_defrag_range_args range;
	struct super_block *sb;	/* will help us tell if inodes are ours */

#ifdef CONFIG_BTRFS_DUET_DEFRAG
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	atomic64_t bittree_time;
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
	__u8 taskid;
	struct inode_tree itree;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
};

static int defrag_inode(struct inode *inode, struct defrag_ctx *dctx,
			int out_of_order)
{
	int ret;
#ifdef CONFIG_BTRFS_DUET_DEFRAG
	unsigned long cache_hits, dirty_pages;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
	struct btrfs_fs_info *fs_info = dctx->defrag_root->fs_info;

	//sb_start_write(fs_info->sb);
#ifdef CONFIG_BTRFS_DUET_DEFRAG
	ret = btrfs_defrag_file_trace(inode, NULL, &dctx->range,
				0, 0, &cache_hits, &dirty_pages);
#else
	ret = btrfs_defrag_file(inode, NULL, &dctx->range, 0, 0);
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
	//sb_end_write(fs_info->sb);

	if (ret > 0) {
		/* Update progress counters */
		atomic64_add(2*ret*PAGE_SIZE, &fs_info->defrag_bytes_total);
#ifdef CONFIG_BTRFS_DUET_DEFRAG
		if (out_of_order) {
			atomic64_add(2 * ret * PAGE_SIZE,
				&fs_info->defrag_bytes_best_effort);
			atomic64_add((dirty_pages + cache_hits) * PAGE_SIZE,
				&fs_info->defrag_bytes_from_mem);
		}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		ret = 0;
	}

	return ret;
}

#ifdef CONFIG_BTRFS_DUET_DEFRAG
/*
 * Get the inode with the specified inode number.
 * Returns 1 if the inode is no longer in the cache (and no inode),
 * and -1 on any other error.
 */
static int defrag_get_inode(void *ctx, unsigned long ino, struct inode **inode)
{
	int ondisk = 0;
	struct defrag_ctx *dctx = (struct defrag_ctx *)ctx;
	struct btrfs_fs_info *fs_info = dctx->defrag_root->fs_info;

	if (btrfs_iget_ino(fs_info, ino, inode, &ondisk)) {
		defrag_dbg("duet-defrag: failed to get inode\n");
		return -1;
	}

	if (ondisk) {
		iput(*inode);
		*inode = NULL;
	}

	return ondisk;
}

/*
 * First, updates the inode tree by fetching all page events that have not been
 * processed yet. Then, picks an inode from the inode tree and processes it, if
 * it has not been processed already.
 * Returns 1 if it processed an inode out of order, or 0 if it didn't.
 */
static int process_inmem_inode(struct defrag_ctx *dctx)
{
	int ret = 0;
	struct inode *inode = NULL;

again:
	if (itree_update(&dctx->itree, dctx->taskid, defrag_get_inode,
			(void *)dctx)) {
		defrag_dbg(KERN_ERR "duet-defrag: failed to update itree\n");
		return 0;
	}

	if (itree_fetch(&dctx->itree, dctx->taskid, &inode, defrag_get_inode,
			(void *)dctx)) {
		defrag_dbg(KERN_INFO "duet-defrag: failed to fetch an inode\n");
		return 0;
	}

	if (!inode) {
		defrag_dbg(KERN_DEBUG "duet-defrag: no inode to pick\n");
		return 0;
	}

	duet_dbg(KERN_INFO "duet-defrag: picked inode %lu\n", inode->i_ino);

	/* We only process regular files */
	if (!S_ISREG(inode->i_mode)) {
		ret = 0;
		goto iput_out;
	}

	if (duet_mark(dctx->taskid, inode->i_ino, 1)) {
		printk(KERN_ERR "duet-defrag: failed to mark inode %lu\n",
			inode->i_ino);
		ret = 0;
		goto iput_out;
	}

	/*
	 * Check if this is a new inode that we should ignore.
	 * If so, try again.
	 */
	if (dctx->defrag_progress > btrfs_ino(inode)) {
		iput(inode);
		goto again;
	}

	if (defrag_inode(inode, dctx, 1)) {
		printk(KERN_ERR "duet-defrag: file defrag failed\n");
		ret = 0;
		goto iput_out;
	}

	printk(KERN_INFO "duet-defrag: processed inode %lu out of order\n",
		inode->i_ino);

	ret = 1;
iput_out:
	iput(inode);
	return ret;
}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

static int defrag_subvol(struct defrag_ctx *dctx)
{
	int ret, slot;
	struct btrfs_path *path;
	struct btrfs_root *defrag_root = dctx->defrag_root;
	struct btrfs_fs_info *fs_info = defrag_root->fs_info;
	struct btrfs_key found_key;
	struct extent_buffer *eb;
	struct btrfs_key key, key_start, key_end;
	struct reada_control *reada;
	struct inode *inode;

	/*
	 * We will be looking for inodes starting from the commit root.
	 * Locking will protect us if the commit root is not read-only.
	 */
	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->search_commit_root = 1;

	/* Start a readahead on defrag_root, to get things rolling faster */
	defrag_dbg(KERN_DEBUG "btrfs defrag: readahead started at %lu.\n", jiffies);
	key_start.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key_start.type = BTRFS_INODE_ITEM_KEY;
	key_start.offset = (u64)0;
	key_end.objectid = (u64)-1;
	key_end.type = BTRFS_INODE_ITEM_KEY;
	key_end.offset = (u64)-1;
	reada = btrfs_reada_add(defrag_root, &key_start, &key_end);

	/* No need to wait for readahead. It's not like we need it all now */
	if (!IS_ERR(reada))
		btrfs_reada_detach(reada);
	defrag_dbg(KERN_DEBUG "btrfs defrag: readahead ended at %lu.\n", jiffies);

	key.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	while (1) {
		/* Check if we've been asked to cancel */
		if (atomic_read(&fs_info->defrag_cancel_req)) {
			defrag_dbg(KERN_INFO "btrfs defrag: we've been asked to cancel\n");
			goto out;
		}

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		if (duet_online() && dctx->taskid && process_inmem_inode(dctx))
			continue;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		ret = btrfs_search_slot_for_read(defrag_root, &key, path, 1, 0);
		if (ret) {
			printk(KERN_INFO "btrfs defrag: defrag complete\n");
			ret = 0;
			break;
		}

		eb = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(eb, &found_key, slot);

		/* We upref'ed the inode. Release the locks */
		btrfs_release_path(path);

		/* If we couldn't find an inode, move on to the next */
		if (found_key.type != BTRFS_INODE_ITEM_KEY)
			goto next;

		/* Mark our progress before we process the inode.
		 * This way, duet will ignore it as processed. */
		dctx->defrag_progress = found_key.objectid;

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		/* Check if we've already processed this inode */
		if (duet_online() && dctx->taskid &&
		    duet_check(dctx->taskid, found_key.objectid, 1) == 1) {
			defrag_dbg(KERN_INFO "btrfs defrag: skipping inode "
					"%llu\n", dctx->defrag_progress);
			goto next;
		}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		/* Get the inode */
		inode = btrfs_iget(fs_info->sb, &found_key, defrag_root, NULL);
		if (IS_ERR(inode)) {
			printk(KERN_ERR "btrfs defrag: iget failed, skipping\n");
			//ret = PTR_ERR(inode);
			goto next;
		}

		/* We only process regular files */
		if ((inode->i_mode & S_IFMT) != S_IFREG) {
			iput(inode);
			goto next;
		}

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		/* Mark the inode as done */
		if (duet_online() && dctx->taskid &&
		    duet_mark(dctx->taskid, found_key.objectid, 1)) {
			printk(KERN_ERR "duet: failed to mark inode %llu\n",
				found_key.objectid);
			iput(inode);
			goto out;
		}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		ret = defrag_inode(inode, dctx, 0);
		if (ret) {
			printk(KERN_ERR "btrfs defrag: file defrag failed\n");
			iput(inode);
			goto out;
		}

		iput(inode);

		printk(KERN_INFO "btrfs defrag: processed inode %llu\n",
			dctx->defrag_progress);

next:
		key.objectid = found_key.objectid + 1;
		key.type = found_key.type;
		key.offset = 0;
	}

out:
	btrfs_free_path(path);
	return ret;
}

long btrfs_ioctl_defrag_start(struct file *file, void __user *arg_)
{
	int ret = 0;
	struct btrfs_fs_info *fs_info;
	struct btrfs_ioctl_defrag_args *arg = NULL;
	struct btrfs_root *defrag_root;
	struct defrag_ctx *dctx = NULL;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	defrag_root = BTRFS_I(file_inode(file))->root;
	fs_info = defrag_root->fs_info;

	if (btrfs_root_readonly(defrag_root))
		return -EROFS;

	/* Check if a defrag is already running, and terminate if so */
	mutex_lock(&fs_info->defrag_lock);
	if (atomic_read(&fs_info->defrag_fs_running)) {
		mutex_unlock(&fs_info->defrag_lock);
		return -EINPROGRESS;
	}
	atomic_inc(&fs_info->defrag_fs_running);
	mutex_unlock(&fs_info->defrag_lock);

	arg = memdup_user(arg_, sizeof(*arg));
	if (IS_ERR(arg)) {
		ret = PTR_ERR(arg);
		arg = NULL;
		return ret;
	}

	/* Initialize defrag context */
	dctx = kzalloc(sizeof(struct defrag_ctx), GFP_NOFS);
	if (!dctx) {
		kfree(arg);
		return -ENOMEM;
	}

	memcpy(&dctx->range, &arg->range, sizeof(dctx->range));

	/* Compression requires us to start the IO */
	if ((dctx->range.flags & BTRFS_DEFRAG_RANGE_COMPRESS)) {
		dctx->range.flags |= BTRFS_DEFRAG_RANGE_START_IO;
		dctx->range.extent_thresh = (u32)-1;
	}

	/* Keep a superblock pointer to help us tell if an inode is ours
	 * (sb is accessible through dctx->defrag_root->fs_info but let's
	 * save ourselves the trouble of pointer resolution) */
	dctx->defrag_root = defrag_root;
	dctx->sb = defrag_root->fs_info->sb;

	/* Store context in fs_info */
	mutex_lock(&fs_info->defrag_lock);
	atomic64_set(&fs_info->defrag_bytes_total, 0);
	atomic64_set(&fs_info->defrag_bytes_best_effort, 0);
	atomic64_set(&fs_info->defrag_bytes_from_mem, 0);
	atomic64_set(&fs_info->defrag_start_jiffies, jiffies);
	fs_info->cur_defrag = dctx;
	/* Were we asked to cancel already? */
	if (atomic_read(&fs_info->defrag_cancel_req)) {
		mutex_unlock(&fs_info->defrag_lock);
		goto bail;
	}
	mutex_unlock(&fs_info->defrag_lock);

#ifdef CONFIG_BTRFS_DUET_DEFRAG
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	atomic64_set(&dctx->bittree_time, 0);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
	itree_init(&dctx->itree);

	/* Register the task with the Duet framework */
	if (duet_register(&dctx->taskid, "btrfs-defrag",
		DUET_EVT_ADD | DUET_EVT_REM, 1, fs_info->sb)) {
		printk(KERN_ERR "defrag: failed to register with duet\n");
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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	/* Let's first print out the bitmaps */
	duet_print_bittree(dctx->taskid);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
	printk(KERN_DEBUG "defrag: total bytes defragged = %ld\n",
			atomic64_read(&fs_info->defrag_bytes_total));
	printk(KERN_DEBUG "defrag: bytes defragged best-effort: %ld\n",
			atomic64_read(&fs_info->defrag_bytes_best_effort));
	printk(KERN_DEBUG "defrag: bytes found already in memory: %ld\n",
			atomic64_read(&fs_info->defrag_bytes_from_mem));
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	printk(KERN_DEBUG "defrag: CPU time spent updating the RBIT: %llds\n",
		(long long) div64_u64(atomic64_read(&dctx->bittree_time), 1E6));
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */

	if (duet_deregister(dctx->taskid))
		printk(KERN_ERR "defrag: failed to deregister with duet\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

bail:
	/* Clear context from fs_info */
	mutex_lock(&fs_info->defrag_lock);
	fs_info->cur_defrag = NULL;
	atomic_dec(&fs_info->defrag_fs_running);
	atomic64_set(&fs_info->defrag_start_jiffies,
		jiffies - atomic64_read(&fs_info->defrag_start_jiffies));
	mutex_unlock(&fs_info->defrag_lock);
	wake_up(&fs_info->defrag_cancel_wait);

	kfree(arg);
	if (dctx) {
#ifdef CONFIG_BTRFS_DUET_DEFRAG
		itree_teardown(&dctx->itree);
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
		kfree(dctx);
	}
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
			atomic64_read(&fs_info->defrag_start_jiffies));
		do_div(da->progress.elapsed_time, HZ);
	} else {
		da->progress.running = 0;
		da->progress.elapsed_time =
			atomic64_read(&fs_info->defrag_start_jiffies);
		do_div(da->progress.elapsed_time, HZ);
	}

	da->progress.bytes_total = atomic64_read(&fs_info->defrag_bytes_total);
	da->progress.bytes_best_effort =
			atomic64_read(&fs_info->defrag_bytes_best_effort);
	da->progress.bytes_from_mem =
			atomic64_read(&fs_info->defrag_bytes_from_mem);

	mutex_unlock(&fs_info->defrag_lock);

	if (copy_to_user(arg, da, sizeof(*da)))
		ret = -EFAULT;

	kfree(da);
	return ret;
}
