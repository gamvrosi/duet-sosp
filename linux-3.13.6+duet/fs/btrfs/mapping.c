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
#include "ctree.h"
#include "volumes.h"
#include "mapping.h"
#include "raid56.h"

/* Moves any address to the next region multiple and increments it by offset */
static inline u64 __next_region_offt(u64 addr, u64 region, u64 offt) {
	return ((addr / region) * region) + offt;
}

/*
 * Finds a device using its bdev pointer. If that's NULL, the devid field
 * is used instead
 */
static struct btrfs_device *__find_device(struct btrfs_fs_info *fs_info,
					struct block_device *bdev, u64 devid)
{
	struct btrfs_device *device;
	struct btrfs_fs_devices *cur_devices;

	cur_devices = fs_info->fs_devices;
	while (cur_devices) {
		list_for_each_entry(device, &cur_devices->devices, dev_list) {
			if ((bdev && device->bdev == bdev) ||
			    (!bdev && device->devid == devid))
				return device;
		}
		cur_devices = cur_devices->seed;
	}

	return NULL;
}

/*
 * Looks for a chunk that contains the given physical address.
 * de_ctx must have already been allocated before calling this.
 */
static int find_chunk_by_paddr(struct btrfs_device *device, u64 p_offt,
				struct btrfs_dev_extent_ctx *de_ctx)
{
	struct btrfs_dev_extent *de;
	struct btrfs_key key;
	struct btrfs_root *root = device->dev_root;
	struct btrfs_path *path;
	struct extent_buffer *l;
	int ret, slot;

	if (!de_ctx)
		return -EIO;

	if (p_offt >= device->total_bytes || device->is_tgtdev_for_dev_replace)
		return -ENXIO;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Work on commit root. The related disk blocks are static as long as
	 * CoW is applied.
	 */
	path->search_commit_root = 1;
	path->skip_locking = 1;
	path->reada = 2;

	key.objectid = device->devid;
	key.offset = p_offt;
	key.type = BTRFS_DEV_EXTENT_KEY;

	ret = btrfs_search_slot(NULL, root, &key, path, 0, 0);
	if (ret < 0)
		goto out;
	if (ret > 0) {
		ret = btrfs_previous_item(root, path, key.objectid, key.type);
		if (ret < 0)
			goto out;
	}

	while (1) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
			ret = btrfs_next_leaf(root, path);
			if (ret == 0)
				continue;
			else
				goto out;
		}
		btrfs_item_key_to_cpu(l, &key, slot);

		if (key.objectid < device->devid)
			goto next;

		if (key.objectid > device->devid)
			goto out;

		if (btrfs_key_type(&key) != BTRFS_DEV_EXTENT_KEY)
			goto next;

		/* We found a dev extent item on the right device. Make sure
		 * it contains the given LBN */
		de = btrfs_item_ptr(l, slot, struct btrfs_dev_extent);
		de_ctx->p_start = key.offset;
		de_ctx->l_start = btrfs_dev_extent_chunk_offset(l, de);
		de_ctx->len = btrfs_dev_extent_length(l, de);

		/* Check if we found it */
		if (de_ctx->p_start <= p_offt &&
		    p_offt < (de_ctx->p_start + de_ctx->len))
			break;

next:
		path->slots[0]++;
	}

out:
	btrfs_free_path(path);
	return ret < 0 ? ret : 0;
}

/*
 * Maps a physical address range to a logical address range for a given chunk,
 * and returns information on the logical address layout of this chunk (this
 * is affected by the type of data redundancy used).
 * Returns 0 on success, or the appropriate error.
 */
static int find_chunk_addr_mapping(struct btrfs_device *device,
				   struct btrfs_dev_extent_ctx de_ctx,
				   u64 p_offt, u64 len,
				   struct btrfs_addr_mapping_ctx *am_ctx)
{
	struct btrfs_mapping_tree *map_tree =
		&device->dev_root->fs_info->mapping_tree;
	struct map_lookup *map;
	struct extent_map *em;
	int stripe_num, i, ret = 0;
	u64 offset, l_stripe, p_stripe;
	u64 diff_factor, diff_offt;

	read_lock(&map_tree->map_tree.lock);
	em = lookup_extent_mapping(&map_tree->map_tree, de_ctx.l_start, 1);
	read_unlock(&map_tree->map_tree.lock);

