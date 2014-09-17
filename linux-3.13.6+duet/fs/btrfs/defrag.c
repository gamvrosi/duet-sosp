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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#include <linux/workqueue.h>
#include <linux/duet.h>
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

#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	atomic64_t rbit_total_time;
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
#ifdef CONFIG_BTRFS_DUET_DEFRAG
	spinlock_t wq_lock;
	struct workqueue_struct *syn_wq;

	/* red-black tree storing inodes with pages in cache */
	struct mutex inner_tree_mtx; /* inner mutex for picking */
	struct mutex outer_tree_mtx; /* outer mutex for updating */
	struct rb_root itree;

	__u8 taskid;
};

struct defrag_synwork {
	struct work_struct work;
	struct defrag_ctx *dctx;
	u8 event_code;

	/* State used to find the inode tree node */
	u64 btrfs_ino;
	u64 inmem_pages;
	u64 total_pages;
};

struct itree_rbnode {
	struct rb_node node;
	__u64 btrfs_ino;
	__u64 inmem_pages;
	__u64 total_pages;
	__u8 inmem_ratio;	/* pages (out of 100) currently in memory */
#endif /* CONFIG_BTRFS_DUET_DEFRAG */
};

#ifdef CONFIG_BTRFS_DUET_DEFRAG
#define get_ratio(ret, inmem, total)	\
	do {				\
		ret = (inmem) * 100;	\
		do_div(ret, total);	\
	} while (0);

/* Preps the itnode for insert. Note that we also increment nrpages! */
static int itnode_init(struct itree_rbnode **itn)
{
	*itn = kzalloc(sizeof(**itn), GFP_NOFS);
	if (!(*itn))
		return 1;

	RB_CLEAR_NODE(&(*itn)->node);
	return 0;
}

static void itree_dispose(struct rb_root *root)
{
	struct rb_node *rbnode;
	struct itree_rbnode *itnode;

	while (!RB_EMPTY_ROOT(root)) {
		rbnode = rb_first(root);
		itnode = rb_entry(rbnode, struct itree_rbnode, node);
		rb_erase(rbnode, root);
		kfree(itnode);
	}

	return;
}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

