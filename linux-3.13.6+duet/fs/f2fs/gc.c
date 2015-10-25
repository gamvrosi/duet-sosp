/*
 * fs/f2fs/gc.c
 *
 * Copyright (c) 2012 Samsung Electronics Co., Ltd.
 *             http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/init.h>
#include <linux/f2fs_fs.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/freezer.h>
#include <linux/blkdev.h>
#ifdef CONFIG_F2FS_DUET_GC
#include <linux/duet.h>
#include <linux/rbtree.h>
#include <linux/ktime.h>
#endif /* CONFIG_F2FS_DUET_GC */

#include "f2fs.h"
#include "node.h"
#include "segment.h"
#include "gc.h"
#include <trace/events/f2fs.h>

static struct kmem_cache *winode_slab;

#ifdef CONFIG_F2FS_DUET_GC
struct blkaddr_tree_node {
	struct rb_node rbn;
	unsigned long ino;
	unsigned long idx;
	block_t blkaddr;
};

struct rb_root blkaddr_tree;

static struct rb_node **blkaddr_tree_search(unsigned long ino, unsigned long idx,
		struct rb_node **ret_par)
{
	struct rb_node *par, **cur;
	struct blkaddr_tree_node *batn;

	par = NULL;
	cur = &blkaddr_tree.rb_node;
	while (*cur) {
		batn = rb_entry(*cur, struct blkaddr_tree_node, rbn);

		par = *cur;
		if (ino < batn->ino) {
			cur = &(*cur)->rb_left;
		} else if (ino > batn->ino) {
			cur = &(*cur)->rb_right;
		} else { /* Found ino */
			if (idx < batn->idx)
				cur = &(*cur)->rb_left;
			else if (idx > batn->idx)
				cur = &(*cur)->rb_right;
			else /* Found ino and idx */
				break;
		}
	}

	*ret_par = par;
	return cur;
}

static void blkaddr_tree_destroy(void)
{
	struct rb_node *node;
	struct blkaddr_tree_node *batn;

	while (!RB_EMPTY_ROOT(&blkaddr_tree)) {
		node = rb_first(&blkaddr_tree);
		batn = rb_entry(node, struct blkaddr_tree_node, rbn);
		rb_erase(node, &blkaddr_tree);
		kfree(batn);
	}
}

static struct seg_entry* get_seg_entry_from_blkaddr(struct f2fs_sb_info *sbi,
		block_t blkaddr)
{
	struct seg_entry *se;
	unsigned int segno;

	se = NULL;
	segno = GET_SEGNO(sbi, blkaddr);
	if (segno == NULL_SEGNO) {
		/* blkaddr == NEW_ADDR, ignore */
	} else if (segno >= TOTAL_SEGS(sbi)) {
		f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
			"segno %u out of range of [0-%u)\n",
			segno, TOTAL_SEGS(sbi));
	} else {
		se = get_seg_entry(sbi, segno);
	}

	return se;
}

static void inc_seg_page_counter(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct seg_entry *se;

	se = get_seg_entry_from_blkaddr(sbi, blkaddr);
	if (se) {
		if (se->page_cached_blocks >= sbi->blocks_per_seg)
			f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
					"counters are inconsistent\n");
		else
			se->page_cached_blocks++;
	}
}

static void dec_seg_page_counter(struct f2fs_sb_info *sbi, block_t blkaddr)
{
	struct seg_entry *se;

	se = get_seg_entry_from_blkaddr(sbi, blkaddr);
	if (se) {
		if (se->page_cached_blocks == 0)
			f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
					"counters are inconsistent.\n");
		else
			se->page_cached_blocks--;
	}
}

static block_t get_blkaddr_from_ino(struct f2fs_sb_info *sbi,
	unsigned long ino, unsigned long idx)
{
	struct node_info ni;
	struct dnode_of_data dn;
	struct inode *inode;
	block_t blkaddr;
	int err;

	blkaddr = NULL_ADDR;

	/* Get the inode */
	inode = f2fs_iget(sbi->sb, ino);
	if (IS_ERR(inode)) {
		f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
				"f2fs_iget error.\n");
		goto out;
	}

	/* Get the block address */
	if (inode->i_ino == F2FS_NODE_INO(sbi)) { /* Node page */
		get_node_info(sbi, idx, &ni);
		blkaddr = ni.blk_addr;
	} else { /* Data page */
		f2fs_lock_op(sbi);
		set_new_dnode(&dn, inode, NULL, NULL, 0);
		err = get_dnode_of_data(&dn, idx, LOOKUP_NODE);
		if (err) {
			f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
					"get_dnode_of_data error.\n");
		} else {
			blkaddr = dn.data_blkaddr;
			f2fs_put_dnode(&dn);
		}
		f2fs_unlock_op(sbi);
	}
	iput(inode);