	if (!em)
		return -EINVAL;

	map = (struct map_lookup *)em->bdev;
	if (em->start != de_ctx.l_start || em->len < de_ctx.len)
		goto out;

	/* Find the first stripe of the chunk */
	stripe_num = -1;
	for (i = 0; i < map->num_stripes; ++i) {
		if (map->stripes[i].dev->bdev == device->bdev &&
		    map->stripes[i].physical == de_ctx.p_start) {
			stripe_num = i;
			break;
		}
	}

	if (stripe_num == -1) {
		ret = -ENXIO;
		goto out;
	}

	/*
	 * We need to step through the stripes to find the stripe corresponding
	 * to the given LBN. To do that, fix up the increment for the current
	 * RAID scheme
	 */
	if (map->type & (BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) {
		if (stripe_num >= nr_data_stripes(map)) {
			ret = -ENXIO;
			goto out;
		}
	}

	am_ctx->stripe_len = map->stripe_len;
	am_ctx->increment = map->stripe_len;
	am_ctx->nstripes = de_ctx.len;
	offset = 0;
	do_div(am_ctx->nstripes, map->stripe_len);
	if (map->type & BTRFS_BLOCK_GROUP_RAID0) {
		offset = map->stripe_len * stripe_num;
		am_ctx->increment = map->stripe_len * map->num_stripes;
	} else if (map->type & BTRFS_BLOCK_GROUP_RAID10) {
		int factor = map->num_stripes / map->sub_stripes;
		offset = map->stripe_len * (stripe_num / map->sub_stripes);
		am_ctx->increment = map->stripe_len * factor;
	} else { /* RAID1/5/6, DUP, or SINGLE */
		am_ctx->increment = map->stripe_len;
	}

	/* We need to offset the l_start for RAID0-based schemes.
	 * We also calculate the physical, logical beginning of the stripe */
	am_ctx->l_start = de_ctx.l_start + offset;
	am_ctx->p_start = map->stripes[stripe_num].physical;
	am_ctx->l_end = am_ctx->l_start + am_ctx->increment * am_ctx->nstripes;

	/*
	 * Find how many stripes we need to skip to get there
	 * - logical moves in increment bytes
	 * - physical moves in map->stripe_len bytes
	 */
	diff_factor = (p_offt - am_ctx->p_start) / map->stripe_len;
	p_stripe = am_ctx->p_start + (diff_factor * map->stripe_len);
	l_stripe = am_ctx->l_start + (am_ctx->increment * diff_factor);

	/* Now find the offset within the stripe, and overwrite l_start */
	diff_offt = p_offt - p_stripe;
	am_ctx->l_offt = l_stripe + diff_offt;
	am_ctx->p_offt = p_offt;

	if (am_ctx->l_offt >= am_ctx->l_end) {
		ret = -EFAULT;
		goto out;
	}

	/* Estimate the number of bytes in the current chunk */
	if (len > (de_ctx.p_start + de_ctx.len - p_offt))
		am_ctx->num_bytes = de_ctx.p_start + de_ctx.len - p_offt;
	else
		am_ctx->num_bytes = len;

	am_ctx->l_offt_end = am_ctx->l_offt + (am_ctx->num_bytes /
		am_ctx->stripe_len) * am_ctx->increment + (am_ctx->num_bytes %
		am_ctx->stripe_len);

out:
	free_extent_map(em);
	return ret;
}

static int find_extent_in_tree(struct btrfs_root *root,
			       struct btrfs_key *key,
			       struct btrfs_file_extent_item *extent)
{
	struct btrfs_path *path;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Work on commit root. The related disk blocks are static as long as
	 * CoW is applied.
	 */
	path->search_commit_root = 1;
	path->skip_locking = 1;

	ret = btrfs_search_slot(NULL, root, key, path, 0, 0);
	if (ret != 0) {
		ret = -ENOENT;
		goto out;
	}

