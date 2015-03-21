/*
 * Routines only used by the sending process.
 *
 * Copyright (C) 1996 Andrew Tridgell
 * Copyright (C) 1996 Paul Mackerras
 * Copyright (C) 2003-2014 Wayne Davison
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, visit the http://fsf.org website.
 */

#include "rsync.h"
#include "inums.h"
#ifdef HAVE_DUET
#include "duet/duet.h"
#endif /* HAVE_DUET */

extern int do_xfers;
extern int am_server;
extern int am_daemon;
extern int inc_recurse;
extern int log_before_transfer;
extern int stdout_format_has_i;
extern int logfile_format_has_i;
extern int want_xattr_optim;
extern int csum_length;
extern int append_mode;
extern int io_error;
extern int flist_eof;
extern int allowed_lull;
extern int preserve_xattrs;
extern int protocol_version;
extern int remove_source_files;
extern int updating_basis_file;
extern int make_backups;
extern int inplace;
extern int batch_fd;
extern int write_batch;
extern int file_old_total;
extern struct stats stats;
extern struct file_list *cur_flist, *first_flist, *dir_flist;
#ifdef HAVE_DUET
extern struct file_list *cur_o3_flist, *first_o3_flist;
extern int out_of_order, current_files, pending_o3_files, duet_fd;
extern int file_total, file_old_total;
extern __u8 tid;
extern struct inode_tree itree;
#endif /* HAVE_DUET */
BOOL extra_flist_sending_enabled;

/**
 * @file
 *
 * The sender gets checksums from the generator, calculates deltas,
 * and transmits them to the receiver.  The sender process runs on the
 * machine holding the source files.
 **/

/**
 * Receive the checksums for a buffer
 **/
static struct sum_struct *receive_sums(int f)
{
	struct sum_struct *s;
	int32 i;
	int lull_mod = protocol_version >= 31 ? 0 : allowed_lull * 5;
	OFF_T offset = 0;

	if (!(s = new(struct sum_struct)))
		out_of_memory("receive_sums");

	read_sum_head(f, s);

	s->sums = NULL;

	if (DEBUG_GTE(DELTASUM, 3)) {
		rprintf(FINFO, "count=%s n=%ld rem=%ld\n",
			big_num(s->count), (long)s->blength, (long)s->remainder);
	}

	if (append_mode > 0) {
		s->flength = (OFF_T)s->count * s->blength;
		if (s->remainder)
			s->flength -= s->blength - s->remainder;
		return s;
	}

	if (s->count == 0)
		return(s);

	if (!(s->sums = new_array(struct sum_buf, s->count)))
		out_of_memory("receive_sums");

	for (i = 0; i < s->count; i++) {
		s->sums[i].sum1 = read_int(f);
		read_buf(f, s->sums[i].sum2, s->s2length);

		s->sums[i].offset = offset;
		s->sums[i].flags = 0;

		if (i == s->count-1 && s->remainder != 0)
			s->sums[i].len = s->remainder;
		else
			s->sums[i].len = s->blength;
		offset += s->sums[i].len;

		if (lull_mod && !(i % lull_mod))
			maybe_send_keepalive(time(NULL), True);

		if (DEBUG_GTE(DELTASUM, 3)) {
			rprintf(FINFO,
				"chunk[%d] len=%d offset=%s sum1=%08x\n",
				i, s->sums[i].len, big_num(s->sums[i].offset),
				s->sums[i].sum1);
		}
	}

	s->flength = offset;

	return s;
}

