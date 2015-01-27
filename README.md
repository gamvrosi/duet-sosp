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

Ubuntu Dependencies:
-------------

For the kernel:

build-essential
kernel-package
libncurses5-dev

For the tools:

uuid-dev
libblkid-dev
libz-dev
liblzo2-dev
libattr1-dev
libacl1-dev
e2fslibs-dev

Setting up inter-VM linux kernel debugging (for VirtualBox 4.1+):
----------------------------------------------------------------

Note: To speed things up, compile on a server, outside the VMs

1. Clone git repo on server
2. Create a new virtual machine
3. Rsync git files from server:
  rsync -rtvue "ssh -A fs.csl.utoronto.ca ssh" ./duet/ c159:src/iris/duet/
  (make sure that you can connect with passphrase-less keys to fs.csl and the server)
4. Run 'make localmodconfig' on VM
5. Run ./setup.sh -c on server, to compile
6. Rsync from server to VM
7. Install gdb on VM and shutdown.
8. Clone VM. One VM is now the debugger, and the other the debuggee.
9. On both VMs create a serial port in VM settings (e.g. COM1 on /tmp/vmcom1).
   On the debugger, check the "Create pipe" box.
10. On the debugger, create a gdbinit file that contains:
   set remotebaud 115200
   target remote /dev/ttyS0
11. On the debuggee, you can change hostname by editing /etc/hostname, /etc/hosts, and
   running 'sudo service hostname restart'
12. On the debuggee, edit the /etc/default/grub line:
   GRUB_LINUX_DEFAULT="kgdboc=ttyS0,115200 kgdbwait"
   and run 'sudo update-grub'
13. Reboot debuggee. On debugger run 'sudo gdb -x gdbinit vmlinux' and press 'c' to
   allow debuggee to boot.

If you want to recompile, do so on the server. Then follow step 3 to pull changes
to both VMs and install on the debuggee, then reboot as per step 13.