	extent = btrfs_item_ptr(path->nodes[0], path->slots[0],
				struct btrfs_file_extent_item);

out:
	btrfs_free_path(path);
	return ret;
}

static int iterate_range_items(struct btrfs_fs_info *fs_info,
			       struct btrfs_addr_mapping_ctx *am_ctx,
			       struct btrfs_root *root,
			       iterate_ranges_t iterate)
{
	struct btrfs_root *extroot = fs_info->extent_root;
	struct btrfs_path *path;
	struct btrfs_key key, ekey;
	struct extent_buffer *l;
	struct btrfs_extent_item *ei;
	struct btrfs_extent_inline_ref *iref;
	struct btrfs_extent_data_ref *dref;
	struct btrfs_file_extent_item *extent;
	unsigned long ptr, end;
	int ret=0, slot, stop_loop, found;
	u64 flags;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/*
	 * Work on commit root. The related disk blocks are static as
	 * long as CoW is applied.
	 */
	path->search_commit_root = 1;
	path->skip_locking = 1;

	while (am_ctx->l_offt < am_ctx->l_offt_end) {
		btrfs_release_path(path);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "iterate_range_item: l_offt = %llu\n",
							am_ctx->l_offt);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		key.objectid = am_ctx->l_offt;
		key.type = BTRFS_EXTENT_ITEM_KEY;
		key.offset = (u64)-1;

		ret = btrfs_search_slot(NULL, extroot, &key, path, 0, 0);
		if (ret < 0)
			goto out;

		if (ret > 0) {
			/* Couldn't find item, grab immediately smaller one */
			ret = btrfs_previous_item(extroot, path, 0,
							BTRFS_EXTENT_ITEM_KEY);
			if (ret < 0 || ret > 0)
				goto out;
		}

		/* Starting with the returned item, traverse the leaves until
		 * we reach the end of the current stripe */
		stop_loop = 0;
		while (1) {
			u64 bytes, extent_start, extent_len;

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item traversing leaves"
				" -- l_offt = %llu\n", am_ctx->l_offt);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			l = path->nodes[0];
			slot = path->slots[0];
			if (slot >= btrfs_header_nritems(l)) {
#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
				printk(KERN_DEBUG "iterate_range_item: slot %d "
					">= items in leaf %d\n", slot,
					btrfs_header_nritems(l));
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */
				ret = btrfs_next_leaf(extroot, path);
				if (ret == 0)
					continue;
				if (ret < 0)
					goto out;

				stop_loop = 1;
				break;
			}
			btrfs_item_key_to_cpu(l, &key, slot);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item processing key "
				"(%llu %u %llu)\n", key.objectid, key.type,
				key.offset);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			if (key.type == BTRFS_METADATA_ITEM_KEY)
				bytes = extroot->leafsize;
			else
				bytes = key.offset;

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item: key type is %u\n",
				key.type);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			/* Check if we're too far behind, or past it */
			if (key.objectid + bytes <= am_ctx->l_offt) {
				goto next;
			} else if (key.objectid >=
				__next_region_offt(am_ctx->l_offt,
				am_ctx->increment, am_ctx->stripe_len)) {
				/* out of this device extent */
				if (key.objectid >= am_ctx->l_offt_end)
					stop_loop = 1;
				break;
			}

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item: we're not too "
				"far behind or forward\n");
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			/* Check if we got the right type */
			if (key.type != BTRFS_EXTENT_ITEM_KEY &&
			    key.type != BTRFS_METADATA_ITEM_KEY)
				goto next;

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item: this is a %s "
				"key\n", key.type == BTRFS_EXTENT_ITEM_KEY ?
				"data" : "metadata");
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			ei = btrfs_item_ptr(l, slot, struct btrfs_extent_item);
			flags = btrfs_extent_flags(l, ei);

			if (!(flags & BTRFS_EXTENT_FLAG_DATA))
				goto next;

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item: DATA flag is set\n");
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			iref = (struct btrfs_extent_inline_ref *)(ei + 1);
			ptr = (unsigned long)iref;
			end = (unsigned long)ei + btrfs_item_size_nr(l, slot);
			found = 0;
			while (ptr < end) {
				int type;
				u64 offset;

				iref = (struct btrfs_extent_inline_ref *)ptr;
				type = btrfs_extent_inline_ref_type(l, iref);
				offset = btrfs_extent_inline_ref_offset(l, iref);

				if (type == BTRFS_EXTENT_DATA_REF_KEY) {
					dref = (struct btrfs_extent_data_ref *)
						(&iref->offset);

					found = 1;
					ekey.objectid = btrfs_extent_data_ref_objectid(l, dref);
					ekey.type = BTRFS_EXTENT_DATA_KEY;
					ekey.offset = btrfs_extent_data_ref_offset(l, dref);
					break;
				}

				ptr += btrfs_extent_inline_ref_size(type);
			}

			if (!found)
				goto next;

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "iterate_range_item looking for extent"
				"(%llu %u %llu)\n", ekey.objectid, ekey.type,
				ekey.offset);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			/* Try to find the data extent in given tree */
			ret = find_extent_in_tree(root, &ekey, extent);
			if (ret)
				goto out;

