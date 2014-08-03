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
#ifdef CONFIG_BTRFS_DUET_DEFRAG
#include <linux/workqueue.h>
#include <linux/duet.h>
#endif /* CONFIG_BTRFS_DUET_DEFRAG */

struct defrag_ctx {
	struct btrfs_root *defrag_root;
	u64 defrag_progress;

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

#if 0
int btrfs_defrag_file(struct inode *inode, struct file *file,
		      struct btrfs_ioctl_defrag_range_args *range,
		      u64 newer_than, unsigned long max_to_defrag)
{
	struct btrfs_root *root = BTRFS_I(inode)->root;
	struct file_ra_state *ra = NULL;
	unsigned long last_index;
	u64 isize = i_size_read(inode);
	u64 last_len = 0;
	u64 skip = 0;
	u64 defrag_end = 0;
	u64 newer_off = range->start;
	unsigned long i;
	unsigned long ra_index = 0;
        int ret;
        int defrag_count = 0;
        int compress_type = BTRFS_COMPRESS_ZLIB;
        int extent_thresh = range->extent_thresh;
        int max_cluster = (256 * 1024) >> PAGE_CACHE_SHIFT;
        int cluster = max_cluster;
        u64 new_align = ~((u64)128 * 1024 - 1);
        struct page **pages = NULL;

        if (isize == 0)
                return 0;

        if (range->start >= isize)
                return -EINVAL;

        if (range->flags & BTRFS_DEFRAG_RANGE_COMPRESS) {
                if (range->compress_type > BTRFS_COMPRESS_TYPES)
                        return -EINVAL;
                if (range->compress_type)
                        compress_type = range->compress_type;
        }

        if (extent_thresh == 0)
                extent_thresh = 256 * 1024;

        /*
         * if we were not given a file, allocate a readahead
         * context
         */
        if (!file) {
                ra = kzalloc(sizeof(*ra), GFP_NOFS);
                if (!ra)
                        return -ENOMEM;
                file_ra_state_init(ra, inode->i_mapping);
        } else {
                ra = &file->f_ra;
        }

        pages = kmalloc_array(max_cluster, sizeof(struct page *),
                        GFP_NOFS);
        if (!pages) {
                ret = -ENOMEM;
                goto out_ra;
        }

        /* find the last page to defrag */
        if (range->start + range->len > range->start) {
                last_index = min_t(u64, isize - 1,
                         range->start + range->len - 1) >> PAGE_CACHE_SHIFT;
        } else {
                last_index = (isize - 1) >> PAGE_CACHE_SHIFT;
        }

        if (newer_than) {
                ret = find_new_extents(root, inode, newer_than,
                                       &newer_off, 64 * 1024);
                if (!ret) {
                        range->start = newer_off;
                        /*
                         * we always align our defrag to help keep
                         * the extents in the file evenly spaced
                         */
                        i = (newer_off & new_align) >> PAGE_CACHE_SHIFT;
                } else
                        goto out_ra;
        } else {
                i = range->start >> PAGE_CACHE_SHIFT;
        }
        if (!max_to_defrag)
                max_to_defrag = last_index + 1;

        /*
         * make writeback starts from i, so the defrag range can be
         * written sequentially.
         */
        if (i < inode->i_mapping->writeback_index)
                inode->i_mapping->writeback_index = i;

        while (i <= last_index && defrag_count < max_to_defrag &&
               (i < (i_size_read(inode) + PAGE_CACHE_SIZE - 1) >>
                PAGE_CACHE_SHIFT)) {
                /*
                 * make sure we stop running if someone unmounts
                 * the FS
                 */
                if (!(inode->i_sb->s_flags & MS_ACTIVE))
                        break;

                if (btrfs_defrag_cancelled(root->fs_info)) {
                        printk(KERN_DEBUG "btrfs: defrag_file cancelled\n");
                        ret = -EAGAIN;
                        break;
                }

                if (!should_defrag_range(inode, (u64)i << PAGE_CACHE_SHIFT,
                                         extent_thresh, &last_len, &skip,
                                         &defrag_end, range->flags &
                                         BTRFS_DEFRAG_RANGE_COMPRESS)) {
                        unsigned long next;
                        /*
                         * the should_defrag function tells us how much to skip
                         * bump our counter by the suggested amount
                         */
                        next = (skip + PAGE_CACHE_SIZE - 1) >> PAGE_CACHE_SHIFT;
                        i = max(i + 1, next);
                        continue;
                }

                if (!newer_than) {
                        cluster = (PAGE_CACHE_ALIGN(defrag_end) >>
                                   PAGE_CACHE_SHIFT) - i;
                        cluster = min(cluster, max_cluster);
                } else {
                        cluster = max_cluster;
                }

                if (i + cluster > ra_index) {
                        ra_index = max(i, ra_index);
                        btrfs_force_ra(inode->i_mapping, ra, file, ra_index,
                                       cluster);
                        ra_index += max_cluster;
                }

                mutex_lock(&inode->i_mutex);
                if (range->flags & BTRFS_DEFRAG_RANGE_COMPRESS)
                        BTRFS_I(inode)->force_compress = compress_type;
                ret = cluster_pages_for_defrag(inode, pages, i, cluster);
                if (ret < 0) {
                        mutex_unlock(&inode->i_mutex);
                        goto out_ra;
                }

                defrag_count += ret;
                balance_dirty_pages_ratelimited(inode->i_mapping);
                mutex_unlock(&inode->i_mutex);

                if (newer_than) {
                        if (newer_off == (u64)-1)
                                break;

                        if (ret > 0)
                                i += ret;

                        newer_off = max(newer_off + 1,
                                        (u64)i << PAGE_CACHE_SHIFT);

                        ret = find_new_extents(root, inode,
                                               newer_than, &newer_off,
                                               64 * 1024);
                        if (!ret) {
                                range->start = newer_off;
                                i = (newer_off & new_align) >> PAGE_CACHE_SHIFT;
                        } else {
                                break;
                        }
                } else {
                        if (ret > 0) {
                                i += ret;
                                last_len += ret << PAGE_CACHE_SHIFT;
                        } else {
                                i++;
                                last_len = 0;
                        }
                }
        }

        if ((range->flags & BTRFS_DEFRAG_RANGE_START_IO))
                filemap_flush(inode->i_mapping);

        if ((range->flags & BTRFS_DEFRAG_RANGE_COMPRESS)) {
                /* the filemap_flush will queue IO into the worker threads, but
                 * we have to make sure the IO is actually started and that
                 * ordered extents get created before we return
                 */
                atomic_inc(&root->fs_info->async_submit_draining);
                while (atomic_read(&root->fs_info->nr_async_submits) ||
                      atomic_read(&root->fs_info->async_delalloc_pages)) {
                        wait_event(root->fs_info->async_submit_wait,
                           (atomic_read(&root->fs_info->nr_async_submits) == 0 &&
                            atomic_read(&root->fs_info->async_delalloc_pages) == 0));
                }
                atomic_dec(&root->fs_info->async_submit_draining);
        }

        if (range->compress_type == BTRFS_COMPRESS_LZO) {
                btrfs_set_fs_incompat(root->fs_info, COMPRESS_LZO);
        }

        ret = defrag_count;

out_ra:
        if (range->flags & BTRFS_DEFRAG_RANGE_COMPRESS) {
                mutex_lock(&inode->i_mutex);
                BTRFS_I(inode)->force_compress = BTRFS_COMPRESS_NONE;
                mutex_unlock(&inode->i_mutex);
        }
        if (!file)
                kfree(ra);
        kfree(pages);
        return ret;
}
#endif /* 0 */

static int process_inode(struct defrag_ctx *dctx, struct btrfs_path *path)
{
	int s, ret = 0;
	struct extent_buffer *l;
	struct btrfs_inode_item *ii;
#if 0
	struct btrfs_ioctl_defrag_range_args *range;

	range = kzalloc
	range->len = (u64)-1;
#endif

	/* Grab the inode item */
	l = path->nodes[0];
	s = path->slots[0];
	ii = btrfs_item_ptr(l, s, struct btrfs_inode_item);

	/* We will only process regular files */
	switch (btrfs_inode_mode(l, ii) & S_IFMT) {
	case S_IFREG:
		/* TODO: Get the file's inode to call file_defrag() */
#if 0
		ret = btrfs_defrag_file(file_inode(file), file, range, 0, 0);
#endif
		break;
	case S_IFDIR:
	case S_IFLNK:
	default:
		ret = 1;
		break;
	}

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

		eb = path->nodes[0];
		slot = path->slots[0];
		btrfs_item_key_to_cpu(eb, &found_key, slot);

		/* Check if we've been asked to cancel */
		if (atomic_read(&fs_info->defrag_cancel_req))
			goto out;

		/* If we couldn't find an inode, we're done */
		if (found_key.type != BTRFS_INODE_ITEM_KEY)
			goto next;

		ret = process_inode(dctx, path);
		if (ret < 0)
			goto out;

		/* Mark our progress */
		dctx->defrag_progress = found_key.objectid;

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

	if (btrfs_root_readonly(defrag_root)) {
		ret = -EROFS;
		goto out;
	}

	/* Check if a defrag is already running, and terminate if so */
	mutex_lock(&fs_info->defrag_lock);
	if (atomic_read(&fs_info->defrag_fs_running)) {
		mutex_unlock(&fs_info->defrag_lock);
		ret = -EINPROGRESS;
		goto out;
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
