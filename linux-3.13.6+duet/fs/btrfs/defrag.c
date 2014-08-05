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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#include <linux/workqueue.h>
#include <linux/duet.h>
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

struct defrag_ctx {
	struct btrfs_root *defrag_root;
	u64 defrag_progress;
	struct btrfs_ioctl_defrag_range_args range;
	struct super_block *sb;	/* will help us tell if inodes are ours */

#ifdef CONFIG_BTRFS_DUET_DEFRAG
	spinlock_t wq_lock;
	struct workqueue_struct *syn_wq;

	/* red-black tree storing inodes with pages in cache */
	struct mutex itree_mutex;
	struct rb_root itree;

	__u8 taskid;
};

struct defrag_synwork {
	struct work_struct work;
	struct defrag_ctx *dctx;
	struct page *page;
	u8 event_code;

	u64 btrfs_ino;
	u64 inmem_pages;
	u64 total_pages;
};

struct itree_rbnode {
	struct rb_node node;
	__u64 ino;
	__u64 inmem_pages;
	__u64 total_pages;
	__u8 inmem_ratio;	/* pages (out of 100) currently in memory */
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
};

#ifdef CONFIG_BTRFS_DUET_DEFRAG
static struct itree_rbnode *itnode_init(struct defrag_synwork *dwork)
{
	struct itree_rbnode *itnode = NULL;

	itnode = kzalloc(sizeof(*itnode), GFP_NOFS);
	if (!itnode)
		return NULL;

	RB_CLEAR_NODE(&itnode->node);
	itnode->ino = dwork->btrfs_ino;
	itnode->inmem_pages = dwork->inmem_pages;
	itnode->total_pages = dwork->total_pages;
	itnode->inmem_ratio = (dwork->inmem_pages * 100) / dwork->total_pages;
	return itnode;
}

static void itnode_dispose(struct itree_rbnode *itnode, struct rb_node *rbnode,
			   struct rb_root *root)
{
	rb_erase(rbnode, root);
	kfree(itnode);
}

static void itree_dispose(struct rb_root *root)
{
	struct rb_node *rbnode;
	struct itree_rbnode *itnode;

	while (!RB_EMPTY_ROOT(root)) {
		rbnode = rb_first(root);
		itnode = rb_entry(rbnode, struct itree_rbnode, node);
		itnode_dispose(itnode, rbnode, root);
	}

	return;
}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

static int defrag_inode(struct inode *inode, struct defrag_ctx *dctx,
			int out_of_order)
{
	int ret;
	struct btrfs_fs_info *fs_info = dctx->defrag_root->fs_info;

	ret = btrfs_defrag_file(inode, NULL, &dctx->range, 0, 0);

	if (ret > 0) {
		/* Update progress counters */
		atomic64_add(ret * PAGE_SIZE, &fs_info->defrag_bytes_total);
#ifdef CONFIG_BTRFS_DUET_DEFRAG
		if (out_of_order)
			atomic64_add(ret * PAGE_SIZE,
				&fs_info->defrag_bytes_best_effort);
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		ret = 0;
	}

	return ret;
}

static int process_inode(struct defrag_ctx *dctx, struct btrfs_path *path,
			 struct btrfs_key *key)
{
	int s, ret = 0;
	struct extent_buffer *l;
	struct btrfs_inode_item *ii;
	struct inode *inode;

	/* Grab the inode item */
	l = path->nodes[0];
	s = path->slots[0];
	ii = btrfs_item_ptr(l, s, struct btrfs_inode_item);

	/* We will only process regular files */
	switch (btrfs_inode_mode(l, ii) & S_IFMT) {
	case S_IFREG:
		/* Get the inode */
		inode = btrfs_iget(dctx->defrag_root->fs_info->sb, key,
				   dctx->defrag_root, NULL);
		if (IS_ERR(inode)) {
			printk(KERN_ERR "btrfs defrag: iget failed\n");
			ret = PTR_ERR(inode);
			goto out;
		}

		ret = defrag_inode(inode, dctx, 0);
		if (ret)
			printk(KERN_ERR "btrfs defrag: file defrag failed\n");

		iput(inode);
		break;
	case S_IFDIR:
	case S_IFLNK:
	default:
		ret = 1;
		goto out;
	}

out:
	return ret;
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
	struct btrfs_key key, key_start, key_end;
	struct reada_control *reada;
	struct btrfs_trans_handle *trans = NULL;
	u64 start_ctransid, ctransid;