again:
			extent_start = am_ctx->l_offt;
			/* Find the shortest length: to the stripe's end, to
			 * the extent's end, or to the logical range's end */
			extent_len = min3(
				__next_region_offt(am_ctx->l_offt,
					am_ctx->increment,
					am_ctx->stripe_len) - am_ctx->l_offt,
				bytes - (am_ctx->l_offt - key.objectid),
				am_ctx->l_offt_end - am_ctx->l_offt);

			if (iterate)
				iterate(extent_start, extent_len, (void *)&ekey);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_INFO "Synergy found w/ extent (%llu %u %llu), "
				"for range [%llu, %llu]\n",
				ekey.objectid, ekey.type, ekey.offset,
				extent_start, extent_start + extent_len);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

			if (extent_start + extent_len < key.objectid + bytes) {
				am_ctx->l_offt = __next_region_offt(
					am_ctx->l_offt, am_ctx->increment,
					am_ctx->increment);
				am_ctx->p_offt = __next_region_offt(
					am_ctx->p_offt, am_ctx->stripe_len,
					am_ctx->stripe_len);

				if (am_ctx->l_offt >= am_ctx->l_offt_end) {
					stop_loop = 1;
					break;
				}

				if (am_ctx->l_offt < key.objectid + bytes) {
					cond_resched();
					goto again;
				}
			}
next:
			path->slots[0]++;
		}
		btrfs_release_path(path);

		am_ctx->l_offt = __next_region_offt(am_ctx->l_offt,
				am_ctx->increment, am_ctx->increment);
		am_ctx->p_offt = __next_region_offt(am_ctx->p_offt,
				am_ctx->stripe_len, am_ctx->stripe_len);

		if (stop_loop)
			break;
	}

out:
	btrfs_free_path(path);
	return ret;
}

/*
 * Given a block device, a physical address range, and a root, find all the
 * metadata items that correspond to the range, and call the provided
 * callback for each
 */
int btrfs_physical_to_items(struct btrfs_fs_info *fs_info,
			    struct block_device *bdev, u64 p_offt, u64 len,
			    struct btrfs_root *root, iterate_ranges_t iterate)
{
	struct btrfs_device *dev;
	u64 cur_offt = p_offt;
	u64 rem_len = len;
	struct btrfs_dev_extent_ctx de_ctx;
	struct btrfs_addr_mapping_ctx am_ctx;
	int ret = 0;

	memset(&de_ctx, 0, sizeof(de_ctx));
	memset(&am_ctx, 0, sizeof(am_ctx));

	dev = __find_device(fs_info, bdev, 0);
	if (!dev) {
		printk(KERN_ERR "btrfs_physical_to_items: device not found\n");
		ret = -ENODEV;
		goto out;
	}

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
	printk(KERN_DEBUG "btrfs_physical_to_items: device id %llu, size %llu.\n",
		dev->devid, dev->total_bytes);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

