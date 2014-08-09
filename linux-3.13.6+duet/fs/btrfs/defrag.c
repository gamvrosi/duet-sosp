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
	struct inode *inode;
	u8 event_code;

	/* State used to find the inode tree node */
	u64 btrfs_ino;
	u64 inmem_pages;
	u64 total_pages;
	u8 inmem_ratio;
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
#define get_ratio(inmem, total)	((inmem) * 100) / (total)

static struct itree_rbnode *itnode_init(struct defrag_synwork *dwork)
{
	struct itree_rbnode *itnode = NULL;

	itnode = kzalloc(sizeof(*itnode), GFP_NOFS);
	if (!itnode)
		return NULL;

	RB_CLEAR_NODE(&itnode->node);
	itnode->btrfs_ino = dwork->btrfs_ino;
	itnode->inmem_pages = dwork->inmem_pages;
	itnode->total_pages = dwork->total_pages;
	itnode->inmem_ratio = get_ratio(dwork->inmem_pages, dwork->total_pages);
	return itnode;
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

#ifdef CONFIG_BTRFS_DUET_DEFRAG
/*
 * Picks an inode from the inode tree, and processes it, if it has not been
 * processed already.
 * Returns 1 if it processed an inode out of order, or 0 if it didn't.
 */
static int pick_inmem_inode(struct defrag_ctx *dctx)
{
	int ret;
	struct rb_node *node;
	struct itree_rbnode *itnode;
	struct btrfs_path *path;
	struct btrfs_key key;

	/* Pick an inode from the inode rbtree and remove it */
	mutex_lock(&dctx->itree_mutex);
	if (RB_EMPTY_ROOT(&dctx->itree)) {
		mutex_unlock(&dctx->itree_mutex);
		return 0;
	}

	/* We order from smallest to largest key so pick the largest */
	node = rb_last(&dctx->itree);
	if (!node) {
		mutex_unlock(&dctx->itree_mutex);
		return 0;
	}

	rb_erase(node, &dctx->itree);
	mutex_unlock(&dctx->itree_mutex);

	/* Check if it's been processed before */
	itnode = rb_entry(node, struct itree_rbnode, node);
	ret = duet_chk_done(dctx->taskid, 0, itnode->btrfs_ino, 1);
	if (ret == 1) {
		kfree(itnode);
		return 1;
	}

	/* Search for the inode in the defrag root */
	path = btrfs_alloc_path();
	if (!path) {
		kfree(itnode);
		return 0;
	}
	path->search_commit_root = 1;
	path->skip_locking = 1;

	key.objectid = itnode->btrfs_ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	ret = btrfs_search_slot(NULL, dctx->defrag_root, &key, path, 0, 0);
	if (ret) {
		ret = 0;
		goto out;
	}

	/* Mark, then process inode */
	ret = duet_mark_done(dctx->taskid, 0, key.objectid, 1);
	if (ret) {
		printk(KERN_ERR "duet: failed to mark inode %llu as "
			"defragged\n", key.objectid);
		ret = 0;
		goto out;
	}

	ret = process_inode(dctx, path, &key);
	if (ret < 0) {
		ret = 0;
		goto out;
	}

	ret = 1;
out:
	btrfs_free_path(path);
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
	struct btrfs_trans_handle *trans = NULL;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;
	path->search_commit_root = 1;
	path->skip_locking = 1;

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

	ret = btrfs_search_slot_for_read(defrag_root, &key, path, 1, 0);
	if (ret) {
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
		printk(KERN_INFO "btrfs defrag: btrfs_search_slot_for_read failed\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
		goto out;
	}

	while (1) {
		/* If we need to commit, end joined transaction and rejoin. */
		if (btrfs_should_end_transaction(trans, defrag_root)) {
			ret = btrfs_end_transaction(trans, defrag_root);
			trans = NULL;
			if (ret < 0)
				goto out;
			btrfs_release_path(path);
			goto join_trans;
		}

		/* Check if we've been asked to cancel */
		if (atomic_read(&fs_info->defrag_cancel_req)) {
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
			printk(KERN_INFO "btrfs defrag: we've been asked to cancel\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
			goto out;
		}

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		ret = pick_inmem_inode(dctx);
		if (ret)
			continue;
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		eb = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(eb, &found_key, slot);

		/* If we couldn't find an inode, we're done */
		if (found_key.type != BTRFS_INODE_ITEM_KEY)
			goto next;

		/* Mark our progress before we process the inode.
		 * This way, duet will ignore it as processed. */
		dctx->defrag_progress = found_key.objectid;

#ifdef CONFIG_BTRFS_DUET_DEFRAG
		/* Check if we've already processed this inode out of order */
		ret = duet_chk_done(dctx->taskid, 0, found_key.objectid, 1);
		if (ret == 1) {
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
			printk(KERN_INFO "btrfs defrag: skipping inode %llu\n",
				dctx->defrag_progress);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
			goto next;
		}

		/* We have not, so mark it as done before we process it */
		ret = duet_mark_done(dctx->taskid, 0, found_key.objectid, 1);
		if (ret) {
			printk(KERN_ERR "duet: failed to mark inode %llu as "
				"defragged\n", found_key.objectid);
			goto out;
		}
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

		ret = process_inode(dctx, path, &found_key);
		if (ret < 0) {
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
			printk(KERN_INFO "btrfs defrag: process_inode failed\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */
			goto out;
		}
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
		printk(KERN_INFO "btrfs defrag: processed inode %llu\n",
			dctx->defrag_progress);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

next:
		key.objectid = found_key.objectid + 1;
		key.type = found_key.type;
		key.offset = 0;

		ret = btrfs_next_item(defrag_root, path);
		if (ret < 0)
			goto out;
		if (ret) {
			ret = 0;
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
 * Looks up an itnode in the RBIT, and removes it from the tree if it's found.
 * Returns 1 if we found it (and the itnode in itn), 0 otherwise.
 *
 * How to lookup an inode in the RBIT
 * 1. The number of inmem_pages, as it is known by the tree, should already be
 *    in the page descriptor. We can only trust the page descriptor because our
 *    work queue is ordered, so INSERT events will be serviced as they get
 *    queued. XXX: Can we decouple this without creating a second RB tree?
 *    (page *)page->(address_space *)mapping->(inode *)host / (ulong)nrpages
 * 2. To find the total_pages in the inode, use the inode size: i_size_read()
 * 3. To find the inode number according to btrfs: btrfs_ino(inode)
 *
 * Bonus (TODO): if we find any nodes during the lookup that are behind the
 * current progress mark, remove them to free up memory and purge the tree
 */
static int lookup_remove_itnode(struct defrag_synwork *dwork,
				struct itree_rbnode **itn)
{
	int found = 0;
	struct rb_node *node = NULL;

	/* Make sure noone interferes while we search */
	mutex_lock(&dwork->dctx->itree_mutex);
	node = dwork->dctx->itree.rb_node;

	while (node) {
		*itn = rb_entry(node, struct itree_rbnode, node);

		/* We order based on (inmem_ratio, inmem_pages, btrfs_ino) */
		if ((*itn)->inmem_ratio > dwork->inmem_ratio) {
			node = node->rb_left;
		} else if ((*itn)->inmem_ratio < dwork->inmem_ratio) {
			node = node->rb_right;
		} else {
			/* Found inmem_ratio, look for inmem_pages */
			if ((*itn)->inmem_pages > dwork->inmem_pages) {
				node = node->rb_left;
			} else if ((*itn)->inmem_pages < dwork->inmem_pages) {
				node = node->rb_right;
			} else {
				/* Found inmem_pages, look for btrfs_ino */
				if ((*itn)->btrfs_ino > dwork->btrfs_ino) {
					node = node->rb_left;
				} else if ((*itn)->btrfs_ino < dwork->btrfs_ino) {
					node = node->rb_right;
				} else {
					found = 1;
					break;
				}
			}
		}
	}

#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	printk(KERN_DEBUG "duet-defrag: %s node with key (%u, %llu, %llu)\n",
		found ? "found" : "didn't find", (*itn)->inmem_ratio,
		(*itn)->inmem_pages, (*itn)->btrfs_ino);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

	/* If we found it, remove it */
	if (found)
		rb_erase(node, &dwork->dctx->itree);
	mutex_unlock(&dwork->dctx->itree_mutex);

	return found;
}

static int insert_itnode(struct defrag_synwork *dwork,
			 struct itree_rbnode *itn)
{
	int found = 0;
	struct rb_node **link, *parent = NULL;

	/* Make sure noone interferes while we search */
	mutex_lock(&dwork->dctx->itree_mutex);
	link = &dwork->dctx->itree.rb_node;

	while (*link) {
		parent = *link;
		itn = rb_entry(parent, struct itree_rbnode, node);

		/* We order based on (inmem_ratio, inmem_pages, btrfs_ino) */
		if (itn->inmem_ratio > dwork->inmem_ratio) {
			link = &(*link)->rb_left;
		} else if (itn->inmem_ratio < dwork->inmem_ratio) {
			link = &(*link)->rb_right;
		} else {
			/* Found inmem_ratio, look for inmem_pages */
			if (itn->inmem_pages > dwork->inmem_pages) {
				link = &(*link)->rb_left;
			} else if (itn->inmem_pages < dwork->inmem_pages) {
				link = &(*link)->rb_right;
			} else {
				/* Found inmem_pages, look for btrfs_ino */
				if (itn->btrfs_ino > dwork->btrfs_ino) {
					link = &(*link)->rb_left;
				} else if (itn->btrfs_ino < dwork->btrfs_ino) {
					link = &(*link)->rb_right;
				} else {
					found = 1;
					break;
				}
			}
		}
	}

#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	printk(KERN_DEBUG "duet-defrag: %s node with key (%u, %llu, %llu)\n",
		found ? "will not insert" : "will insert", itn->inmem_ratio,
		itn->inmem_pages, itn->btrfs_ino);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

	if (found)
		goto out;

	/* Insert node in RBtree */
	rb_link_node(&itn->node, parent, link);
	rb_insert_color(&itn->node, &dwork->dctx->itree);

out:
	mutex_unlock(&dwork->dctx->itree_mutex);
	return found;
}

/*
 * This is the heart of synergistic defrag.
 *
 * Check that the inode belongs to the defrag_root. We do this by looking
 * up the btrfs_ino in defrag_root to make sure it can be found there.
 *
 * We need this function to:
 * - lookup (inmem_ratio, total_pages, btrfs_ino) in the RBIT and remove it
 * - if this is an INSERT event, and the node was not in the RBIT, insert it;
 *   otherwise, increment page count and reinsert
 * - if this is a REMOVE event, and the node was not in the RBIT, return;
 *   otherwise, decrement page count and reinsert if inmem_pages > 0
 */
static void __handle_event(struct work_struct *work)
{
	int ret, found;
	struct defrag_synwork *dwork = (struct defrag_synwork *)work;
	struct itree_rbnode *itnode = NULL;

#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
	printk(KERN_DEBUG "duet-defrag: __handle_event started (%p)\n", (void *)work);
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

	/* Check if we've already processed this inode out of order (RBBT) */
	ret = duet_chk_done(dwork->dctx->taskid, 0, dwork->btrfs_ino, 1);
	if (ret == 1)
		goto out;

	switch (dwork->event_code) {
	case DUET_EVENT_CACHE_INSERT:
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
		printk(KERN_DEBUG "duet-defrag: insert event detected\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

#if 0
		/* Lookup itnode, and remove it */
		found = lookup_remove_itnode(dwork, &itnode);

		/* If the itnode doesn't exist, create it */
		if (!found)
			itnode = itnode_init(dwork);

		if (!itnode) {
			printk(KERN_ERR "duet-defrag: itnode not present\n");
			goto out;
		}

		/* Update the itnode */
		itnode->inmem_pages++;
		itnode->total_pages = (i_size_read(dwork->inode)-1) >>
							PAGE_CACHE_SHIFT;
		itnode->inmem_ratio = get_ratio(itnode->inmem_pages,
							itnode->total_pages);

		/* Insert the itnode */
		ret = insert_itnode(dwork, itnode);
		if (ret) {
			printk(KERN_ERR "duet-defrag: insert failed\n");
			kfree(itnode);
			goto out;
		}
#endif /* 0 */
		break;
	case DUET_EVENT_CACHE_REMOVE:
#ifdef CONFIG_BTRFS_DUET_DEFRAG_DEBUG
		printk(KERN_DEBUG "duet-defrag: remove event detected\n");
#endif /* CONFIG_BTRFS_DUET_DEFRAG_DEBUG */

#if 0
		/* Lookup itnode, and remove it */
		found = lookup_remove_itnode(dwork, &itnode);

		/* The itnode doesn't exist, bail */
		if (!found)
			goto out;

		/* The itnode exists, update it */
		itnode->inmem_pages--;
		itnode->total_pages = (i_size_read(dwork->inode)-1) >>
							PAGE_CACHE_SHIFT;
		itnode->inmem_ratio = get_ratio(itnode->inmem_pages,
							itnode->total_pages);

		/* Remove if needed, otherwise reinsert */
		if (!itnode->inmem_pages) {
			kfree(itnode);
			goto out;
		}

		ret = insert_itnode(dwork, itnode);
		if (ret) {
			printk(KERN_ERR "duet-defrag: insert failed\n");
			kfree(itnode);
			goto out;
		}
		break;
#endif /* 0 */
	}

out:
	put_page(dwork->page);
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
	dwork->total_pages = (i_size_read(inode)-1) >> PAGE_CACHE_SHIFT;
	dwork->inmem_ratio = get_ratio(dwork->inmem_pages, dwork->total_pages);
	dwork->page = page;
	dwork->inode = inode;
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

	/*
	 * Out-of-order inode defrag will be put on this work queue. It needs
	 * to be ordered, otherwise our counters will get messed up, as we're
	 * consulting the page descriptor to tell us things -- and if we
	 * process e.g. INSERT items out of order we may see a file shrinking
	 * when it's really growing
	 */
	dctx->syn_wq = alloc_ordered_workqueue("duet-defrag",
						WQ_UNBOUND | WQ_HIGHPRI);
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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
		itree_dispose(&dctx->itree);
		mutex_destroy(&dctx->itree_mutex);
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
