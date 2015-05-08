duet
====

A framework that leverages synergistic behaviour between maintenance tasks and
the foreground workload.

Ubuntu linux kernel 3.13.6 retrieved from:
git://kernel.ubuntu.com/ubuntu/ubuntu-precise.git (tag:Ubuntu-lts-3.13.0-17.37)

btrfs tools 3.12 retrieved from:
git://git.kernel.org/pub/scm/linux/kernel/git/mason/btrfs-progs.git

f2fs tools 1.4.0 retrieved from:
git.kernel.org/cgit/linux/kernel/git/jaegeuk/f2fs-tools.git (tag: v1.4.0)

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

Note: To speed things up we'll compile on a server, outside the VMs

1. Clone git repo on server
2. Create two virtual machines, one of which (the debugger VM) will have a disk
   with the source code attached
3. On both VMs create a serial port in VM settings (e.g. COM1 on ```/tmp/vmcom1```
   on a Linux host, or ```\\.\pipe\vmcom1``` on a Windows host).
   On the debugger, check the "Create pipe" box.
4. Set up two network interfaces per VM. The first is a 'Bridged Adapter' network
   for access to the outside world. The second is a 'Host-only Adapter' for NFS
   traffic (may need to edit ```/etc/network/interfaces``` and restart networking service).
5. Rsync git files from server to source code disk of debugger VM:

   ```rsync -rtvue "ssh -A fs.csl.utoronto.ca ssh" c159:src/duet/ /media/iris/duet/```

   (make sure that you can connect with passphrase-less keys to fs.csl and the server)
6. Run ```make localmodconfig``` on debugger VM
7. Run ```./setup.sh -c``` on server, to compile
8. Rsync from server to debugger VM
9. Install gdb on debugger VM. Then, create a ```gdbinit``` file that contains:

   ```
   set remotebaud 115200
   target remote /dev/ttyS0
   ```
10. On the debuggee, edit the ```/etc/default/grub``` line:

   ```
   GRUB_LINUX_DEFAULT="kgdboc=ttyS0,115200 kgdbwait"
   and run 'sudo update-grub'
   ```
11. To get NFS server up and running on the debugger, do:

   ```
   # apt-get install nfs-kernel-server
   # mkdir -p /export/iris
   ```

   Update /etc/fstab:

   ```
   /home/users    /export/users   none    bind  0  0
   ```

   Update /etc/exports:

   ```
   /export       192.168.56.0/24(rw,fsid=0,insecure,no_subtree_check,async)
   /export/iris  192.168.56.0/24(rw,nohide,insecure,no_subtree_check,async)
   ```
12. To get NFS client up and running on the debuggee, do:

   ```
   # apt-get install nfs-common
   ```

   Update /etc/fstab:

   ```
   192.168.56.101:/   /mnt   nfs    auto  0  0
   ```
13. Reboot debuggee. On debugger run ```sudo gdb -x gdbinit vmlinux``` and press
   ```c``` to allow debuggee to boot.

If you want to recompile, do so on the server. Then follow step 3 to pull changes
to both VMs and install on the debuggee, then reboot as per step 13.

On Ubuntu server, you can change hostname by editing ```/etc/hostname```,
```/etc/hosts```, and running ```sudo service hostname restart```.