	/* Iterate through every chunk in the given range */
	while (rem_len) {
		ret = find_chunk_by_paddr(dev, cur_offt, &de_ctx);
		if (ret) {
			printk(KERN_ERR "btrfs_physical_to_items: chunk not found\n");
			goto out;
		}

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "btrfs_physical_to_items: chunk p_start %llu, "
			"l_start %llu, len %llu\n",
			de_ctx.p_start, de_ctx.l_start, de_ctx.len);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		ret = find_chunk_addr_mapping(dev, de_ctx, cur_offt, rem_len,
								&am_ctx);
		if (ret) {
			printk(KERN_ERR "btrfs_physical_to_items: address mapping "
				"info collection failed\n");
			goto out;
		}

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "btrfs_physical_to_items: address mapping found\n"
				"btrfs_physical_to_items: \tl_start %llu\n"
				"btrfs_physical_to_items: \tl_offt %llu\n"
				"btrfs_physical_to_items: \tl_offt_end %llu\n"
				"btrfs_physical_to_items: \tl_end %llu\n"
				"btrfs_physical_to_items: \tp_start %llu\n"
				"btrfs_physical_to_items: \tp_offt %llu\n"
				"btrfs_physical_to_items: \tstripe_len %llu\n"
				"btrfs_physical_to_items: \tincrement %llu\n"
				"btrfs_physical_to_items: \tnstripes %llu\n"
				"btrfs_physical_to_items: \tnum_bytes %llu\n",
			am_ctx.l_start, am_ctx.l_offt, am_ctx.l_offt_end,
			am_ctx.l_end, am_ctx.p_start, am_ctx.p_offt,
			am_ctx.stripe_len, am_ctx.increment, am_ctx.nstripes,
			am_ctx.num_bytes);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		ret = iterate_range_items(fs_info, &am_ctx, root, iterate);
		if (ret) {
			printk(KERN_DEBUG "btrfs_physical_to_items: item iteration failed\n");
			goto out;
		}

		rem_len -= am_ctx.num_bytes;
	}

out:
	return ret;
}

/*
 * Translates a chunk's virtual range in one or more physical ranges. Note that
 * an extent may not span chunks, but a chunk may span physical devices.
 */
static int __iter_chunk_ranges(struct btrfs_fs_info *fs_info, u64 chunk_offt,
			struct btrfs_chunk *chunk, struct extent_buffer *l,
			u64 vofft, u64 vlen, iterate_ranges_t iterate)
{
	struct btrfs_stripe *stripe;
	struct btrfs_device *device;
	int i, ret=0;
	u64 pofft;
	int num_stripes = btrfs_chunk_num_stripes(l, chunk);

	if (btrfs_chunk_type(l, chunk) & (BTRFS_BLOCK_GROUP_RAID0 |
	    BTRFS_BLOCK_GROUP_RAID1 | BTRFS_BLOCK_GROUP_RAID10 |
	    BTRFS_BLOCK_GROUP_RAID5 | BTRFS_BLOCK_GROUP_RAID6)) {
		/* TODO: Support RAID schemes */
		return -EINVAL;
	} else { /* DUP, or SINGLE */
		/* Process every stripe */
		for (i = 0; i < num_stripes; i++) {
			stripe = btrfs_stripe_nr(chunk, i);

			/* Find the device for this chunk */
			device = __find_device(fs_info, NULL,
					btrfs_stripe_devid_nr(l, chunk, i));

			/* To get the physical offset, just count from the
			 * beginning of the stripe. No need to change the
			 * length, we're still staying in the chunk */
			pofft = btrfs_stripe_offset_nr(l, chunk, i) +
				(vofft - chunk_offt);

			ret = iterate(pofft, vlen, (void *)device->bdev);
			if (ret)
				goto out;
		}
	}

out:
	return ret;
}

/*
 * Finds all the physical ranges in the virtual range [@vofft, @vofft+@vlen].
 * The virtual range is assumed to be contained within one extent (and as
 * a result, the same chunk.
 */
static int __iter_physical_ranges(struct btrfs_fs_info *fs_info, u64 vofft,
					u64 vlen, iterate_ranges_t iterate)
{
	struct btrfs_root *fs_root = fs_info->chunk_root;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct btrfs_chunk *chunk;
	struct extent_buffer *l;
	u64 chunk_len, chunk_offt;
	int ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	path->search_commit_root = 1;
	path->skip_locking = 1;

	/* Find the chunk that the logical range belongs in */
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = vofft;

	ret = btrfs_search_slot_for_read(fs_root, &key, path, 0, 0);
	if (ret != 0)
		goto not_found;

	l = path->nodes[0];
	btrfs_item_key_to_cpu(l, &key, path->slots[0]);

	if (key.objectid != BTRFS_FIRST_CHUNK_TREE_OBJECTID ||
	    key.type != BTRFS_CHUNK_ITEM_KEY)
		goto not_found;

	chunk = btrfs_item_ptr(l, path->slots[0], struct btrfs_chunk);
	chunk_offt = key.offset;
	chunk_len = btrfs_chunk_length(l, chunk);

	if (chunk_offt + chunk_len <= vofft || chunk_offt > vofft)
		goto not_found;

	ret = __iter_chunk_ranges(fs_info, chunk_offt, chunk, l, vofft, vlen,
								iterate);

	btrfs_free_path(path);
	return ret;

not_found:
	btrfs_free_path(path);
	return -ENOENT;
}

