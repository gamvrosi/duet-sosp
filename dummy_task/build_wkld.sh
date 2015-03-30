#!/bin/bash

# A function to compile the probability table for file hotness, for a given
# number of files ($nfiles)
compile_probtable () {
	fbprobtable="
define randvar name=\$fileidx, type=tabular, min=1, round=1, randtable=
{{10, $(( 0*$nfiles/100)), $(( 40*$nfiles/100))},
 {10, $((40*$nfiles/100)), $(( 60*$nfiles/100))},
 {10, $((60*$nfiles/100)), $(( 71*$nfiles/100))},
 {10, $((71*$nfiles/100)), $(( 79*$nfiles/100))},
 {10, $((79*$nfiles/100)), $(( 85*$nfiles/100))},
 {10, $((85*$nfiles/100)), $(( 90*$nfiles/100))},
 {10, $((90*$nfiles/100)), $(( 94*$nfiles/100))},
 {10, $((94*$nfiles/100)), $(( 97*$nfiles/100))},
 {10, $((97*$nfiles/100)), $(( 99*$nfiles/100))},
 {10, $((99*$nfiles/100)), $((100*$nfiles/100))}}"
}

# This function generates the filebench personality and profile files that
# will be used for the experiments
# @1: filebench personality name
# @2: disk utilization (%)
# @3: hot data (% of fs capacity)
# @4: use a realistic, empirical distribution for file accesses
compileprof () {
	case $1 in
	"var")	# Varmail personality
		fbwarmup=120
		if [[ $4 -eq 1 ]]; then
			case $2 in #TODO: profile for real hotness
			0)	evrate="1"	;;
			10)	evrate="3"	;;
			20)	evrate="6"	;;
			30)	evrate="9"	;;
			40)	evrate="12"	;;
			50)	evrate="16"	;;
			60)	evrate="20"	;;
			70)	evrate="24"	;;
			80)	evrate="27"	;;
			90)	evrate="30"	;;
			100)	evrate="41"	;;
			esac
		else
			case $2 in
			0)	evrate="1"	;;
			10)	evrate="3"	;;
			20)	evrate="6"	;;
			30)	evrate="9"	;;
			40)	evrate="12"	;;
			50)	evrate="16"	;;
			60)	evrate="20"	;;
			70)	evrate="24"	;;
			80)	evrate="27"	;;
			90)	evrate="30"	;;
			100)	evrate="41"	;;
			esac
		fi

		case $3 in
		25)	nfiles="12000"	;;
		50)	nfiles="23000"	;;
		75)	nfiles="34000"	;;
		100)	nfiles="45000"	;;
		esac

		if [[ $4 -eq 1 ]]; then
			compile_probtable
			fbindexattr=",indexed=\$fileidx"
		else
			fbprobtable=""
			fbindexattr=""
		fi

		fbperson="#
# Varmail personality, as found in Filebench 1.4.9.1, varmail.f
#

set \$dir=$fspath
set \$nfiles=$nfiles
set \$meandirwidth=10000
set \$meanfilesize=1m
set \$appendsize=16k
set \$nthreads=16
set \$iosize=1m
set \$eventrate=$evrate

eventgen rate=\$eventrate
$fbprobtable

define fileset name=bigfileset, path=\$dir, size=\$meanfilesize,
               entries=\$nfiles, dirwidth=\$meandirwidth, prealloc=80

