/*
 * Copyright (C) 2007 Oracle.  All rights reserved.
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

#ifndef __UTILS__
#define __UTILS__

#include <dirent.h>
#if 0
#include <sys/stat.h>

int pretty_size_snprintf(u64 size, char *str, size_t str_bytes);
#define pretty_size(size) 						\
	({								\
		static __thread char _str[24];				\
		(void)pretty_size_snprintf((size), _str, sizeof(_str));	\
		_str;							\
	})

int get_mountpt(char *dev, char *mntpt, size_t size);
u64 parse_size(char *s);
#endif
int open_dev(const char *fname);
void close_dev(int fd);
#if 0
int get_fs_info(char *path, struct btrfs_ioctl_fs_info_args *fi_args,
		struct btrfs_ioctl_dev_info_args **di_ret);

char *__strncpy__null(char *dest, const char *src, size_t n);
int is_block_device(const char *file);
int is_mount_point(const char *file);
int open_path_or_dev_mnt(const char *path, DIR **dirstream);
/* Helper to always get proper size of the destination string */
#define strncpy_null(dest, src) __strncpy__null(dest, src, sizeof(dest))
int csum_tree_block(struct btrfs_root *root, struct extent_buffer *buf,
			   int verify);
int ask_user(char *question);
#endif
#endif
