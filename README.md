duet
====

A framework that leverages synergistic behaviour between maintenance tasks and
the foreground workload.

Ubuntu linux kernel 3.13.6 retrieved from:
git://kernel.ubuntu.com/ubuntu/ubuntu-precise.git (tag:Ubuntu-lts-3.13.0-17.37)

btrfs tools 3.12 retrieved from:
git://git.kernel.org/pub/scm/linux/kernel/git/mason/btrfs-progs.git

Telling foreground from background:
-----------------------------------

In order to detect which blocks get accessed in the critical path (file system
foreground workload), we need to intercept the following functions:
- btrfsic_submit_bio, and submit_bio when called directly
- btrfsic_submit_bio_wait, and submit_bio_wait when called directly
- btrfsic_submit_bh, and submit_bh when called directly
  (currently only used by disk-io.c:write_supers, and ignored there)

Readahead:
----------

readahead only prefetches metadata, and inline data. extents are usually not
stored in the extent tree (unless the data is inlined).

Patches applied to btrfs send:
------------------------------

[PATCH] Btrfs: remove transaction from btrfs send [Wang Shilong]
[PATCH v2] Btrfs: do not use extent commit root for sending [Wang Shilong]