out:
	return blkaddr;
}

/** Flushlist functions **/

struct flushlist_node {
	struct list_head list;
	unsigned long ino;
	unsigned long idx;
	block_t blkaddr;
};

struct list_head flushlist;

static int flushlist_add(unsigned long ino, unsigned long idx, block_t blkaddr)
{
	struct flushlist_node *fln;

	fln = kzalloc(sizeof(struct flushlist_node), GFP_NOFS);
	if (!fln)
		return -ENOMEM;

	INIT_LIST_HEAD(&fln->list);
	fln->ino = ino;
	fln->idx = idx;
	fln->blkaddr = blkaddr;

	list_add(&fln->list, &flushlist);

	return 0;
}

static int blkaddr_lookup_update(struct f2fs_sb_info *sbi,
	unsigned long ino, unsigned long idx);
static void flushlist_update(struct f2fs_sb_info *sbi)
{
	struct flushlist_node *fln, *next;
	block_t new_blkaddr;
	int err = 0;
	struct rb_node *parent, **link;
	struct blkaddr_tree_node *batn;
	int ret;

	list_for_each_entry_safe(fln, next, &flushlist, list) {
		new_blkaddr = get_blkaddr_from_ino(sbi, fln->ino, fln->idx);
		if (new_blkaddr == NULL_ADDR) {
			f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
					"flushlist_update: new_blkaddr NULL\n");
			goto delete_node;
		}

		if (new_blkaddr == fln->blkaddr) /* Still hasn't been flushed */
			continue;

		parent = NULL;
		ret = 0;

		link = blkaddr_tree_search(fln->ino, fln->idx, &parent);
		if (!(*link)) { /* Node not in tree */
			ret = -EINVAL;
			goto err;
		}
		batn = rb_entry(*link, struct blkaddr_tree_node, rbn);
		batn->blkaddr = new_blkaddr;

err:
		if (!err) {
			/*
			 * XXX: This could probably cause inconsistencies in 
			 * certain scenarios. Think about it more if it's an
			 * issue. 
			 */
			//dec_seg_page_counter(sbi, fln->blkaddr);
			inc_seg_page_counter(sbi, new_blkaddr);
		}

delete_node:
		list_del(&fln->list);
		kfree(fln);
	}

	return;
}

static void flushlist_destroy(void)
{
	struct flushlist_node *fln, *next;

	list_for_each_entry_safe(fln, next, &flushlist, list) {
		list_del(&fln->list);
		kfree(fln);
	}

	return;
}

/** blkaddr tree functions **/

static int blkaddr_lookup_insert(struct f2fs_sb_info *sbi,
	unsigned long ino, unsigned long idx)
{
	struct rb_node *parent, **link;
	struct blkaddr_tree_node *new_batn;
	block_t blkaddr = NULL_ADDR;

	parent = NULL;

	link = blkaddr_tree_search(ino, idx, &parent);
	if (*link) {
		/* Nothing to do; page already there */
		return 0;
	}

	blkaddr = get_blkaddr_from_ino(sbi, ino, idx);
	if (blkaddr == NULL_ADDR) {
		f2fs_duet_debug(KERN_ERR "duet-gc: blkaddr_lookup_insert "
				"new_blkaddr is NULL.\n");
		return 0;
	}

	new_batn = kzalloc(sizeof(struct blkaddr_tree_node), GFP_NOFS);
	if (!new_batn)
		return -ENOMEM;

	new_batn->ino = ino;
	new_batn->idx = idx;
	new_batn->blkaddr = blkaddr;

	inc_seg_page_counter(sbi, blkaddr);
	rb_link_node(&new_batn->rbn, parent, link);
	rb_insert_color(&new_batn->rbn, &blkaddr_tree);

	return 0;
}

static int blkaddr_lookup_update(struct f2fs_sb_info *sbi,
	unsigned long ino, unsigned long idx)
{
	struct rb_node *parent, **link;
	struct blkaddr_tree_node *batn;
	block_t blkaddr = NULL_ADDR;
	int ret;

	parent = NULL;
	link = blkaddr_tree_search(ino, idx, &parent);
	if (!(*link)) {
		/*
		 * This can (and does) happen if a page is added, removed, and
		 * flushed (added/removed cancel each other, so all we hear
		 * about is the flush).
		 */
		return 0;
	}

	batn = rb_entry(*link, struct blkaddr_tree_node, rbn);
	dec_seg_page_counter(sbi, batn->blkaddr);

	/* Now work on updating the new blkaddr */
	blkaddr = get_blkaddr_from_ino(sbi, ino, idx);

	/* Did we beat the flusher thread here */
	if (blkaddr == batn->blkaddr) {
		ret = flushlist_add(ino, idx, batn->blkaddr);
		if (ret)
			f2fs_duet_debug(KERN_ERR "duet-gc: flushlist_add error\n");
		return -EINVAL;
	}

	batn->blkaddr = blkaddr;
	inc_seg_page_counter(sbi, blkaddr);
	return 0;
}