define process name=filereader, instances=1
{
  thread name=filereaderthread, memsize=10m, instances=\$nthreads
  {
    flowop deletefile name=deletefile1,filesetname=bigfileset
    flowop createfile name=createfile2,filesetname=bigfileset,fd=1
    flowop appendfile name=appendfile2,iosize=\$appendsize,fd=1
    #flowop appendfilerand name=appendfilerand2,iosize=\$appendsize,fd=1
    flowop bwlimit name=serverlimit2, target=appendfile2
    flowop fsync name=fsyncfile2,fd=1
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile3,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile3,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit3a, target=readfile3
    flowop appendfile name=appendfile3,iosize=\$appendsize,fd=1
    #flowop appendfilerand name=appendfilerand3,iosize=\$appendsize,fd=1
    flowop bwlimit name=serverlimit3b, target=appendfile3
    flowop fsync name=fsyncfile3,fd=1
    flowop closefile name=closefile3,fd=1
    flowop openfile name=openfile4,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile4,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit4, target=readfile4
    flowop closefile name=closefile4,fd=1
    #flowop opslimit name=serverlimit
  }
}

echo  \"Varmail Version 3.0 personality successfully loaded\""
		;;

	"wsv")	# Webserver personality
		fbwarmup=120
		if [[ $4 -eq 1 ]]; then
			case $3 in
			25)	nfiles="12000"
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="13"	;;
				20)	evrate="26"	;;
				30)	evrate="43"	;;
				40)	evrate="57"	;;
				50)	evrate="71"	;;
				60)	evrate="88"	;;
				70)	evrate="105"	;;
				80)	evrate="122"	;;
				90)	evrate="139"	;;
				100)	evrate="160"	;;
				esac			;;
			50)	nfiles="23000"
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="11"	;;
				20)	evrate="22"	;;
				30)	evrate="33"	;;
				40)	evrate="45"	;;
				50)	evrate="57"	;;
				60)	evrate="70"	;;
				70)	evrate="82"	;;
				80)	evrate="96"	;;
				90)	evrate="110"	;;
				100)	evrate="125"	;;
				esac			;;
			75)	nfiles="34000"
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="10"	;;
				20)	evrate="18"	;;
				30)	evrate="28"	;;
				40)	evrate="38"	;;
				50)	evrate="48"	;;
				60)	evrate="58"	;;
				70)	evrate="69"	;;
				80)	evrate="81"	;;
				90)	evrate="92"	;;
				100)	evrate="108"	;;
				esac			;;
			100)	nfiles="45000"
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="10"	;;
				20)	evrate="18"	;;
				30)	evrate="28"	;;
				40)	evrate="37"	;;
				50)	evrate="46"	;;
				60)	evrate="55"	;;
				70)	evrate="64"	;;
				80)	evrate="74"	;;
				90)	evrate="84"	;;
				100)	evrate="104"	;;
				esac			;;
			esac
		else
			if [[ $eval_ssd -eq 1 ]]; then
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="28"	;;
				20)	evrate="56"	;;
				30)	evrate="84"	;;
				40)	evrate="112"	;;
				50)	evrate="136"	;;
				60)	evrate="165"	;;
				70)	evrate="195"	;;
				80)	evrate="235"	;;
				90)	evrate="275"	;;
				100)	evrate="330"	;;
				esac
			else
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="8"	;;
				20)	evrate="16"	;;
				30)	evrate="25"	;;
				40)	evrate="34"	;;
				50)	evrate="44"	;;
				60)	evrate="53"	;;
				70)	evrate="62"	;;
				80)	evrate="72"	;;
				90)	evrate="82"	;;
				100)	evrate="101"	;;
				esac
			fi

			case $3 in
			25)	nfiles="12000"	;;
			50)	nfiles="23000"	;;
			75)	nfiles="34000"	;;
			100)	nfiles="45000"	;;
			esac
		fi

		if [[ $4 -eq 1 ]]; then
			compile_probtable
			fbindexattr=",indexed=\$fileidx"
		else
			fbprobtable=""
			fbindexattr=""
		fi

		if [[ $defrag_reuse -eq 1 ]]; then
			reusestr=", reuse"
		else
			reusestr=""
		fi

		fbperson="#
# Webserver personality, as found in Filebench 1.4.9.1, webserver.f
#

set \$dir=$fspath
set \$nfiles=$nfiles
set \$meandirwidth=20
set \$meanfilesize=1m
set \$nthreads=1
set \$iosize=1m
set \$appendsize=128k
set \$eventrate=$evrate