	path = alloc_path_for_defrag();
	if (!path)
		return -ENOMEM;

	/* Start a readahead on defrag_root, to get things rolling faster */
	printk(KERN_INFO "btrfs defrag: readahead started at %lu.\n", jiffies);
	key_start.objectid = BTRFS_FIRST_FREE_OBJECTID;
	key_start.type = BTRFS_INODE_ITEM_KEY;
	key_start.offset = (u64)0;
	key_end.objectid = (u64)-1;
	key_end.type = BTRFS_INODE_ITEM_KEY;
	key_end.offset = (u64)-1;
	reada = btrfs_reada_add(defrag_root, &key_start, &key_end);

	if (!IS_ERR(reada))
		btrfs_reada_wait(reada);
	printk(KERN_INFO "btrfs defrag: readahead ended at %lu.\n", jiffies);

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

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		/* Pick an inode from the inode rbtree if it's not empty */
		/* TODO: Check tree, pick inode, check if it's been processed,
		 * process, mark as processed. continue to next iteration */
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		eb = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(eb, &found_key, slot);

		/* Check if we've been asked to cancel */
		if (atomic_read(&fs_info->defrag_cancel_req))
			goto out;

		/* If we couldn't find an inode, we're done */
		if (found_key.type != BTRFS_INODE_ITEM_KEY)
			goto next;

		/* Mark our progress before we process the inode.
		 * This way, duet will ignore it as processed. */
		dctx->defrag_progress = found_key.objectid;

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		/* TODO: Check if we've already processed this inode out of
		 * order. If so, goto next one */
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		ret = process_inode(dctx, path, &found_key);
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

#ifdef CONFIG_BTRFS_DUET_DEFRAG
/*
 * This is the heart of synergistic defrag.
 *
 * Check that the inode belongs to the defrag_root. We do this by looking
 * up the btrfs_ino in defrag_root to make sure it can be found there.
 *
 * We need this function to:
 * - lookup the btrfs_ino in the RBBT; if it's in, then we've processed it
 *   already out of order
 * - lookup the btrfs_ino in the RBIT, and remove it
 * - if the event type is INSERT, and it was not in the RBIT, insert it;
 *   otherwise, update the existing info (increment count) and reinsert
 * - if the event type is REMOVE, and it was not in the RBIT, return;
 *   otherwise, update the existing info (decrement count) and only
 *   reinsert it if the count > 0
 *
 * Bonus: if we find any nodes during the lookup that are behind the current
 *        progress mark, we remove them to free up memory and purge the tree
 *
 * How to lookup an inode in the RBIT
 * 1. To find the number of pages in memory, use the page descriptor.
 *    (struct page).mapping -> (struct address_space).host -> (struct inode *)
 *                             (struct address_space).nrpages -> (unsigned long)
 * 2. To find the total number of pages in the inode, use the inode
 *    isize = i_size_read(inode);
 *    last_index = min(u64, isize-1) >> PAGE_CACHE_SHIFT;
 * 3. To find the inode number according to btrfs: BTRFS_I(inode) will return the
 *    struct btrfs_inode. Check btrfs_inode.h (e.g. btrfs_ino, 
 */
static void __handle_event(struct work_struct *work)
{
	struct defrag_synwork *dwork = (struct defrag_synwork *)work;
	struct itree_rbnode *itnode;

	/* TODO: Lookup the btrfs_ino in the RBBT */
	/* Bear in mind: because of hook placement, nrpages is up-to-date for
	 * INSERT events, but out-of-date for REMOVE events */

	/* TODO: Lookup, remove, and return itnode */

	switch (dwork->event_code) {
	case DUET_EVENT_CACHE_INSERT:
		/* TODO: Update/create itnode, and insert */
		break;
	case DUET_EVENT_CACHE_REMOVE:
		/* TODO: Update itnode, and insert if count > 0 */
		break;
	}

out:
	put_page(dwork->page);
	kfree((void *)work);
	return;	
}

/*
 * We're in interrupt context here, so we need to keep this short:
 * 1) Check that the event_code and data type are valid
 * 2) Check that the page host is part of our file system
 * 3) Check that this inode has not been processed already
 * We then proceed to enqueue the item, and we'll handle the rest
 * after interrupts are raised and we're scheduled back.
 */
static void btrfs_defrag_duet_handler(__u8 taskid, __u8 event_code,
		void *owner, __u64 offt, __u32 len, void *data,
		int data_type, void *privdata)
{
	struct page *page;
	struct inode *inode;
	struct defrag_ctx *dctx;
	struct defrag_synwork *dwork;

	/* Check that we have a reason to be here */
	if ((data_type != DUET_DATA_PAGE) ||
	    !(event_code & (DUET_EVENT_CACHE_INSERT|DUET_EVENT_CACHE_REMOVE)))
		return;

	/*
	 * Check that inode is in our file system. We still need to check
	 * that it belongs to the root we're defragging, but we'll have to
	 * leave that for when interrupts are back on.
	 */
	page = (struct page *)data;
	inode = (struct inode *)owner;
	dctx = (struct defrag_ctx *)privdata;

	if (inode->i_sb != dctx->sb) {
		printk(KERN_ERR "duet-defrag: superblock mismatch for inode "
			"%lu\n", inode->i_ino);
		return;
	}

	/*
	 * Check if we've processed this inode number. This check is not
	 * 100% correct, as the inode may:
	 * - have been processed out of order
	 * - not belong to our root
	 * but nonetheless, we might be able to throw away some work here
	 * and queue less work
	 */
	if (dctx->defrag_progress >= btrfs_ino(inode))
		return;

	/* We're good. Now enqueue a work item. */
	dwork = (struct defrag_synwork *)kzalloc(sizeof(struct defrag_synwork),
								GFP_ATOMIC);
	if (!dwork) {
		printk(KERN_ERR "duet-defrag: failed to allocate work item\n");
		return;
	}

	INIT_WORK((struct work_struct *)dwork, __handle_event);

	/* Populate synergistic work struct */
	dwork->dctx = dctx;
	dwork->btrfs_ino = btrfs_ino(inode);
	dwork->inmem_pages = page->mapping->nrpages;
	dwork->total_pages = (i_size_read(inode)-1) >> PAGE_CACHE_SHIFT;
	dwork->page = page;
	dwork->event_code = event_code;

	/* Get a hold on the page, so it doesn't go away. */
	get_page(page);

	spin_lock(&dctx->wq_lock);
	if (!dctx->syn_wq || queue_work(dctx->syn_wq,
			(struct work_struct *)dwork) != 1) {
		printk(KERN_ERR "duet-defrag: failed to queue up work\n");
		spin_unlock(&dctx->wq_lock);
		kfree(dwork);
		return;
	}
	spin_unlock(&dctx->wq_lock);

#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	printk(KERN_DEBUG "duet-defrag: Queued up work for defrag\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

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
			if (PTR_ERR(trans) != -ENOENT)
				return PTR_ERR(trans);
			/* ENOENT means theres no transaction */
		} else {
			ret = btrfs_commit_transaction(trans, defrag_root);
			if (ret)
				return ret;
		}
	} else {
		up_read(&defrag_root->fs_info->extent_commit_sem);
	}

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

