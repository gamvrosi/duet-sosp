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

For the btrfs tools:

uuid-dev
libblkid-dev
libz-dev
liblzo2-dev
libattr1-dev
libacl1-dev
e2fslibs-dev

For the f2fs tools:

autoconf
libtool
pkg-config

For rsync:

libpopt-dev
yodl

Setting up inter-VM linux kernel debugging (for VirtualBox 4.1+):
----------------------------------------------------------------

Note: To speed things up we'll compile on a server, outside the VMs

1. Clone git repo on server
2. Create two virtual machines, one of which (the debugger VM) will have a disk
   with the source code attached
3. On both VMs create a serial port in VM settings (e.g. COM1 on ```/tmp/vmcom1```
   on a Linux host, or ```\\.\pipe\vmcom1``` on a Windows host). On the debugger,
   check the "Create pipe" box. However, if on Windows 7 you may need to check the
   "Create pipe" box in the debugee to avoid getting initial messages scrambled
   (not sure why this happens).
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
10. On the debuggee, edit ```/etc/default/grub```:

   ```
   GRUB_DEFAULT="Ubuntu, with Linux 3.13.6+duet"
   GRUB_DISABLE_SUBMENU=y
   GRUB_LINUX_DEFAULT="kgdboc=ttyS0,115200 kgdbwait"
   ```

   Then do:

   ```
   # sudo update-grub
   ```
11. To get NFS server up and running on the debugger, do:

   ```
   # apt-get install nfs-kernel-server
   # mkdir -p /export/iris
   ```

   Update /etc/fstab:

   ```
   /home/iris    /export/iris   none    bind  0  0
   ```

   Update /etc/exports:

   ```
   /export       192.168.56.0/24(rw,fsid=0,insecure,no_subtree_check,async)
   /export/iris  192.168.56.0/24(rw,nohide,insecure,no_subtree_check,async)
   ```

   Then run:

   ```
   # exportfs -a
   ```
12. To get NFS client up and running on the debuggee, do:

   ```
   # apt-get install nfs-common
   ```

   Update /etc/fstab:

   ```
   192.168.56.101:/iris   /media/iris   nfs    auto  0  0
   ```
13. Reboot debuggee. On debugger run ```sudo gdb -x gdbinit vmlinux``` and press
   ```c``` to allow debuggee to boot.

If you want to recompile, do so on the server. Then follow step 3 to pull changes
to both VMs and install on the debuggee, then reboot as per step 13.

On Ubuntu server, you can change hostname by editing ```/etc/hostname```,
```/etc/hosts```, and running ```sudo service hostname restart```.


Using perf with Duet
----------------------

_perf_ is a powerful tool for profiling, tracing and instrumenting the Linux
kernel, and is extremely useful for diagnosing performance issues that cross
the userspace/kernel system call boundary. In order to use perf with the Duet
kernel, include the following options in your build configuration:

 * `CONFIG_KPROBES=y`
 * `CONFIG_HAVE_KPROBES=y`
 * `CONFIG_KPROBES_ON_FTRACE=y`
 * `CONFIG_KPROBE_EVENT=y`
 * `CONFIG_UPROBES=y`
 * `CONFIG_UPROBE_EVENT=y`
 * `CONFIG_FTRACE=y`
 * `CONFIG_FTRACE_SYSCALLS=y`
 * `CONFIG_DYNAMIC_FTRACE=y`
 * `CONFIG_FUNCTION_PROFILER=y`
 * `CONFIG_FUNCTION_TRACER=y`
 * `CONFIG_FUNCTION_GRAPH_TRACR=y`
 * `CONFIG_STACKTRACE_SUPPORT=y`
 * `CONFIG_TRACEPOINTS=y`
 * `CONFIG_PERF_EVENTS=y`

Also, `CONFIG_DEBUG_INFO=y` and `CONFIG_DEBUG_KERNEL=y` are a good idea too (be sure to run `./setup.sh -g` and install the resulting `linux-image-3.13.6+duet-$SHA1-dbg` package in order to give perf the necessary debug info found under `/usr/lib/debug/lib/modules/3.13.6+duet-$SHA1/vmlinux`.)

Unfortunately, the userspace tool found in `linux-3.13.6+duet/tools/perf` is
unusable. Instead, on Debian and Ubuntu systems, install the
`linux-tools-X.Y.Z-W` package, where the kernel version _X.Y.Z-W_ is whichever
is the highest available as reported by `apt-cache search linux-tools` (at the
time of this writing, on Ubuntu 15.10 "Wily" this is in the 4.2.0 range.)

Next, as `root` run the `perf` binary found under
`/usr/lib/linux-tools-X.Y.Z-W/perf`. If you see a message such as _WARNING:
perf not found for kernel 3.13.6+duet_, you have actually only run the shell
script frontend `/usr/bin/perf` installed by the `linux-tools-common` package,
and not the real binary.

### A few basic perf commands

 * `perf top`: see which kernel and userspace process functions are consuming the most CPU
 * `perf top --call-graph dwarf,1024`: see which functions are consuming CPU along with the graph of callsites that invoke them, broken down by CPU usage
 * `perf trace -p $PID1,$PID2,...`: trace system calls and events in the spirit of _strace_ but with much less performance impact on the traced process

### Useful perf links

 * <https://perf.wiki.kernel.org/index.php/Main_Page>
 * <https://perf.wiki.kernel.org/index.php/Tutorial>
 * <http://www.brendangregg.com/perf.html>