static int blkaddr_lookup_remove(struct f2fs_sb_info *sbi,
	unsigned long ino, unsigned long idx)
{
	struct rb_node *parent = NULL, **link;
	struct blkaddr_tree_node *batn;

	link = blkaddr_tree_search(ino, idx, &parent);
	if (!(*link)) {
		/* Node not in tree */
		return 0;
	}

	batn = rb_entry(*link, struct blkaddr_tree_node, rbn);
	dec_seg_page_counter(sbi, batn->blkaddr);
	rb_erase(&batn->rbn, &blkaddr_tree);
	return 0;
}

/*
 * The core of opportunistic segment cleaning.
 *
 * We maintain per-segment counters that represent how many blocks
 * each segment has in memory. These in-memory block counts are then 
 * considered when the garbage collector is selecting its victim
 * segment.
 */
void fetch_and_handle_duet_events(struct f2fs_sb_info *sbi)
{
	struct duet_item itm;
	int ret;
	__u16 iret = 1;

	if (!duet_online() || !sbi->duet_task_id)
		return;

	/* Update our flush list */
	flushlist_update(sbi);

	/* Get new events */
	while (1) {
		if (duet_fetch(sbi->duet_task_id, &itm, &iret)) {
			f2fs_duet_debug(KERN_ERR "f2fs: duet-gc: "
					"duet_fetch failed.\n");
			return;
		}

		/* No events? */
		if (!iret)
			break;

		/* GC not interested in meta pages */
		if (itm.ino == F2FS_META_INO(sbi))
			continue;

		if (itm.state & DUET_PAGE_ADDED) {
			ret = blkaddr_lookup_insert(sbi, itm.ino, itm.idx);
			if (ret < 0)
				f2fs_duet_debug(KERN_ERR "duet-gc: "
					"blkaddr_lookup_insert error\n");
		} else if (itm.state & DUET_PAGE_REMOVED) {
			ret = blkaddr_lookup_remove(sbi, itm.ino, itm.idx);
			if (ret < 0)
				f2fs_duet_debug(KERN_ERR "duet-gc: "
					"blkaddr_lookup_remove error\n");
		} else if (itm.state == DUET_PAGE_FLUSHED) {
			ret = blkaddr_lookup_update(sbi, itm.ino, itm.idx);
			if (ret < 0)
				f2fs_duet_debug(KERN_ERR "duet-gc: "
					"blkaddr_lookup_update error\n");
		}
	}
}

int register_with_duet(struct f2fs_sb_info *sbi)
{
	int err;

	if (!duet_online()) {
		printk(KERN_ERR "f2fs: duet-gc: "
			"duet is offline, cannot register.\n");
		sbi->duet_task_id = 0;
		return -ENODEV;
	}

	err = duet_register((char *)sbi->sb,
			DUET_REG_SBLOCK | DUET_PAGE_EXISTS | DUET_PAGE_FLUSHED,
			sbi->blocksize, "f2fs-gc", &sbi->duet_task_id);
	if (err) {
		printk(KERN_ERR "f2fs: duet-gc: "
			"failed to register with the duet framework.\n");
		sbi->duet_task_id = 0;
	} else {
		printk(KERN_INFO "f2fs: duet-gc: "
			"registered with the duet framework successfully.\n");
	}

	return err;
}
#endif /* CONFIG_F2FS_DUET_GC */

static int gc_thread_func(void *data)
{
	struct f2fs_sb_info *sbi = data;
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	wait_queue_head_t *wq = &sbi->gc_thread->gc_wait_queue_head;
	long wait_ms;

	wait_ms = gc_th->min_sleep_time;

	do {
		if (try_to_freeze())
			continue;
		else
			wait_event_interruptible_timeout(*wq,
						kthread_should_stop(),
						msecs_to_jiffies(wait_ms));
		if (kthread_should_stop())
			break;

		if (sbi->sb->s_writers.frozen >= SB_FREEZE_WRITE) {
			wait_ms = increase_sleep_time(gc_th, wait_ms);
			continue;
		}

		/*
		 * [GC triggering condition]
		 * 0. GC is not conducted currently.
		 * 1. There are enough dirty segments.
		 * 2. IO subsystem is idle by checking the # of writeback pages.
		 * 3. IO subsystem is idle by checking the # of requests in
		 *    bdev's request list.
		 *
		 * Note) We have to avoid triggering GCs too much frequently.
		 * Because it is possible that some segments can be
		 * invalidated soon after by user update or deletion.
		 * So, I'd like to wait some time to collect dirty segments.
		 */
		if (!mutex_trylock(&sbi->gc_mutex))
			continue;

		if (!is_idle(sbi)) {
			wait_ms = increase_sleep_time(gc_th, wait_ms);
			mutex_unlock(&sbi->gc_mutex);
			continue;
		}

		if (has_enough_invalid_blocks(sbi))
			wait_ms = decrease_sleep_time(gc_th, wait_ms);
		else
			wait_ms = increase_sleep_time(gc_th, wait_ms);

		stat_inc_bggc_count(sbi);

		/* if return value is not zero, no victim was selected */
		if (f2fs_gc(sbi))
			wait_ms = gc_th->no_gc_sleep_time;

		/* balancing f2fs's metadata periodically */
		f2fs_balance_fs_bg(sbi);

	} while (!kthread_should_stop());
	return 0;
}