#ifdef CONFIG_BTRFS_DUET_DEFRAG
	mutex_init(&dctx->itree_mutex);
	dctx->itree = RB_ROOT;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

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
	atomic64_set(&fs_info->defrag_start_jiffies, jiffies);
	fs_info->cur_defrag = dctx;
	/* Were we asked to cancel already? */
	if (atomic_read(&fs_info->defrag_cancel_req)) {
		mutex_unlock(&fs_info->defrag_lock);
		goto bail;
	}
	mutex_unlock(&fs_info->defrag_lock);

#ifdef CONFIG_BTRFS_DUET_DEFRAG
	spin_lock_init(&dctx->wq_lock);

	/* Out-of-order inode defrag will be put on this work queue */
	dctx->syn_wq = alloc_workqueue("duet-defrag", WQ_UNBOUND | WQ_HIGHPRI, 0);
	if (!dctx->syn_wq) {
		printk(KERN_ERR "defrag: failed to allocate work queue\n");
		ret = -EFAULT;
		goto bail;
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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	/* Let's first print out the bitmaps */
	duet_print_rbt(dctx->taskid);
	printk(KERN_DEBUG "defrag: total bytes defragged = %ld\n",
			atomic64_read(&fs_info->defrag_bytes_total));
	printk(KERN_DEBUG "defrag: bytes defragged best-effort: %ld\n",
			atomic64_read(&fs_info->defrag_bytes_best_effort));
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

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
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
		itree_dispose(&dctx->itree);
		mutex_destroy(&dctx->itree_mutex);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
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