eventgen rate=\$eventrate
$fbprobtable

define fileset name=bigfileset, path=\$dir, size=\$meanfilesize,
               entries=\$nfiles, dirwidth=\$meandirwidth, prealloc=100$reusestr
define fileset name=logfiles, path=\$dir, size=\$meanfilesize,
               entries=1, dirwidth=\$meandirwidth, prealloc$reusestr

define process name=filereader,instances=1
{
  thread name=filereaderthread,memsize=10m,instances=\$nthreads
  {
    flowop openfile name=openfile1,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile1,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit1, target=readfile1
    flowop closefile name=closefile1,fd=1
    flowop openfile name=openfile2,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile2,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit2, target=readfile2
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile3,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile3,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit3, target=readfile3
    flowop closefile name=closefile3,fd=1
    flowop openfile name=openfile4,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile4,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit4, target=readfile4
    flowop closefile name=closefile4,fd=1
    flowop openfile name=openfile5,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile5,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit5, target=readfile5
    flowop closefile name=closefile5,fd=1
    flowop openfile name=openfile6,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile6,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit6, target=readfile6
    flowop closefile name=closefile6,fd=1
    flowop openfile name=openfile7,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile7,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit7, target=readfile7
    flowop closefile name=closefile7,fd=1
    flowop openfile name=openfile8,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile8,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit8, target=readfile8
    flowop closefile name=closefile8,fd=1
    flowop openfile name=openfile9,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile9,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit9, target=readfile9
    flowop closefile name=closefile9,fd=1
    flowop openfile name=openfile10,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile10,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit10, target=readfile10
    flowop closefile name=closefile10,fd=1
    flowop appendfile name=appendlog,filesetname=logfiles,iosize=\$appendsize,fd=2
    flowop bwlimit name=serverlimitA, target=appendlog
  }
}

echo  \"Web-server Version 3.0 personality successfully loaded\""
		;;

	"wpx")	# Webproxy personality
		fbwarmup=120
		if [[ $4 -eq 1 ]]; then
			case $3 in
			25)	nfiles="15000"	# Should be 12k, but we prealloc 80%
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="13"	;;
				20)	evrate="26"	;;
				30)	evrate="41"	;;
				40)	evrate="54"	;;
				50)	evrate="71"	;;
				60)	evrate="76"	;;
				70)	evrate="80"	;;
				80)	evrate="94"	;;
				90)	evrate="96"	;;
				100)	evrate="110"	;;
				esac	;;
			50)	nfiles="28000"	# Should be 23k, but we prealloc 80%
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="11"	;;
				20)	evrate="21"	;;
				30)	evrate="31"	;;
				40)	evrate="41"	;;
				50)	evrate="50"	;;
				60)	evrate="67"	;;
				70)	evrate="73"	;;
				80)	evrate="80"	;;
				90)	evrate="87"	;;
				100)	evrate="99"	;;
				esac	;;
			75)	nfiles="42000"	# Should be 34k, but we prealloc 80%
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="10"	;;
				20)	evrate="17"	;;
				30)	evrate="26"	;;
				40)	evrate="34"	;;
				50)	evrate="44"	;;
				60)	evrate="53"	;;
				70)	evrate="68"	;;
				80)	evrate="75"	;;
				90)	evrate="83"	;;
				100)	evrate="91"	;;
				esac	;;
			100)	nfiles="55000"	# Should be 45k, but we prealloc 80%
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="8"	;;
				20)	evrate="16"	;;
				30)	evrate="25"	;;
				40)	evrate="34"	;;
				50)	evrate="44"	;;
				60)	evrate="54"	;;
				70)	evrate="63"	;;
				80)	evrate="73"	;;
				90)	evrate="83"	;;
				100)	evrate="92"	;;
				esac	;;
			esac
		else
			case $2 in
			0)	evrate="1"	;;
			10)	evrate="7"	;;
			20)	evrate="14"	;;
			30)	evrate="21"	;;
			40)	evrate="28"	;;
			50)	evrate="37"	;;
			60)	evrate="45"	;;
			70)	evrate="54"	;;
			80)	evrate="64"	;;
			90)	evrate="72"	;;
			100)	evrate="80"	;;
			esac

			case $3 in
			25)	nfiles="15000"	;; # Should be 12k, but we prealloc 80%
			50)	nfiles="28000"	;; # Should be 23k, but we prealloc 80%
			75)	nfiles="42000"	;; # Should be 34k, but we prealloc 80%
			100)	nfiles="55000"	;; # Should be 45k, but we prealloc 80%
			esac
		fi

		if [[ $4 -eq 1 ]]; then
			compile_probtable
			fbindexattr=",indexed=\$fileidx"
		else
			fbprobtable=""
			fbindexattr=""
		fi

		if [[ $defrag_reuse -eq 1 ]]; then
			reusestr=", reuse"
		else
			reusestr=""
		fi

		fbperson="#