int start_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th;
	dev_t dev = sbi->sb->s_bdev->bd_dev;
	int err = 0;

	if (!test_opt(sbi, BG_GC))
		goto out;
	gc_th = kmalloc(sizeof(struct f2fs_gc_kthread), GFP_KERNEL);
	if (!gc_th) {
		err = -ENOMEM;
		goto out;
	}

	gc_th->min_sleep_time = DEF_GC_THREAD_MIN_SLEEP_TIME;
	gc_th->max_sleep_time = DEF_GC_THREAD_MAX_SLEEP_TIME;
	gc_th->no_gc_sleep_time = DEF_GC_THREAD_NOGC_SLEEP_TIME;

	gc_th->gc_idle = 0;

	sbi->gc_thread = gc_th;
	init_waitqueue_head(&sbi->gc_thread->gc_wait_queue_head);
	sbi->gc_thread->f2fs_gc_task = kthread_run(gc_thread_func, sbi,
			"f2fs_gc-%u:%u", MAJOR(dev), MINOR(dev));
	if (IS_ERR(gc_th->f2fs_gc_task)) {
		err = PTR_ERR(gc_th->f2fs_gc_task);
		kfree(gc_th);
		sbi->gc_thread = NULL;
	}

out:
	return err;
}

void stop_gc_thread(struct f2fs_sb_info *sbi)
{
	struct f2fs_gc_kthread *gc_th = sbi->gc_thread;
	if (!gc_th)
		return;
	kthread_stop(gc_th->f2fs_gc_task);
	kfree(gc_th);
	sbi->gc_thread = NULL;
/* XXX: Move this code somewhere else */
#ifdef CONFIG_F2FS_DUET_GC
	blkaddr_tree_destroy();
	flushlist_destroy();

	if (sbi->duet_task_id) {
		duet_deregister(sbi->duet_task_id);
		sbi->duet_task_id = 0;
		printk(KERN_INFO "f2fs: duet-gc: "
			"succesfully deregistered from the duet framework.\n");
	}
#endif /* CONFIG_F2FS_DUET_GC */
}

static int select_gc_type(struct f2fs_gc_kthread *gc_th, int gc_type)
{
	int gc_mode = (gc_type == BG_GC) ? GC_CB : GC_GREEDY;

	if (gc_th && gc_th->gc_idle) {
		if (gc_th->gc_idle == 1)
			gc_mode = GC_CB;
		else if (gc_th->gc_idle == 2)
			gc_mode = GC_GREEDY;
	}
	return gc_mode;
}

static void select_policy(struct f2fs_sb_info *sbi, int gc_type,
			int type, struct victim_sel_policy *p)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);

	if (p->alloc_mode == SSR) {
		p->gc_mode = GC_GREEDY;
		p->dirty_segmap = dirty_i->dirty_segmap[type];
		p->max_search = dirty_i->nr_dirty[type];
		p->ofs_unit = 1;
	} else {
		p->gc_mode = select_gc_type(sbi->gc_thread, gc_type);
		p->dirty_segmap = dirty_i->dirty_segmap[DIRTY];
		p->max_search = dirty_i->nr_dirty[DIRTY];
		p->ofs_unit = sbi->segs_per_sec;
	}

	if (p->max_search > MAX_VICTIM_SEARCH)
		p->max_search = MAX_VICTIM_SEARCH;

	p->offset = sbi->last_victim[p->gc_mode];
}

static unsigned int get_max_cost(struct f2fs_sb_info *sbi,
				struct victim_sel_policy *p)
{
	/* SSR allocates in a segment unit */
	if (p->alloc_mode == SSR)
		return 1 << sbi->log_blocks_per_seg;
	if (p->gc_mode == GC_GREEDY)
		return (1 << sbi->log_blocks_per_seg) * p->ofs_unit;
	else if (p->gc_mode == GC_CB)
		return UINT_MAX;
	else /* No other gc_mode */
		return 0;
}

static unsigned int check_bg_victims(struct f2fs_sb_info *sbi)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	unsigned int hint = 0;
	unsigned int secno;

	/*
	 * If the gc_type is FG_GC, we can select victim segments
	 * selected by background GC before.
	 * Those segments guarantee they have small valid blocks.
	 */