static int defrag_inode(struct inode *inode, struct defrag_ctx *dctx,
			int out_of_order)
{
	int ret;
	unsigned long cache_hits, dirty_pages;
	struct btrfs_fs_info *fs_info = dctx->defrag_root->fs_info;

	//sb_start_write(fs_info->sb);
	ret = btrfs_defrag_file_trace(inode, NULL, &dctx->range,
				0, 0, &cache_hits, &dirty_pages);
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
 * Picks an inode from the inode tree and processes it, if it has not been
 * processed already.
 * Returns 1 if it processed an inode out of order, or 0 if it didn't.
 */
static int pick_inmem_inode(struct defrag_ctx *dctx)
{
	int removed=0, ret=0;
	struct rb_node *node;
	struct itree_rbnode *itnode;
	struct btrfs_key key;
	struct inode *inode;
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	ktime_t start, finish;
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */

	if (RB_EMPTY_ROOT(&dctx->itree)) {
		defrag_dbg(KERN_DEBUG "duet-defrag: nothing to pick");
		return 0;
	}

#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	start = ktime_get();
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
	/* Pick an inode from the inode rbtree and remove it */
	mutex_lock(&dctx->inner_tree_mtx);
	if (RB_EMPTY_ROOT(&dctx->itree)) {
		defrag_dbg(KERN_DEBUG "duet-defrag: nothing to pick");
		mutex_unlock(&dctx->inner_tree_mtx);
		return 0;
	}

again:
	/* We order from smallest to largest key so pick the largest */
	node = rb_last(&dctx->itree);
	if (!node) {
		mutex_unlock(&dctx->inner_tree_mtx);
		return 0;
	}

	rb_erase(node, &dctx->itree);

	/* Check if it's been processed before */
	itnode = rb_entry(node, struct itree_rbnode, node);
	ret = duet_chk_done(dctx->taskid, 0, itnode->btrfs_ino, 1);
	if (ret == 1) {
		kfree(itnode);
		removed++;
		goto again;
	}
	mutex_unlock(&dctx->inner_tree_mtx);
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	finish = ktime_get();
	atomic64_add(ktime_us_delta(finish, start), &dctx->rbit_total_time);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */

	printk(KERN_DEBUG "duet-defrag: picked key (%u, %llu/%llu, %llu) -- removed %d",
		itnode->inmem_ratio, itnode->inmem_pages,
		itnode->total_pages, itnode->btrfs_ino, removed);

	key.objectid = itnode->btrfs_ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	/* Get the inode */
	inode = btrfs_iget(dctx->defrag_root->fs_info->sb, &key,
						dctx->defrag_root, NULL);
	if (IS_ERR(inode)) {
		printk(KERN_ERR "btrfs defrag: pick_inmem iget failed\n");
		ret = 0;
		goto out;
	}

	/* We only process regular files */
	if ((inode->i_mode & S_IFMT) != S_IFREG) {
		ret = 0;
		goto iput_out;
	}

	/* Mark as done */
	ret = duet_mark_done(dctx->taskid, 0, key.objectid, 1);
	if (ret) {
		printk(KERN_ERR "duet: failed to mark inode %llu as "
			"defragged\n", key.objectid);
		ret = 0;
		goto iput_out;
	}

	ret = defrag_inode(inode, dctx, 1);
	if (ret) {
		printk(KERN_ERR "btrfs defrag: file defrag failed\n");
		ret = 0;
		goto iput_out;
	}

	printk(KERN_INFO "duet-defrag: processed inode %llu out of order\n",
			key.objectid);

	ret = 1;
iput_out:
	iput(inode);
out:
	kfree(itnode);
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
		if (duet_is_online() && pick_inmem_inode(dctx))
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
		if (duet_is_online() && duet_chk_done(dctx->taskid, 0,
		    found_key.objectid, 1) == 1) {
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
		if (duet_is_online() && duet_mark_done(dctx->taskid, 0,
		    found_key.objectid, 1)) {
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

#ifdef CONFIG_BTRFS_DUET_DEFRAG
/*
 * Inserts an itnode in the RBIT tree. Assumes that you've obtained the
 * relevant locks. Returns 1 on failure.
 */
static int insert_itnode(struct defrag_synwork *dwork,
			 struct itree_rbnode *itn)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;
	struct itree_rbnode *cur;

	link = &dwork->dctx->itree.rb_node;

	while (*link) {
		parent = *link;
		cur = rb_entry(parent, struct itree_rbnode, node);

		/* We order based on (inmem_ratio, btrfs_ino) */
		if (cur->inmem_ratio > itn->inmem_ratio) {
			link = &(*link)->rb_left;
		} else if (cur->inmem_ratio < itn->inmem_ratio) {
			link = &(*link)->rb_right;
		} else {
			/* Found inmem_ratio, look for btrfs_ino */
			if (cur->btrfs_ino > itn->btrfs_ino) {
				link = &(*link)->rb_left;
			} else if (cur->btrfs_ino < itn->btrfs_ino) {
				link = &(*link)->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	defrag_dbg(KERN_DEBUG "duet-defrag: %s node (%u, %llu/%llu, %llu)\n",
		found ? "will not insert" : "will insert", itn->inmem_ratio,
		itn->inmem_pages, itn->total_pages, itn->btrfs_ino);

	if (found)
		goto out;

	/* Insert node in RBtree */
	rb_link_node(&itn->node, parent, link);
	rb_insert_color(&itn->node, &dwork->dctx->itree);

out:
	return found;
}

/*
 * Looks up an itnode in the RBIT, and updates it; this may entail inserting
 * or removing it, depending on the event type.
 * TODO: if we find any nodes during the lookup that are behind the current
 * progress mark, remove them to free up memory and purge the tree
 */
static int update_itree(struct defrag_synwork *dwork)
{
	int ret=0, found=0;
	struct rb_node *node = NULL;
	struct itree_rbnode *itn = NULL;
	u8 cur_inmem_ratio, new_inmem_ratio;
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	ktime_t start, finish;
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */

	/* Calculate the current inmem ratio, and the updated one */
	get_ratio(cur_inmem_ratio, dwork->inmem_pages, dwork->total_pages);
	get_ratio(new_inmem_ratio, dwork->inmem_pages +
		(dwork->event_code == DUET_EVENT_CACHE_INSERT ? 1 : -1),
		dwork->total_pages);

	/* First, look up the itnode in the RBIT */
	/* Make sure noone interferes while we search */
	mutex_lock(&dwork->dctx->outer_tree_mtx);
	mutex_lock(&dwork->dctx->inner_tree_mtx);
	node = dwork->dctx->itree.rb_node;

#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	start = ktime_get();
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
	while (node) {
		itn = rb_entry(node, struct itree_rbnode, node);

		/* We order based on (inmem_ratio, btrfs_ino) */
		if (itn->inmem_ratio > cur_inmem_ratio) {
			node = node->rb_left;
		} else if (itn->inmem_ratio < cur_inmem_ratio) {
			node = node->rb_right;
		} else {
			/* Found inmem_ratio, look for btrfs_ino */
			if (itn->btrfs_ino > dwork->btrfs_ino) {
				node = node->rb_left;
			} else if (itn->btrfs_ino < dwork->btrfs_ino) {
				node = node->rb_right;
			} else {
				found = 1;
				break;
			}
		}
	}

	defrag_dbg(KERN_DEBUG "duet-defrag: %s node (%u [%llu/%llu], %llu)\n",
		found ? "found" : "didn't find",
		found ? itn->inmem_ratio : cur_inmem_ratio,
		found ? itn->inmem_pages : dwork->inmem_pages,
		found ? itn->total_pages : dwork->total_pages,
		found ? itn->btrfs_ino : dwork->btrfs_ino);

	/* If we didn't find it and should create/insert, do so and return */
	if (!found && dwork->event_code == DUET_EVENT_CACHE_INSERT) {
		if (itnode_init(&itn)) {
			printk(KERN_ERR "duet-defrag: itnode alloc failed\n");
			ret = 1;
			goto out;
		}

		/* Update the itnode */
		itn->btrfs_ino = dwork->btrfs_ino;
		itn->inmem_pages = dwork->inmem_pages + 1;
		itn->total_pages = dwork->total_pages;
		itn->inmem_ratio = new_inmem_ratio;

		ret = insert_itnode(dwork, itn);
		if (ret) {
			printk(KERN_ERR "duet-defrag: insert failed\n");
			kfree(itn);
		}
	} else if (found) {
		/* Update the itnode */
		itn->btrfs_ino = dwork->btrfs_ino;
		itn->inmem_pages = dwork->inmem_pages +
			(dwork->event_code == DUET_EVENT_CACHE_INSERT ? 1 : -1);
		itn->total_pages = dwork->total_pages;
		itn->inmem_ratio = new_inmem_ratio;

		/* Did the number of pages reach zero? Then remove */
		if (itn->inmem_pages == 0) {
			rb_erase(node, &dwork->dctx->itree);
			kfree(itn);
			goto out;
		}

		/* If the ratio has changed, erase and reinsert */
		if (new_inmem_ratio != cur_inmem_ratio) {
			rb_erase(node, &dwork->dctx->itree);
			ret = insert_itnode(dwork, itn);
			if (ret) {
				printk(KERN_ERR "duet-defrag: insert failed\n");
				kfree(itn);
			}
		}
	}

out:
#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	finish = ktime_get();
	atomic64_add(ktime_us_delta(finish, start),
			&dwork->dctx->rbit_total_time);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
	mutex_unlock(&dwork->dctx->inner_tree_mtx);
	mutex_unlock(&dwork->dctx->outer_tree_mtx);
	return ret;
}

/* Check that the inode has not been processed already, and update the RBIT */
static void __handle_page_event(struct work_struct *work)
{
	int ret;
	struct defrag_synwork *dwork = (struct defrag_synwork *)work;

	defrag_dbg(KERN_DEBUG "duet-defrag: %s (%llu/%llu, %llu)\n",
		dwork->event_code == DUET_EVENT_CACHE_INSERT ? "insert" :
		"remove", dwork->inmem_pages, dwork->total_pages,
		dwork->btrfs_ino);

	/* Check if we've already processed this inode out of order (RBBT) */
	ret = duet_chk_done(dwork->dctx->taskid, 0, dwork->btrfs_ino, 1);
	if (ret == 1)
		goto out;

	ret = update_itree(dwork);
	if (ret)
		printk(KERN_ERR "duet-defrag: error updating the itree\n");

out:
	kfree((void *)work);
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
		defrag_dbg(KERN_ERR "duet-defrag: superblock mismatch for inode "
			"%lu\n", inode->i_ino);
		return;
	}

	if (!i_size_read(inode)) {
		defrag_dbg(KERN_ERR "duet-defrag: found zero-size inode\n");
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

	INIT_WORK((struct work_struct *)dwork, __handle_page_event);

	/*
	 * Populate synergistic work struct. Note that due to hook placement,
	 * nrpages is already incremented for INSERTs, but not decremented
	 * for REMOVE events.
	 */
	dwork->dctx = dctx;
	dwork->btrfs_ino = btrfs_ino(inode);
	dwork->inmem_pages = page->mapping->nrpages;
	if (event_code == DUET_EVENT_CACHE_INSERT)
		dwork->inmem_pages--;
	dwork->total_pages = ((i_size_read(inode)-1) >> PAGE_CACHE_SHIFT) + 1;
	dwork->event_code = event_code;

	spin_lock(&dctx->wq_lock);
	if (!dctx->syn_wq || queue_work(dctx->syn_wq,
			(struct work_struct *)dwork) != 1) {
		printk(KERN_ERR "duet-defrag: failed to queue up work\n");
		spin_unlock(&dctx->wq_lock);
		kfree(dwork);
		return;
	}
	spin_unlock(&dctx->wq_lock);
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

#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	atomic64_set(&dctx->rbit_total_time, 0);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */
#ifdef CONFIG_BTRFS_DUET_DEFRAG
	mutex_init(&dctx->inner_tree_mtx);
	mutex_init(&dctx->outer_tree_mtx);
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
	spin_lock_init(&dctx->wq_lock);

	/*
	 * Out-of-order inode defrag will be put on this work queue. It needs
	 * to be ordered, otherwise our counters will get messed up, as we're
	 * consulting the page descriptor to tell us things -- and if we
	 * process e.g. INSERT items out of order we may see a file shrinking
	 * when it's really growing
	 * XXX: Would WQ_HIGHPRI make sense in this case?
	 */
	//dctx->syn_wq = alloc_workqueue("duet-defrag", WQ_UNBOUND, 0);
	dctx->syn_wq = alloc_ordered_workqueue("duet-defrag", 0);
	if (!dctx->syn_wq) {
		printk(KERN_ERR "defrag: failed to allocate work queue\n");
		ret = -EFAULT;
		goto bail;
	}

	/* Register the task with the Duet framework -- every bit represents
	 * one inode number, and we map 32768 * 8 = 262144 inodes per node */
	if (duet_task_register(&dctx->taskid, "btrfs-defrag", 1, 32768,
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
	printk(KERN_DEBUG "defrag: bytes found already in memory: %ld\n",
			atomic64_read(&fs_info->defrag_bytes_from_mem));
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

	/* Flush and destroy work queue */
	spin_lock(&dctx->wq_lock);
	tmp_wq = dctx->syn_wq;
	dctx->syn_wq = NULL;
	spin_unlock(&dctx->wq_lock);
	flush_workqueue(tmp_wq);
	destroy_workqueue(tmp_wq);

#ifdef CONFIG_BTRFS_DUET_DEFRAG_CPUMON
	printk(KERN_DEBUG "defrag: CPU time spent updating the RBIT: %llds\n",
		(long long) div64_u64(atomic64_read(&dctx->rbit_total_time), 1E6));
#endif /* CONFIG_BTRFS_DUET_DEFRAG_CPUMON */

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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
		itree_dispose(&dctx->itree);
		mutex_destroy(&dctx->inner_tree_mtx);
		mutex_destroy(&dctx->outer_tree_mtx);
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