# Webproxy personality, as found in Filebench 1.4.9.1, webproxy.f
#

set \$dir=$fspath
set \$nfiles=$nfiles
# set \$meandirwidth=1000000
set \$meandirwidth=20
set \$meanfilesize=1m
set \$nthreads=16
set \$meaniosize=1m
set \$iosize=1m
set \$eventrate=$evrate

eventgen rate=\$eventrate
$fbprobtable

define fileset name=bigfileset,path=\$dir,size=\$meanfilesize,entries=\$nfiles,
               dirwidth=\$meandirwidth,prealloc=80$reusestr

define process name=proxycache,instances=1
{
  thread name=proxycache,memsize=10m,instances=\$nthreads
  {
    flowop deletefile name=deletefile1,filesetname=bigfileset
    flowop createfile name=createfile1,filesetname=bigfileset,fd=1
    flowop appendfile name=appendfile1,iosize=\$meaniosize,fd=1
    flowop bwlimit name=serverlimit1, target=appendfile1
    flowop closefile name=closefile1,fd=1
    flowop openfile name=openfile2,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile2,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit2, target=readfile2
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile3,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile3,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit3, target=readfile3
    flowop closefile name=closefile3,fd=1
    flowop openfile name=openfile4,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile4,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit4, target=readfile4
    flowop closefile name=closefile4,fd=1
    flowop openfile name=openfile5,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile5,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit5, target=readfile5
    flowop closefile name=closefile5,fd=1
    flowop openfile name=openfile6,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile6,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimit6, target=readfile6
    flowop closefile name=closefile6,fd=1
  }
}