void successful_send(int ndx)
{
	char fname[MAXPATHLEN];
	char *failed_op;
	struct file_struct *file;
	struct file_list *flist;
	STRUCT_STAT st;

	if (!remove_source_files)
		return;

	flist = flist_for_ndx(ndx, "successful_send", 1);
	file = flist->files[ndx - flist->ndx_start];
	if (!change_pathname(file, NULL, 0))
		return;
	f_name(file, fname);

	if (do_lstat(fname, &st) < 0) {
		failed_op = "re-lstat";
		goto failed;
	}

	if (st.st_size != F_LENGTH(file) || st.st_mtime != file->modtime
#ifdef ST_MTIME_NSEC
	 || (NSEC_BUMP(file) && (uint32)st.ST_MTIME_NSEC != F_MOD_NSEC(file))
#endif
	) {
		rprintf(FERROR_XFER, "ERROR: Skipping sender remove for changed file: %s\n", fname);
		return;
	}

	if (do_unlink(fname) < 0) {
		failed_op = "remove";
	  failed:
		if (errno == ENOENT)
			rprintf(FINFO, "sender file already removed: %s\n", fname);
		else
			rsyserr(FERROR_XFER, errno, "sender failed to %s %s", failed_op, fname);
	} else {
		if (INFO_GTE(REMOVE, 1))
			rprintf(FINFO, "sender removed %s\n", fname);
	}
}

static void write_ndx_and_attrs(int f_out, int ndx, int iflags,
				const char *fname, struct file_struct *file,
				uchar fnamecmp_type, char *buf, int len)
{
#ifdef HAVE_DUET
	if (DEBUG_GTE(SEND, 4))
		rprintf(FINFO, "write_ndx_and_attrs: writing ndx %d\n", ndx);

	if (file->flags & FLAG_O3)
		write_ndx(f_out, NDX_IS_O3);
#endif /* HAVE_DUET */

	write_ndx(f_out, ndx);
	if (protocol_version < 29)
		return;
#if HAVE_DUET
	write_int(f_out, iflags);
#else
	write_shortint(f_out, iflags);
#endif /* HAVE_DUET */
	if (iflags & ITEM_BASIS_TYPE_FOLLOWS)
		write_byte(f_out, fnamecmp_type);
	if (iflags & ITEM_XNAME_FOLLOWS)
		write_vstring(f_out, buf, len);
#ifdef SUPPORT_XATTRS
	if (preserve_xattrs && iflags & ITEM_REPORT_XATTR && do_xfers
	 && !(want_xattr_optim && BITS_SET(iflags, ITEM_XNAME_FOLLOWS|ITEM_LOCAL_CHANGE)))
		send_xattr_request(fname, file, f_out);
#endif
}

