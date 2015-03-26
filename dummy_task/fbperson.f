#
# Webserver personality, as found in Filebench 1.4.9.1, webserver.f
#

set $dir=/media/btrfs-test
set $nfiles=45000
set $meandirwidth=20
set $meanfilesize=1m
set $nthreads=1
set $iosize=1m
set $appendsize=128k
set $eventrate=101

eventgen rate=$eventrate


define fileset name=bigfileset, path=$dir, size=$meanfilesize,
               entries=$nfiles, dirwidth=$meandirwidth, prealloc=100
define fileset name=logfiles, path=$dir, size=$meanfilesize,
               entries=1, dirwidth=$meandirwidth, prealloc

define process name=filereader,instances=1
{
  thread name=filereaderthread,memsize=10m,instances=$nthreads
  {
    flowop openfile name=openfile1,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile1,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit1, target=readfile1
    flowop closefile name=closefile1,fd=1
    flowop openfile name=openfile2,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile2,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit2, target=readfile2
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile3,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile3,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit3, target=readfile3
    flowop closefile name=closefile3,fd=1
    flowop openfile name=openfile4,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile4,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit4, target=readfile4
    flowop closefile name=closefile4,fd=1
    flowop openfile name=openfile5,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile5,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit5, target=readfile5
    flowop closefile name=closefile5,fd=1
    flowop openfile name=openfile6,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile6,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit6, target=readfile6
    flowop closefile name=closefile6,fd=1
    flowop openfile name=openfile7,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile7,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit7, target=readfile7
    flowop closefile name=closefile7,fd=1
    flowop openfile name=openfile8,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile8,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit8, target=readfile8
    flowop closefile name=closefile8,fd=1
    flowop openfile name=openfile9,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile9,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit9, target=readfile9
    flowop closefile name=closefile9,fd=1
    flowop openfile name=openfile10,filesetname=bigfileset,fd=1
    flowop readwholefile name=readfile10,fd=1,iosize=$iosize
    flowop bwlimit name=serverlimit10, target=readfile10
    flowop closefile name=closefile10,fd=1
    flowop appendfile name=appendlog,filesetname=logfiles,iosize=$appendsize,fd=2
    flowop bwlimit name=serverlimitA, target=appendlog
  }
}

echo  "Web-server Version 3.0 personality successfully loaded"