echo  \"Web proxy-server Version 3.0 personality successfully loaded\""
		;;

	"fsv")	# Fileserver personality
		fbwarmup=120
		if [[ $4 -eq 1 ]]; then
			case $3 in
			25)	nfiles="6000"	# 12000 x 1m
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="11"	;;
				20)	evrate="21"	;;
				30)	evrate="31"	;;
				40)	evrate="41"	;;
				50)	evrate="51"	;;
				60)	evrate="62"	;;
				70)	evrate="72"	;;
				80)	evrate="84"	;;
				90)	evrate="96"	;;
				100)	evrate="107"	;;
				esac	;;
			50)	nfiles="11500"	# 23000 x 1m
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="10"	;;
				20)	evrate="17"	;;
				30)	evrate="26"	;;
				40)	evrate="35"	;;
				50)	evrate="44"	;;
				60)	evrate="53"	;;
				70)	evrate="63"	;;
				80)	evrate="71"	;;
				90)	evrate="79"	;;
				100)	evrate="85"	;;
				esac	;;
			75)	nfiles="17000"	# 34000 x 1m
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="9"	;;
				20)	evrate="15"	;;
				30)	evrate="24"	;;
				40)	evrate="33"	;;
				50)	evrate="42"	;;
				60)	evrate="51"	;;
				70)	evrate="59"	;;
				80)	evrate="66"	;;
				90)	evrate="73"	;;
				100)	evrate="80"	;;
				esac	;;
			100)	nfiles="22500"	# 45000 x 1m
				case $2 in
				0)	evrate="1"	;;
				10)	evrate="8"	;;
				20)	evrate="17"	;;
				30)	evrate="26"	;;
				40)	evrate="35"	;;
				50)	evrate="44"	;;
				60)	evrate="52"	;;
				70)	evrate="59"	;;
				80)	evrate="68"	;;
				90)	evrate="75"	;;
				100)	evrate="84"	;;
				esac	;;
			esac
		else
			case $2 in
			0)	evrate="1"	;;
			10)	evrate="5"	;;
			20)	evrate="10"	;;
			30)	evrate="17"	;;
			40)	evrate="22"	;;
			50)	evrate="29"	;;
			60)	evrate="34"	;;
			70)	evrate="40"	;;
			80)	evrate="45"	;;
			90)	evrate="50"	;;
			100)	evrate="56"	;;
			esac

			case $3 in
			25)	nfiles="6000"	;; # 12000 x 1m
			50)	nfiles="11500"	;; # 23000 x 1m
			75)	nfiles="17000"	;; # 34000 x 1m
			100)	nfiles="22500"	;; # 45000 x 1m
			esac
		fi

		if [[ $4 -eq 1 ]]; then
			compile_probtable
			fbindexattr=",indexed=\$fileidx"
		else
			fbprobtable=""
			fbindexattr=""
		fi

		if [[ $defrag_reuse -eq 1 ]]; then
			reusestr=", reuse"
		else
			reusestr=""
		fi

		fbperson="#
# Fileserver personality, as found in Filebench 1.4.9.1, fileserver.f
#

set \$dir=$fspath
set \$nfiles=$nfiles
set \$meandirwidth=20
#set \$meanfilesize=1m
set \$meanfilesize=2m
set \$nthreads=16
set \$iosize=1m
set \$appendsize=16k
set \$eventrate=$evrate

eventgen rate=\$eventrate
$fbprobtable

define fileset name=bigfileset, path=\$dir, size=\$meanfilesize, entries=\$nfiles,
               dirwidth=\$meandirwidth, prealloc=97$reusestr

define process name=filereader, instances=1
{
  thread name=filereaderthread, memsize=10m, instances=\$nthreads
  {
    flowop createfile name=createfile1,filesetname=bigfileset,fd=1
    flowop writewholefile name=wrtfile1,srcfd=1,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimitW, target=wrtfile1
    flowop closefile name=closefile1,fd=1
    flowop openfile name=openfile1,filesetname=bigfileset,fd=1$fbindexattr
    flowop appendfile name=appendfile1,iosize=\$appendsize,fd=1
    flowop bwlimit name=serverlimitA, target=appendfile1
    flowop closefile name=closefile2,fd=1
    flowop openfile name=openfile2,filesetname=bigfileset,fd=1$fbindexattr
    flowop readwholefile name=readfile1,fd=1,iosize=\$iosize
    flowop bwlimit name=serverlimitR, target=readfile1
    flowop closefile name=closefile3,fd=1
    flowop deletefile name=deletefile1,filesetname=bigfileset
    flowop statfile name=statfile1,filesetname=bigfileset
  }
}

echo  \"File-server Version 3.0 personality successfully loaded\""
		;;
	esac

	echo -e "$fbperson" > fbperson.f

	echo -e "load fbperson\ncreate filesets\ncreate processes
stats clear\nsleep $fbwarmup\nstats snap" > $fbprof
	iters=`echo $explen/$profgran | bc`
	for i in $(seq 1 $iters); do
		echo -e "stats clear\nsleep $profgran\nstats snap" >> $fbprof
	done
	echo -e "shutdown processes\nquit" >> $fbprof
}