/*
 * A bit scarier than btrfs_get_extent, this finds the mapping of a virtual
 * range to a physical one. Since the resulting physical ranges might be
 * discontiguous, we discover each separately, and call the given callback
 * function.
 */
int btrfs_ino_to_physical(struct btrfs_fs_info *fs_info, u64 ino, u64 iofft,
			u64 ilen, iterate_ranges_t iterate)
{
	struct btrfs_root *fs_root = fs_info->fs_root;
	struct btrfs_key key;
	struct btrfs_path *path;
	struct extent_buffer *l;
	struct btrfs_file_extent_item *fi;
	u64 vofft, vlen;
	u64 cur_iofft, cur_ilen;
	int slot, ret = 0;

	path = btrfs_alloc_path();
	if (!path)
		return -ENOMEM;

	/* Do some readahead, in case we need to go over multiple extents */
	path->reada = 2;
	path->search_commit_root = 1;
	path->skip_locking = 1;

	/* Find the first extent data item corresponding to the given i-offset */
	key.objectid = ino;
	key.type = BTRFS_EXTENT_DATA_KEY;
	key.offset = iofft;

	ret = btrfs_search_slot_for_read(fs_root, &key, path, 0, 0);
	if (ret != 0) {
		ret = -ENOENT;
		goto out;
	}

	/* Iterate over all extents corresponding to the given v-range */
	cur_iofft = iofft;
	cur_ilen = ilen;

	while (cur_ilen) {
		l = path->nodes[0];
		slot = path->slots[0];
		if (slot >= btrfs_header_nritems(l)) {
#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
			printk(KERN_DEBUG "btrfs_file_to_physical: EOLeaf\n");
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */
			ret = btrfs_next_leaf(fs_root, path);
			if (ret == 0) {
				continue;
			} else {
				ret = -ENOENT;
				goto out;
			}
		}
		btrfs_item_key_to_cpu(l, &key, slot);

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
		printk(KERN_DEBUG "btrfs_file_to_physical processing (%llu, "
			"%u, %llu)\n", key.objectid, key.type, key.offset);
#endif /* CONFIG_BTRFS_FS_MAPPING_DEBUG */

		if (key.objectid != ino || key.type != BTRFS_EXTENT_DATA_KEY) {
			ret = -ENOENT;
			goto out;
		}

		/* TODO: Account for compression */
		fi = btrfs_item_ptr(l, slot, struct btrfs_file_extent_item);
		if (btrfs_file_extent_type(l, fi) == BTRFS_FILE_EXTENT_INLINE) {
			/* TODO: Support inline extents */
			//l_len = btrfs_file_extent_inline_len(l, fi);
			ret = 0;
			goto out;
		}

		/* Check if we found the extent we're looking for */
		if (key.offset + (btrfs_file_extent_num_bytes(l, fi) -
		    btrfs_file_extent_disk_bytenr(l,fi)) <= cur_iofft)
			goto next;

		/* Find the v-length starting from the v-offset */
		vofft = btrfs_file_extent_disk_bytenr(l, fi) +
			btrfs_file_extent_offset(l, fi) +
			(cur_iofft - key.offset);
		vlen = btrfs_file_extent_num_bytes(l, fi) -
			(vofft - btrfs_file_extent_disk_bytenr(l, fi));

		if (cur_ilen >= vlen) {
			cur_ilen -= vlen;
		} else {
			vlen = cur_ilen;
			cur_ilen = 0;
		}

		/* Process this v-range */
		if (__iter_physical_ranges(fs_info, vofft, vlen, iterate)) {
			ret = -ENOENT;
			goto out;
		}

		/* Move on to the next extent */
		cur_iofft += vlen;

next:
		path->slots[0]++;
		cond_resched();
	}

out:
	btrfs_free_path(path);
	return ret;
}