next:
	secno = find_next_bit(dirty_i->victim_secmap, TOTAL_SECS(sbi), hint++);
	if (secno < TOTAL_SECS(sbi)) {
		if (sec_usage_check(sbi, secno))
			goto next;
		clear_bit(secno, dirty_i->victim_secmap);
		return secno * sbi->segs_per_sec;
	}
	return NULL_SEGNO;
}

static unsigned int get_cb_cost(struct f2fs_sb_info *sbi, unsigned int segno)
{
	struct sit_info *sit_i = SIT_I(sbi);
	unsigned int secno = GET_SECNO(sbi, segno);
	unsigned int start = secno * sbi->segs_per_sec;
	unsigned long long mtime = 0;
	unsigned int vblocks;
	unsigned char age = 0;
	unsigned char u;
	unsigned int i;
#ifdef CONFIG_F2FS_DUET_GC
	unsigned int inmem = 0;
	struct seg_entry *se = NULL;
#endif /* CONFIG_F2FS_DUET_GC */

	for (i = 0; i < sbi->segs_per_sec; i++) {
		se = get_seg_entry(sbi, start + i);
		mtime += se->mtime;
#ifdef CONFIG_F2FS_DUET_GC
		inmem += se->page_cached_blocks;
#endif /* CONFIG_F2FS_DUET_GC */
	}

	vblocks = get_valid_blocks(sbi, segno, sbi->segs_per_sec);
#ifdef CONFIG_F2FS_DUET_GC
	//printk(KERN_DEBUG "f2fs-gc: examining segno %u - segs_per_sec %u, "
	//		"valid = %u, inmem = %u\n",
	//	segno, sbi->segs_per_sec, vblocks, inmem);
	vblocks -= min(div_u64(inmem, sbi->segs_per_sec),
			(unsigned long long)vblocks); // >> 0;
#endif

	mtime = div_u64(mtime, sbi->segs_per_sec);
	vblocks = div_u64(vblocks, sbi->segs_per_sec);

	u = (vblocks * 100) >> sbi->log_blocks_per_seg;

	/* Handle if the system time is changed by user */
	if (mtime < sit_i->min_mtime)
		sit_i->min_mtime = mtime;
	if (mtime > sit_i->max_mtime)
		sit_i->max_mtime = mtime;
	if (sit_i->max_mtime != sit_i->min_mtime)
		age = 100 - div64_u64(100 * (mtime - sit_i->min_mtime),
				sit_i->max_mtime - sit_i->min_mtime);

	return UINT_MAX - ((100 * (100 - u) * age) / (100 + u));
}

static inline unsigned int get_gc_cost(struct f2fs_sb_info *sbi,
			unsigned int segno, struct victim_sel_policy *p)
{
	if (p->alloc_mode == SSR)
		return get_seg_entry(sbi, segno)->ckpt_valid_blocks;

	/* alloc_mode == LFS */
	if (p->gc_mode == GC_GREEDY)
		return get_valid_blocks(sbi, segno, sbi->segs_per_sec);
	else
		return get_cb_cost(sbi, segno);
}

/*
 * This function is called from two paths.
 * One is garbage collection and the other is SSR segment selection.
 * When it is called during GC, it just gets a victim segment
 * and it does not remove it from dirty seglist.
 * When it is called from SSR segment selection, it finds a segment
 * which has minimum valid blocks and removes it from dirty seglist.
 */
static int get_victim_by_default(struct f2fs_sb_info *sbi,
		unsigned int *result, int gc_type, int type, char alloc_mode)
{
	struct dirty_seglist_info *dirty_i = DIRTY_I(sbi);
	struct victim_sel_policy p;
	unsigned int secno, max_cost;
	int nsearched = 0;

	p.alloc_mode = alloc_mode;
	select_policy(sbi, gc_type, type, &p);

	p.min_segno = NULL_SEGNO;
	p.min_cost = max_cost = get_max_cost(sbi, &p);

	mutex_lock(&dirty_i->seglist_lock);

	if (p.alloc_mode == LFS && gc_type == FG_GC) {
		p.min_segno = check_bg_victims(sbi);
		if (p.min_segno != NULL_SEGNO)
			goto got_it;
	}

	while (1) {
		unsigned long cost;
		unsigned int segno;

		segno = find_next_bit(p.dirty_segmap,
						TOTAL_SEGS(sbi), p.offset);
		if (segno >= TOTAL_SEGS(sbi)) {
			if (sbi->last_victim[p.gc_mode]) {
				sbi->last_victim[p.gc_mode] = 0;
				p.offset = 0;
				continue;
			}
			break;
		}

		p.offset = segno + p.ofs_unit;
		if (p.ofs_unit > 1)
			p.offset -= segno % p.ofs_unit;

		secno = GET_SECNO(sbi, segno);

		if (sec_usage_check(sbi, secno))
			continue;
		if (gc_type == BG_GC && test_bit(secno, dirty_i->victim_secmap))
			continue;

		cost = get_gc_cost(sbi, segno, &p);

		if (p.min_cost > cost) {
			p.min_segno = segno;
			p.min_cost = cost;
		} else if (unlikely(cost == max_cost)) {
			continue;
		}

		if (nsearched++ >= p.max_search) {
			sbi->last_victim[p.gc_mode] = segno;
			break;
		}
	}
	if (p.min_segno != NULL_SEGNO) {
got_it:
		if (p.alloc_mode == LFS) {
			secno = GET_SECNO(sbi, p.min_segno);
			if (gc_type == FG_GC)
				sbi->cur_victim_sec = secno;
			else
				set_bit(secno, dirty_i->victim_secmap);
		}
		*result = (p.min_segno / p.ofs_unit) * p.ofs_unit;

		trace_f2fs_get_victim(sbi->sb, type, gc_type, &p,
				sbi->cur_victim_sec,
				prefree_segments(sbi), free_segments(sbi));
	}
	mutex_unlock(&dirty_i->seglist_lock);

	return (p.min_segno == NULL_SEGNO) ? 0 : 1;
}