void send_files(int f_in, int f_out)
{
	int fd = -1;
	struct sum_struct *s;
	struct map_struct *mbuf = NULL;
	STRUCT_STAT st;
	char fname[MAXPATHLEN], xname[MAXPATHLEN];
	const char *path, *slash;
	uchar fnamecmp_type;
	int iflags, xlen;
	struct file_struct *file;
	int phase = 0, max_phase = protocol_version >= 29 ? 2 : 1;
	int itemizing = am_server ? logfile_format_has_i : stdout_format_has_i;
	enum logcode log_code = log_before_transfer ? FLOG : FINFO;
	int f_xfer = write_batch < 0 ? batch_fd : f_out;
	int save_io_error = io_error;
	int ndx, j;
#ifdef HAVE_DUET
	char buf[PATH_MAX];
	long long ino;
	long long inmem;
	struct timeval start_tv, end_tv;
	int64 total_update_time = 0, total_fetch_time = 0;
#endif /* HAVE_DUET */

	if (DEBUG_GTE(SEND, 1))
		rprintf(FINFO, "send_files starting\n");

	while (1) {
#ifdef HAVE_DUET
		if (out_of_order) {
start_another:
			if (pending_o3_files + file_total - file_old_total >= MIN_FILECNT_LOOKAHEAD)
				goto start_inorder;

			gettimeofday(&start_tv, NULL);
			/* Update the itree */
			if (itree_update(&itree, tid, duet_fd)) {
				rprintf(FERROR, "itree_update failed\n");
				exit_cleanup(RERR_DUET);
			}
			gettimeofday(&end_tv, NULL);
			total_update_time += (int64)(end_tv.tv_sec - start_tv.tv_sec) * 1000
					+ (end_tv.tv_usec - start_tv.tv_usec) / 1000;

			/* Send a file out of order */
			gettimeofday(&start_tv, NULL);
			if (itree_fetch(&itree, tid, duet_fd, buf, &ino, &inmem)) {
				if (INFO_GTE(DUET, 3))
					rprintf(FERROR, "duet: nothing to fetch\n");
				exit_cleanup(RERR_DUET);
			}
			stats.total_o3_pages += inmem;
			gettimeofday(&end_tv, NULL);
			total_fetch_time += (int64)(end_tv.tv_sec - start_tv.tv_sec) * 1000
					+ (end_tv.tv_usec - start_tv.tv_usec) / 1000;

			if (buf[0] == '\0') {
				if (INFO_GTE(DUET, 3))
					rprintf(FINFO, "duet: fetch got nothing\n");
				goto start_inorder;
			}

			send_o3_file(f_out, buf);
			pending_o3_files++;
			if (INFO_GTE(DUET, 1))
				rprintf(FINFO, "duet: Sending %s out of order "
					"(inmem = %lld) -- pending-o3=%d, "
					"file_total=%d, file_old_total=%d\n",
					buf, inmem, pending_o3_files, file_total,
					file_old_total);

			if (duet_mark(tid, duet_fd, ino, 1))
				rprintf(FERROR, "duet: failed to mark %s (ino %lld)\n",
					buf, ino);

			if (INFO_GTE(DUET, 3))
				rprintf(FINFO, "duet: Marked %s (ino %lld)\n",
					buf, ino);

			/* If we had less than 800KB in memory there's no point bothering */
//			if (inmem > 100)
			goto start_another;
//			else
//				send_extra_file_list(f_out, -2);
		}
start_inorder:
#endif /* HAVE_DUET */
		if (inc_recurse) {
			send_extra_file_list(f_out, MIN_FILECNT_LOOKAHEAD);
			extra_flist_sending_enabled = !flist_eof;
		}

		/* This call also sets cur_flist. */
		ndx = read_ndx_and_attrs(f_in, f_out, &iflags, &fnamecmp_type,
					 xname, &xlen);
		extra_flist_sending_enabled = False;

#ifdef HAVE_DUET
		if (ndx == NDX_O3_DONE)	{
			if (!am_server && INFO_GTE(PROGRESS, 2))
				end_progress(0);
			if (first_o3_flist)
				flist_free(first_o3_flist);
			pending_o3_files--;
			write_ndx(f_out, NDX_O3_DONE);
			continue;
		}
#endif /* HAVE_DUET */
		if (ndx == NDX_DONE) {
			if (!am_server && INFO_GTE(PROGRESS, 2) && cur_flist) {
				set_current_file_index(NULL, 0);
				end_progress(0);
			}
			if (inc_recurse && first_flist) {
				file_old_total -= first_flist->used;
				flist_free(first_flist);
				if (first_flist) {
					if (first_flist == cur_flist)
						file_old_total = cur_flist->used;
					write_ndx(f_out, NDX_DONE);
					continue;
				}
			}
			if (++phase > max_phase)
				break;
			if (DEBUG_GTE(SEND, 1))
				rprintf(FINFO, "send_files phase=%d\n", phase);
			write_ndx(f_out, NDX_DONE);
			continue;
		}
		if (inc_recurse)
			send_extra_file_list(f_out, MIN_FILECNT_LOOKAHEAD);

#ifdef HAVE_DUET
		if (DEBUG_GTE(SEND, 3)) {
			rprintf(FINFO, "send_files: sending file with ndx %d\n", ndx);
			rprintf(FINFO, "send_files: pending_o3_files = %d\n", pending_o3_files);
		}

		/* Look for o3 file, and if there's none we'll fall through */
		if (cur_o3_flist && cur_o3_flist->ndx_start == ndx) {
			file = cur_o3_flist->files[0];
			goto process_file;
		}

		if (DEBUG_GTE(FLIST, 4))
			output_all_flists("send_files");
#endif /* HAVE_DUET */
		if (ndx - cur_flist->ndx_start >= 0)
			file = cur_flist->files[ndx - cur_flist->ndx_start];
		else
			file = dir_flist->files[cur_flist->parent_ndx];
#ifdef HAVE_DUET
process_file:
#endif /* HAVE_DUET */
		if (F_PATHNAME(file)) {
			path = F_PATHNAME(file);
			slash = "/";
		} else {
			path = slash = "";
		}
		if (!change_pathname(file, NULL, 0))
			continue;
		f_name(file, fname);

		if (DEBUG_GTE(SEND, 1))
			rprintf(FINFO, "send_files(%d, %s%s%s)\n", ndx, path,slash,fname);

#ifdef HAVE_DUET
		if (out_of_order) {
			if (iflags & ITEM_SKIPPED) {
				if (INFO_GTE(DUET, 1))
					rprintf(FINFO, "duet: sender skipping ino %lu\n",
						file->src_ino);

				/* Tell the receiver to not expect any data */
				iflags |= ITEM_SKIPPED;
				if (file->flags & FLAG_O3)
					write_ndx(f_out, NDX_IS_O3);
				write_ndx(f_out, ndx);
				write_int(f_out, iflags);

				/* Flag that we actually sent this entry. */
				file->flags |= FLAG_FILE_SENT;
				continue;
			}
		}

		if (INFO_GTE(DUET, 3))
			rprintf(FINFO, "sending ino %lu\n", file->src_ino);
		current_files++;
#endif /* HAVE_DUET */
#ifdef SUPPORT_XATTRS
		if (preserve_xattrs && iflags & ITEM_REPORT_XATTR && do_xfers
		 && !(want_xattr_optim && BITS_SET(iflags, ITEM_XNAME_FOLLOWS|ITEM_LOCAL_CHANGE)))
			recv_xattr_request(file, f_in);
#endif

		if (!(iflags & ITEM_TRANSFER)) {
			maybe_log_item(file, iflags, itemizing, xname);
			write_ndx_and_attrs(f_out, ndx, iflags, fname, file,
					    fnamecmp_type, xname, xlen);
			if (iflags & ITEM_IS_NEW) {
				stats.created_files++;
				if (S_ISREG(file->mode)) {
					/* Nothing further to count. */
				} else if (S_ISDIR(file->mode))
					stats.created_dirs++;
#ifdef SUPPORT_LINKS
				else if (S_ISLNK(file->mode))
					stats.created_symlinks++;
#endif
				else if (IS_DEVICE(file->mode))
					stats.created_devices++;
				else
					stats.created_specials++;
			}
			continue;
		}
		if (phase == 2) {
			rprintf(FERROR,
				"got transfer request in phase 2 [%s]\n",
				who_am_i());
			exit_cleanup(RERR_PROTOCOL);
		}

		if (file->flags & FLAG_FILE_SENT) {
			if (csum_length == SHORT_SUM_LENGTH) {
				/* For inplace: redo phase turns off the backup
				 * flag so that we do a regular inplace send. */
				make_backups = -make_backups;
				append_mode = -append_mode;
				csum_length = SUM_LENGTH;
			}
		} else {
			if (csum_length != SHORT_SUM_LENGTH) {
				make_backups = -make_backups;
				append_mode = -append_mode;
				csum_length = SHORT_SUM_LENGTH;
			}
			if (iflags & ITEM_IS_NEW)
				stats.created_files++;
		}

		updating_basis_file = inplace && (protocol_version >= 29
			? fnamecmp_type == FNAMECMP_FNAME : make_backups <= 0);

		if (!am_server && INFO_GTE(PROGRESS, 1))
			set_current_file_index(file, ndx);
		stats.xferred_files++;
		stats.total_transferred_size += F_LENGTH(file);

		if (!log_before_transfer)
			remember_initial_stats();

		if (!do_xfers) { /* log the transfer */
			log_item(FCLIENT, file, iflags, NULL);
			write_ndx_and_attrs(f_out, ndx, iflags, fname, file,
					    fnamecmp_type, xname, xlen);
			continue;
		}

		if (!(s = receive_sums(f_in))) {
			io_error |= IOERR_GENERAL;
			rprintf(FERROR_XFER, "receive_sums failed\n");
			exit_cleanup(RERR_PROTOCOL);
		}

		fd = do_open(fname, O_RDONLY, 0);
		if (fd == -1) {
			if (errno == ENOENT) {
				enum logcode c = am_daemon
				    && protocol_version < 28 ? FERROR
							     : FWARNING;
				io_error |= IOERR_VANISHED;
				rprintf(c, "file has vanished: %s\n",
					full_fname(fname));
			} else {
				io_error |= IOERR_GENERAL;
				rsyserr(FERROR_XFER, errno,
					"send_files failed to open %s",
					full_fname(fname));
			}
			free_sums(s);
			if (protocol_version >= 30)
				send_msg_int(MSG_NO_SEND, ndx);
			continue;
		}

		/* map the local file */
		if (do_fstat(fd, &st) != 0) {
			io_error |= IOERR_GENERAL;
			rsyserr(FERROR_XFER, errno, "fstat failed");
			free_sums(s);
			close(fd);
			exit_cleanup(RERR_FILEIO);
		}

#ifdef HAVE_DUET
		if (out_of_order && (file->flags & FLAG_O3)) {
			if (INFO_GTE(DUET, 1))
				rprintf(FINFO, "duet: sending %lu bytes"
					" out of order\n", st.st_size);
			stats.total_o3_written += st.st_size;
		}
#endif /* HAVE_DUET */

		if (st.st_size) {
			int32 read_size = MAX(s->blength * 3, MAX_MAP_SIZE);
			mbuf = map_file(fd, st.st_size, read_size, s->blength);
		} else
			mbuf = NULL;

		if (DEBUG_GTE(DELTASUM, 2)) {
			rprintf(FINFO, "send_files mapped %s%s%s of size %s\n",
				path,slash,fname, big_num(st.st_size));
		}

		write_ndx_and_attrs(f_out, ndx, iflags, fname, file,
				    fnamecmp_type, xname, xlen);
		write_sum_head(f_xfer, s);

		if (DEBUG_GTE(DELTASUM, 2))
			rprintf(FINFO, "calling match_sums %s%s%s\n", path,slash,fname);

		if (log_before_transfer)
			log_item(FCLIENT, file, iflags, NULL);
		else if (!am_server && INFO_GTE(NAME, 1) && INFO_EQ(PROGRESS, 1))
			rprintf(FCLIENT, "%s\n", fname);

		set_compression(fname);

		match_sums(f_xfer, s, mbuf, st.st_size);
		if (INFO_GTE(PROGRESS, 1))
			end_progress(st.st_size);

		log_item(log_code, file, iflags, NULL);

		if (mbuf) {
			j = unmap_file(mbuf);
			if (j) {
				io_error |= IOERR_GENERAL;
				rsyserr(FERROR_XFER, j,
					"read errors mapping %s",
					full_fname(fname));
			}
		}
		close(fd);

		free_sums(s);

		if (DEBUG_GTE(SEND, 1))
			rprintf(FINFO, "sender finished %s%s%s\n", path,slash,fname);

		/* Flag that we actually sent this entry. */
		file->flags |= FLAG_FILE_SENT;
	}
	if (make_backups < 0)
		make_backups = -make_backups;

	if (io_error != save_io_error && protocol_version >= 30)
		send_msg_int(MSG_IO_ERROR, io_error);

	if (DEBUG_GTE(SEND, 1))
		rprintf(FINFO, "send files finished\n");

	match_report();

	write_ndx(f_out, NDX_DONE);
	rprintf(FINFO, "Total time spent updating inode tree: %s seconds.\n",
			comma_dnum((double)total_update_time / 1000, 3));
	rprintf(FINFO, "Total time spent fetching o3 inodes: %s seconds.\n",
			comma_dnum((double)total_fetch_time / 1000, 3));
}
