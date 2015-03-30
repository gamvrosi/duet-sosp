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
#include "ctree.h"
#include "btrfs_inode.h"
#include "mapping.h"

#ifdef CONFIG_BTRFS_FS_MAPPING_DEBUG
#define map_dbg(...)	printk(__VA_ARGS__)
#else
#define map_dbg(...)
#endif

/*
 * Gets an inode from the fs tree, using its inode number.
 * Returns 1 on failure, 0 if inode was found.
 */
int btrfs_iget_ino(struct btrfs_fs_info *fs_info, unsigned long ino,
	struct inode **inode, int *ondisk)
{
	struct btrfs_key key;

	key.objectid = ino;
	key.type = BTRFS_INODE_ITEM_KEY;
	key.offset = 0;

	*inode = btrfs_iget(fs_info->sb, &key, fs_info->fs_root, ondisk);
	if (IS_ERR(*inode)) {
		map_dbg(KERN_ERR "btrfs_iget_ino: no inode %lu\n", ino);
		return 1;
	}

	map_dbg(KERN_INFO "btrfs_iget_ino: got inode %lu\n", ino);

	return 0;
}

int btrfs_get_logical(struct inode *inode, unsigned long index,
	struct extent_map **em, int *ondisk)
{
	u64 start, len;
	struct extent_map_tree *em_tree;
	struct extent_io_tree *io_tree;

	if (ondisk)
		*ondisk = 0;
	start = index << PAGE_CACHE_SHIFT;
	len = PAGE_CACHE_SIZE;

	em_tree = &BTRFS_I(inode)->extent_tree;
	io_tree = &BTRFS_I(inode)->io_tree;

	/*
	 * hopefully we have this extent in the tree already, try without
	 * the full extent lock
	 */
	map_dbg(KERN_INFO "btrfs_get_logical: file offt %llu, len %llu\n",
		start, len);
	read_lock(&em_tree->lock);
	*em = lookup_extent_mapping(em_tree, start, len);
	read_unlock(&em_tree->lock);

	if (!(*em)) {
		if (ondisk)
			*ondisk = 1;

		/* get the big lock and read metadata off disk */
		lock_extent(io_tree, start, start + len - 1);
		*em = btrfs_get_extent(inode, NULL, 0, start, len, 0);
		unlock_extent(io_tree, start, start + len - 1);

		if (IS_ERR(*em))
			return 1;
	}

	map_dbg(KERN_INFO "btrfs_get_logical: struct extent_map contents:\n"
		"\tstart = %llu, len = %llu\n"
		"\tmod_start = %llu, mod_len = %llu\n"
		"\torig_start = %llu, orig_block_len = %llu, ram_bytes = %llu\n"
		"\tblock_start = %llu, block_len = %llu, generation = %llu\n",
		(*em)->start, (*em)->len, (*em)->mod_start, (*em)->mod_len,
		(*em)->orig_start, (*em)->orig_block_len, (*em)->ram_bytes,
		(*em)->block_start, (*em)->block_len, (*em)->generation);

	/* We won't bother with holes, inline extents, delallocs etc. */
	if ((*em)->block_start >= EXTENT_MAP_LAST_BYTE)
		return 1;

	return 0;
}