static const struct victim_selection default_v_ops = {
	.get_victim = get_victim_by_default,
};

static struct inode *find_gc_inode(nid_t ino, struct list_head *ilist)
{
	struct inode_entry *ie;

	list_for_each_entry(ie, ilist, list)
		if (ie->inode->i_ino == ino)
			return ie->inode;
	return NULL;
}

static void add_gc_inode(struct inode *inode, struct list_head *ilist)
{
	struct inode_entry *new_ie;

	if (inode == find_gc_inode(inode->i_ino, ilist)) {
		iput(inode);
		return;
	}

	new_ie = f2fs_kmem_cache_alloc(winode_slab, GFP_NOFS);
	new_ie->inode = inode;
	list_add_tail(&new_ie->list, ilist);
}

static void put_gc_inode(struct list_head *ilist)
{
	struct inode_entry *ie, *next_ie;
	list_for_each_entry_safe(ie, next_ie, ilist, list) {
		iput(ie->inode);
		list_del(&ie->list);
		kmem_cache_free(winode_slab, ie);
	}
}

static int check_valid_map(struct f2fs_sb_info *sbi,
				unsigned int segno, int offset)
{
	struct sit_info *sit_i = SIT_I(sbi);
	struct seg_entry *sentry;
	int ret;

	mutex_lock(&sit_i->sentry_lock);
	sentry = get_seg_entry(sbi, segno);
	ret = f2fs_test_bit(offset, sentry->cur_valid_map);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

/*
 * This function compares node address got in summary with that in NAT.
 * On validity, copy that node with cold status, otherwise (invalid node)
 * ignore that.
 */
static void gc_node_segment(struct f2fs_sb_info *sbi,
		struct f2fs_summary *sum, unsigned int segno, int gc_type)
{
	bool initial = true;
	struct f2fs_summary *entry;
	int off;

next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		nid_t nid = le32_to_cpu(entry->nid);
		struct page *node_page;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (initial) {
			ra_node_page(sbi, nid);
			continue;
		}
		node_page = get_node_page(sbi, nid);
		if (IS_ERR(node_page))
			continue;

		/* set page dirty and write it */
		if (gc_type == FG_GC) {
			f2fs_wait_on_page_writeback(node_page, NODE, true);
			set_page_dirty(node_page);
		} else {
			if (!PageWriteback(node_page))
				set_page_dirty(node_page);
		}
		f2fs_put_page(node_page, 1);
		stat_inc_node_blk_count(sbi, 1);
	}

	if (initial) {
		initial = false;
		goto next_step;
	}

	if (gc_type == FG_GC) {
		struct writeback_control wbc = {
			.sync_mode = WB_SYNC_ALL,
			.nr_to_write = LONG_MAX,
			.for_reclaim = 0,
		};
		sync_node_pages(sbi, 0, &wbc);

		/*
		 * In the case of FG_GC, it'd be better to reclaim this victim
		 * completely.
		 */
		if (get_valid_blocks(sbi, segno, 1) != 0)
			goto next_step;
	}
}

/*
 * Calculate start block index indicating the given node offset.
 * Be careful, caller should give this node offset only indicating direct node
 * blocks. If any node offsets, which point the other types of node blocks such
 * as indirect or double indirect node blocks, are given, it must be a caller's
 * bug.
 */
block_t start_bidx_of_node(unsigned int node_ofs, struct f2fs_inode_info *fi)
{
	unsigned int indirect_blks = 2 * NIDS_PER_BLOCK + 4;
	unsigned int bidx;

	if (node_ofs == 0)
		return 0;

	if (node_ofs <= 2) {
		bidx = node_ofs - 1;
	} else if (node_ofs <= indirect_blks) {
		int dec = (node_ofs - 4) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 2 - dec;
	} else {
		int dec = (node_ofs - indirect_blks - 3) / (NIDS_PER_BLOCK + 1);
		bidx = node_ofs - 5 - dec;
	}
	return bidx * ADDRS_PER_BLOCK + ADDRS_PER_INODE(fi);
}

static int check_dnode(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct node_info *dni, block_t blkaddr, unsigned int *nofs)
{
	struct page *node_page;
	nid_t nid;
	unsigned int ofs_in_node;
	block_t source_blkaddr;

	nid = le32_to_cpu(sum->nid);
	ofs_in_node = le16_to_cpu(sum->ofs_in_node);

	node_page = get_node_page(sbi, nid);
	if (IS_ERR(node_page))
		return 0;

	get_node_info(sbi, nid, dni);

	if (sum->version != dni->version) {
		f2fs_put_page(node_page, 1);
		return 0;
	}

	*nofs = ofs_of_node(node_page);
	source_blkaddr = datablock_addr(node_page, ofs_in_node);
	f2fs_put_page(node_page, 1);

	if (source_blkaddr != blkaddr)
		return 0;
	return 1;
}

static void move_data_page(struct inode *inode, struct page *page, int gc_type)
{
	if (gc_type == BG_GC) {
		if (PageWriteback(page))
			goto out;
		set_page_dirty(page);
		set_cold_data(page);
	} else {
		struct f2fs_sb_info *sbi = F2FS_SB(inode->i_sb);

		f2fs_wait_on_page_writeback(page, DATA, true);

		if (clear_page_dirty_for_io(page) &&
			S_ISDIR(inode->i_mode)) {
			dec_page_count(sbi, F2FS_DIRTY_DENTS);
			inode_dec_dirty_dents(inode);
		}
		set_cold_data(page);
		do_write_data_page(page);
		clear_cold_data(page);
	}
out:
	f2fs_put_page(page, 1);
}

/*
 * This function tries to get parent node of victim data block, and identifies
 * data block validity. If the block is valid, copy that with cold status and
 * modify parent node.
 * If the parent node is not valid or the data block address is different,
 * the victim data block is ignored.
 */
static void gc_data_segment(struct f2fs_sb_info *sbi, struct f2fs_summary *sum,
		struct list_head *ilist, unsigned int segno, int gc_type)
{
	struct super_block *sb = sbi->sb;
	struct f2fs_summary *entry;
	block_t start_addr;
	int off;
	int phase = 0;

	start_addr = START_BLOCK(sbi, segno);

next_step:
	entry = sum;

	for (off = 0; off < sbi->blocks_per_seg; off++, entry++) {
		struct page *data_page;
		struct inode *inode;
		struct node_info dni; /* dnode info for the data */
		unsigned int ofs_in_node, nofs;
		block_t start_bidx;

		/* stop BG_GC if there is not enough free sections. */
		if (gc_type == BG_GC && has_not_enough_free_secs(sbi, 0))
			return;

		if (check_valid_map(sbi, segno, off) == 0)
			continue;

		if (phase == 0) {
			ra_node_page(sbi, le32_to_cpu(entry->nid));
			continue;
		}

		/* Get an inode by ino with checking validity */
		if (check_dnode(sbi, entry, &dni, start_addr + off, &nofs) == 0)
			continue;

		if (phase == 1) {
			ra_node_page(sbi, dni.ino);
			continue;
		}

		ofs_in_node = le16_to_cpu(entry->ofs_in_node);

		if (phase == 2) {
			inode = f2fs_iget(sb, dni.ino);
			if (IS_ERR(inode))
				continue;

			start_bidx = start_bidx_of_node(nofs, F2FS_I(inode));

			data_page = find_data_page(inode,
					start_bidx + ofs_in_node, false);
			if (IS_ERR(data_page))
				goto next_iput;

			f2fs_put_page(data_page, 0);
			add_gc_inode(inode, ilist);
		} else {
			inode = find_gc_inode(dni.ino, ilist);
			if (inode) {
				start_bidx = start_bidx_of_node(nofs,
								F2FS_I(inode));
				data_page = get_lock_data_page(inode,
						start_bidx + ofs_in_node);
				if (IS_ERR(data_page))
					continue;
				move_data_page(inode, data_page, gc_type);
				stat_inc_data_blk_count(sbi, 1);
			}
		}
		continue;
next_iput:
		iput(inode);
	}

	if (++phase < 4)
		goto next_step;

	if (gc_type == FG_GC) {
		f2fs_submit_bio(sbi, DATA, true);

		/*
		 * In the case of FG_GC, it'd be better to reclaim this victim
		 * completely.
		 */
		if (get_valid_blocks(sbi, segno, 1) != 0) {
			phase = 2;
			goto next_step;
		}
	}
}

static int __get_victim(struct f2fs_sb_info *sbi, unsigned int *victim,
						int gc_type, int type)
{
	struct sit_info *sit_i = SIT_I(sbi);
	int ret;
	mutex_lock(&sit_i->sentry_lock);
	ret = DIRTY_I(sbi)->v_ops->get_victim(sbi, victim, gc_type, type, LFS);
	mutex_unlock(&sit_i->sentry_lock);
	return ret;
}

static void do_garbage_collect(struct f2fs_sb_info *sbi, unsigned int segno,
				struct list_head *ilist, int gc_type)
{
	struct page *sum_page;
	struct f2fs_summary_block *sum;
	struct blk_plug plug;

	/* read segment summary of victim */
	sum_page = get_sum_page(sbi, segno);
	if (IS_ERR(sum_page))
		return;

	blk_start_plug(&plug);

	sum = page_address(sum_page);

	switch (GET_SUM_TYPE((&sum->footer))) {
	case SUM_TYPE_NODE:
		gc_node_segment(sbi, sum->entries, segno, gc_type);
		break;
	case SUM_TYPE_DATA:
		gc_data_segment(sbi, sum->entries, ilist, segno, gc_type);
		break;
	}
	blk_finish_plug(&plug);

	stat_inc_seg_count(sbi, GET_SUM_TYPE((&sum->footer)));
	stat_inc_call_count(sbi->stat_info);

	f2fs_put_page(sum_page, 1);
}

int f2fs_gc(struct f2fs_sb_info *sbi)
{
	struct list_head ilist;
	unsigned int segno, i;
	int gc_type = BG_GC;
	int nfree = 0;
	int ret = -1;

#ifdef CONFIG_F2FS_DUET_STAT
	ktime_t tstart;
	tstart = ktime_get();
#endif
#ifdef CONFIG_F2FS_DUET_GC
	fetch_and_handle_duet_events(sbi);
#endif
#ifdef CONFIG_F2FS_DUET_STAT
	F2FS_STAT(sbi)->t_duet = ktime_add_safe(F2FS_STAT(sbi)->t_duet,
						ktime_sub(ktime_get(), tstart));
#endif

	INIT_LIST_HEAD(&ilist);
gc_more:
	if (!(sbi->sb->s_flags & MS_ACTIVE))
		goto stop;

	if (gc_type == BG_GC && has_not_enough_free_secs(sbi, nfree)) {
		gc_type = FG_GC;
		write_checkpoint(sbi, false);
	}

	if (!__get_victim(sbi, &segno, gc_type, NO_CHECK_TYPE))
		goto stop;
	ret = 0;

#ifdef CONFIG_F2FS_DUET_STAT
	F2FS_STAT(sbi)->gc_inmem += get_seg_entry(sbi, segno)->page_cached_blocks;
	tstart = ktime_get();
#endif /* CONFIG_F2FS_DUET_STAT */
	for (i = 0; i < sbi->segs_per_sec; i++)
		do_garbage_collect(sbi, segno + i, &ilist, gc_type);
#ifdef CONFIG_F2FS_DUET_STAT
	F2FS_STAT(sbi)->t_gc = ktime_add_safe(F2FS_STAT(sbi)->t_gc,
					ktime_sub(ktime_get(), tstart));
#endif /* CONFIG_F2FS_DUET_STAT */

	if (gc_type == FG_GC) {
		sbi->cur_victim_sec = NULL_SEGNO;
		nfree++;
		WARN_ON(get_valid_blocks(sbi, segno, sbi->segs_per_sec));
	}

	if (has_not_enough_free_secs(sbi, nfree))
		goto gc_more;

	if (gc_type == FG_GC)
		write_checkpoint(sbi, false);
stop:
	mutex_unlock(&sbi->gc_mutex);

	put_gc_inode(&ilist);
	return ret;
}

void build_gc_manager(struct f2fs_sb_info *sbi)
{
#ifdef CONFIG_F2FS_DUET_GC
	int err;

	blkaddr_tree = RB_ROOT;
	INIT_LIST_HEAD(&flushlist);

	err = register_with_duet(sbi);
	if (err)
		blkaddr_tree_destroy();
#endif /* CONFIG_F2FS_DUET_GC */
	DIRTY_I(sbi)->v_ops = &default_v_ops;
}

int __init create_gc_caches(void)
{
	winode_slab = f2fs_kmem_cache_create("f2fs_gc_inodes",
			sizeof(struct inode_entry), NULL);
	if (!winode_slab)
		return -ENOMEM;
	return 0;
}

void destroy_gc_caches(void)
{
	kmem_cache_destroy(winode_slab);
}
