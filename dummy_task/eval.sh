#!/bin/bash -x

# Sinkpart is where rsync dumps data
fspart="/dev/sda3"
sinkpart="/dev/sdb6"

# Set up experiment variables
# Supported workloads: "varmail", "webserver", "webproxy", "fileserver"
workload="webserver"
numreps=1

# Usually you shouldn't have to touch these
wkldutil=(100) #(0 10 20 30 40 50 60 70 80 90 100) #(50)
hotcold=(100)
fspath="/media/btrfs-test"
fbprof="filebench.prof"
tmpout="fbtmp.out"
outdir="./rsync_results/"
explen=8000 #1800
profgran=20
do_idle=0
eval_ssd=0

# These are rsync-specific. Comment out if not needed.
sinkpath="/media/btrfs-sink"
rsyncpath="/home/gamvrosi/src/duet/rsync-3.1.1+duet/rsync"

source build_wkld.sh

collect_idle () {
	idleoutput=`sudo blktrace -d $fspart -a issue -a complete -o - | \
		blkparse -i - -f "%a\t%d\t%N\t%S\t%T.%t\n" | \
		awk -F'\t' 'BEGIN{
			last = 0.0
			gcint = 20.0
		}
		{
			type = $2;
			num = strtonum($3);
			sector = strtonum($4);

			if ($1 == "D") {
				# add event: events(isread, bytes, sectors) => time
				events[type][num][sector] = $5;
			} else if ($1 == "C") {
				# match and delete event from array
				if (type in events && num in events[type] &&
				    sector in events[type][num]) {
					time = events[type][num][sector];
					if ($5 - time < 1.0)
						print time"\t"$5;
					delete events[type][num][sector];
					if (length(events[type][num]) == 0)
						delete events[type][num];
				}

				if ($5 - last > gcint) {
					for (r in events) {
						for (n in events[r]) {
							for (s in events[r][n])
								if ($5 - events[r][n][s] > gcint)
									delete events[r][n][s];
							if (length(events[r][n]) == 0)
								delete events[r][n];
						}
					}
					last = $5;
				}
			}
		}'` && echo -e "$idleoutput" >iter_idle.tmp &
#
#	idleoutput=`sudo blktrace -d $fspart -a issue -a complete -o - | \
#	blkparse -i - -f "%a\t%d\t%N\t%S\t%T.%t\n" | \
#	awk -F'\t' '{ # Spit out active intervals	
#		if ($1 == "D") {
#			# add event: events(isread, bytes, sectors) => time
#			events[ $2","$3","$4 ] = $5
#		} else if ($1 == "C") {
#			# match and delete event from array
#			if ( $2","$3","$4 in events ) {
#				time = events[ $2","$3","$4 ]
#				if ($5 - time < 1.0) {
#					# print event info: enter, leave
#					print time"\t"$5
#				}
#				delete events[ $2","$3","$4 ]
#			}
#
#			for (e in events) {
#				if ($5 - events[e] > 60.0)
#					delete events[e]
#			}
#		}
#	}'` && echo -e "$idleoutput" > iter_idle.tmp &
}

# Runs one iteration of an experiment
runiter () {
	duet status stop
	if [ $dueton -eq 1 ]; then
		echo -ne "- Starting the duet framework... " | tee -a $output
		duet status start
		echo -ne "Done.\n" | tee -a $output
	fi

	echo -ne "- Clearing buffer cache... " | tee -a $output
	sudo sh -c "sync && echo 3 > /proc/sys/vm/drop_caches"
	echo -ne "Done.\n" | tee -a $output

	# Start filebench and maintenance task (if applicable)
	echo "" > $tmpout
	running=0 # number of times we've seen the "Running..." sequence
	case $bgtask in
	0|4)	# No maintenance task
		echo -ne "- Running filebench... " | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
			elif [ $running == 1 ]; then
				running=2
				# Start collecting idle interval data
				if [[ $do_idle == 1 ]]; then
					collect_idle
				fi
			fi
		done

		if [[ $do_idle == 1 ]]; then
			# Stop collecting idleness data
			blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
				tr -s ' ' | cut -d' ' -f 2`"
			echo -ne "- Killing blktrace... " | tee -a $output
			while [[ "$blkpid" != "" ]]; do
				echo -ne "x" | tee -a $output
				sudo kill -2 $blkpid
				sleep 1
				blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
					tr -s ' ' | cut -d' ' -f 2`"
			done
			echo -e " ...Done." | tee -a $output
		fi

		# No stats
		iter_time="0"; iter_data="0"; iter_slat="0"
		;;
	[1-2])	# Scrubbing
		scargs=""
		if [ $bgtask -eq 2 ]; then
			scargs="-D $explen -E $scargs"
		fi

		echo "- Running filebench" | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
			elif [ $running == 1 ]; then
				running=2
				# Start collecting idle interval data
				if [[ $do_idle == 1 ]]; then
					collect_idle
				fi

				# Start scrubber
				echo "- Starting the scrubber..." | tee -a $output
				sudo btrfs scrub start $fspath $scargs | tee -a $output &
				echo -ne "" > iter_work.tmp
				num_work=0
			else
				# Collect data on work being completed
				bytes_scrubbed=""
				while [ "$bytes_scrubbed" == "" ]; do
					bytes_scrubbed=`sudo btrfs scrub status -R $fspath | \
					grep "bytes_scrubbed" | \
					sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`
					sleep 1
				done
				echo -ne "$bytes_scrubbed, " >> iter_work.tmp
				num_work=$((num_work+1))
				if [ $(( $num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> iter_work.tmp
				fi
			fi
		done

		if [[ $do_idle == 1 ]]; then
			# Stop collecting idleness data
			blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
				tr -s ' ' | cut -d' ' -f 2`"
			echo -ne "- Killing blktrace... " | tee -a $output
			while [[ "$blkpid" != "" ]]; do
				echo -ne "x" | tee -a $output
				sudo kill -2 $blkpid
				sleep 1
				blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
					tr -s ' ' | cut -d' ' -f 2`"
			done
			echo -e " ...Done." | tee -a $output
		fi

		# Grab scrubbing work stats and delete tmp file
		iter_work="`cat iter_work.tmp`"
		rm iter_work.tmp

		# Wait for filebench to finish, then poll scrubber for stats, and stop blktrace
		bytes_scrubbed=`sudo btrfs scrub status -R $fspath | \
			grep "bytes_scrubbed" | \
			sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`
		iter_work="$iter_work$bytes_scrubbed"

		echo "- Waiting for the scrubber and collecting stats" | tee -a $output
		while [ 1 ]; do
			scrubstats=`sudo btrfs scrub status $fspath | \
				grep "scrub started at .* and finished after .* seconds"`
			if [ "$scrubstats" ]; then
				sudo btrfs scrub status $fspath | tee -a $output
				break
			fi
			sleep 2
		done

		# Grab total bytes scrubbed
		total_data=`sudo btrfs scrub status -R $fspath | \
			grep "bytes_scrubbed" | \
			sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`

		# Scrubbing stats
		iter_time="`sudo btrfs scrub status $fspath | grep "scrub started" | \
			sed 's/.*after //' | sed 's/ sec.*//'`"
		iter_data="`sudo btrfs scrub status $fspath | grep "verified" | \
			sed 's/.*verified: //' | sed 's/.iB.*//'`"

		periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_scrubbed=`echo "scale=2; $bytes_scrubbed*100/$total_data" | bc`
		echo " == When filebench finished, we had scrubbed $p_scrubbed%" \
			"of the data ($bytes_scrubbed out of $total_data bytes) == " | \
			tee -a $output
		;;
	3)	# Backup
		echo "- Running filebench" | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
			elif [ $running == 1 ]; then
				running=2
				# Start collecting idle interval data
				if [[ $do_idle == 1 ]]; then
					collect_idle
				fi

				# Take a snapshot
				btrfs subvolume snapshot -r $fspath $fspath/snap
				sync
				sleep 5

				# Start sending backup to /dev/null
				echo "- Starting the backup process (to /dev/null)..." | tee -a $output
				sudo btrfs send start -B $fspath/snap > /dev/null
				echo -ne "" > iter_work.tmp
				num_work=0
			else
				# Collect data on work being completed
				bytes_sent=""
				while [ "$bytes_sent" == "" ]; do
					bytes_sent=`sudo btrfs send status $fspath/snap | \
					grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`
					sleep 1
				done
				echo -ne "$bytes_sent, " >> iter_work.tmp
				num_work=$((num_work+1))
				if [ $(( $num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> iter_work.tmp
				fi
			fi
		done

		if [[ $do_idle == 1 ]]; then
			# Stop collecting idleness data
			blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
				tr -s ' ' | cut -d' ' -f 2`"
			echo -ne "- Killing blktrace... " | tee -a $output
			while [[ "$blkpid" != "" ]]; do
				echo -ne "x" | tee -a $output
				sudo kill -2 $blkpid
				sleep 1
				blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
					tr -s ' ' | cut -d' ' -f 2`"
			done
			blkpid="`ps aux | grep echo | grep -v grep`"
			while [[ "$blkpid" != "" ]]; do
				echo -ne "o" | tee -a $output
				sleep 2
				blkpid="`ps aux | grep echo | grep -v grep`"
			done
			echo -e " ...Done." | tee -a $output
		fi

		# Grab scrubbing work stats and delete tmp file
		iter_work="`cat iter_work.tmp`"
		rm iter_work.tmp

		# Get last stats
		bytes_sent=`sudo btrfs send status $fspath/snap | \
			grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`
		iter_work="$iter_work$bytes_sent"

		echo -ne "- Waiting for send to terminate... " | tee -a $output
		while [ 1 ]; do
			sendstats=`sudo btrfs send status $fspath/snap | grep "finished"`
			if [ "$sendstats" ]; then
				echo "Done." | tee -a $output
				sudo btrfs send status $fspath/snap | tee -a $output
				break
			fi
			sleep 2
		done

		# Grab total bytes sent
		total_data=`sudo btrfs send status $fspath/snap | \
			grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`

		# Backup stats
		iter_time="`sudo btrfs send status $fspath/snap | \
			grep "finished" | sed 's/.*after //g;s/ sec.//g'`"
		iter_data="`sudo btrfs send status $fspath/snap | grep Sent | \
			sed 's/Sent //g;s/ bytes.*//g' | paste -sd- | bc`"

		periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_sent=`echo "scale=2; $bytes_sent*100/$total_data" | bc`
		echo " == When filebench finished, we had sent $p_sent% of" \
			"the data ($bytes_sent out of $total_data bytes) == " \
			| tee -a $output

		# We're done; delete the snapshot
		btrfs subvolume delete $fspath/snap
		sync
		;;
	5)	# Defrag
		echo "- Running filebench" | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
			elif [ $running == 1 ]; then
				running=2
				# Start collecting idle interval data
				if [[ $do_idle == 1 ]]; then
					collect_idle
				fi

				# Start defrag
				echo "- Starting the defrag process..." | tee -a $output
				sudo btrfs defrag start -B $fspath
				echo -ne "" > iter_work.tmp
				num_work=0
			else
				# Collect data on work being completed
				bytes_defrag=""
				while [ "$bytes_defrag" == "" ]; do
					bytes_defrag=`sudo btrfs defrag status $fspath | \
					grep "bytes," | sed 's/Defragged //g;s/ bytes.*//g'`
					sleep 1
				done
				echo -ne "$bytes_defrag, " >> iter_work.tmp
				num_work=$((num_work+1))
				if [ $(( $num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> iter_work.tmp
				fi
			fi
		done

		if [[ $do_idle == 1 ]]; then
			# Stop collecting idleness data
			blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
				tr -s ' ' | cut -d' ' -f 2`"
			echo -ne "- Killing blktrace... " | tee -a $output
			while [[ "$blkpid" != "" ]]; do
				echo -ne "x" | tee -a $output
				sudo kill -2 $blkpid
				sleep 1
				blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
					tr -s ' ' | cut -d' ' -f 2`"
			done
			blkpid="`ps aux | grep echo | grep -v grep`"
			while [[ "$blkpid" != "" ]]; do
				echo -ne "o" | tee -a $output
				sleep 2
				blkpid="`ps aux | grep echo | grep -v grep`"
			done
			echo -e " ...Done." | tee -a $output
		fi

		# Grab defrag work stats and delete tmp file
		iter_work="`cat iter_work.tmp`"
		rm iter_work.tmp

		# Get last stats
		bytes_defrag=`sudo btrfs defrag status $fspath | \
			grep "bytes," | sed 's/Defragged //g;s/ bytes.*//g'`
		iter_work="$iter_work$bytes_defrag"

		echo -ne "- Waiting for defrag to terminate... " | tee -a $output
		while [ 1 ]; do
			defragstats=`sudo btrfs defrag status $fspath | grep "finished"`
			if [ "$defragstats" ]; then
				echo "Done." | tee -a $output
				sudo btrfs defrag status $fspath | tee -a $output
				break
			fi
			sleep 2
		done

		# Grab total bytes defragged
		total_data=`sudo btrfs defrag status $fspath | \
			grep "bytes," | sed 's/Defragged //g;s/ bytes.*//g'`

		# Defrag stats
		iter_time="`sudo btrfs defrag status $fspath | \
			grep "finished" | sed 's/.*after //g;s/ sec.//g'`"
		iter_data="`sudo btrfs defrag status $fspath | grep Defragged | \
			sed 's/Defragged //g;s/ bytes.*//g' | paste -sd- | bc`"

		periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_defrag=`echo "scale=2; $bytes_defrag*100/$total_data" | bc`
		echo " == When filebench finished, we had defragged $p_defrag% of" \
			"the data ($bytes_defrag out of $total_data bytes) == " \
			| tee -a $output
		;;
	6)	# Scrubbing and backup
		scargs=""

		echo "- Running filebench" | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
			elif [ $running == 1 ]; then
				running=2
				# Start collecting idle interval data
				if [[ $do_idle == 1 ]]; then
					collect_idle
				fi

				# Take a snapshot
				btrfs subvolume snapshot -r $fspath $fspath/snap
				sync

				# Start scrubber
				echo "- Starting the scrubber..." | tee -a $output
				sudo btrfs scrub start $fspath $scargs | tee -a $output &
				echo -ne "" > scrub_iter_work.tmp
				scrub_num_work=0

				# Start sending backup to /dev/null
				echo "- Starting the backup process (to /dev/null)..." | tee -a $output
				sudo btrfs send start -B $fspath/snap > /dev/null
				echo -ne "" > send_iter_work.tmp
				send_num_work=0
			else
				# Collect data on work being completed
				bytes_scrubbed=""
				bytes_sent=""

				while [ "$bytes_scrubbed" == "" ]; do
					bytes_scrubbed=`sudo btrfs scrub status -R $fspath | \
					grep "bytes_scrubbed" | \
					sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`
					sleep 1
				done
				while [ "$bytes_sent" == "" ]; do
					bytes_sent=`sudo btrfs send status $fspath/snap | \
					grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`
					sleep 1
				done

				echo -ne "$bytes_scrubbed, " >> scrub_iter_work.tmp
				scrub_num_work=$((scrub_num_work+1))
				if [ $(( $scrub_num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> scrub_iter_work.tmp
				fi
				echo -ne "$bytes_sent, " >> send_iter_work.tmp
				send_num_work=$((send_num_work+1))
				if [ $(( $send_num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> send_iter_work.tmp
				fi
			fi
		done

		if [[ $do_idle == 1 ]]; then
			# Stop collecting idleness data
			blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
				tr -s ' ' | cut -d' ' -f 2`"
			echo -ne "- Killing blktrace... " | tee -a $output
			while [[ "$blkpid" != "" ]]; do
				echo -ne "x" | tee -a $output
				sudo kill -2 $blkpid
				sleep 1
				blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
					tr -s ' ' | cut -d' ' -f 2`"
			done
			echo -e " ...Done." | tee -a $output
		fi

		# Grab work stats and delete tmp files
		scrub_iter_work="`cat scrub_iter_work.tmp`"
		send_iter_work="`cat send_iter_work.tmp`"
		rm scrub_iter_work.tmp
		rm send_iter_work.tmp

		# Wait for filebench to finish, then poll scrubber for stats, and stop blktrace
		bytes_scrubbed=`sudo btrfs scrub status -R $fspath | \
			grep "bytes_scrubbed" | \
			sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`
		scrub_iter_work="$scrub_iter_work$bytes_scrubbed"

		bytes_sent=`sudo btrfs send status $fspath/snap | \
			grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`
		send_iter_work="$send_iter_work$bytes_sent"

		echo "- Waiting for the scrubber and collecting stats" | tee -a $output
		while [ 1 ]; do
			scrubstats=`sudo btrfs scrub status $fspath | \
				grep "scrub started at .* and finished after .* seconds"`
			if [ "$scrubstats" ]; then
				sudo btrfs scrub status $fspath | tee -a $output
				break
			fi
			sleep 2
		done

		echo -ne "- Waiting for send to terminate... " | tee -a $output
		while [ 1 ]; do
			sendstats=`sudo btrfs send status $fspath/snap | grep "finished"`
			if [ "$sendstats" ]; then
				echo "Done." | tee -a $output
				sudo btrfs send status $fspath/snap | tee -a $output
				break
			fi
			sleep 2
		done

		# Grab total bytes scrubbed
		scrub_total_data=`sudo btrfs scrub status -R $fspath | \
			grep "bytes_scrubbed" | \
			sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`

		# Scrubbing stats
		scrub_iter_time="`sudo btrfs scrub status $fspath | grep "scrub started" | \
			sed 's/.*after //' | sed 's/ sec.*//'`"
		scrub_iter_data="`sudo btrfs scrub status $fspath | grep "verified" | \
			sed 's/.*verified: //' | sed 's/.iB.*//'`"

		#periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		#iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_scrubbed=`echo "scale=2; $bytes_scrubbed*100/$scrub_total_data" | bc`
		echo " == When filebench finished, we had scrubbed $p_scrubbed%" \
			"of the data ($bytes_scrubbed out of $scrub_total_data bytes) == " | \
			tee -a $output

		# Grab total bytes sent
		send_total_data=`sudo btrfs send status $fspath/snap | \
			grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`

		# Backup stats
		send_iter_time="`sudo btrfs send status $fspath/snap | \
			grep "finished" | sed 's/.*after //g;s/ sec.//g'`"
		send_iter_data="`sudo btrfs send status $fspath/snap | grep Sent | \
			sed 's/Sent //g;s/ bytes.*//g' | paste -sd- | bc`"

		#periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		#iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_sent=`echo "scale=2; $bytes_sent*100/$send_total_data" | bc`
		echo " == When filebench finished, we had sent $p_sent% of" \
			"the data ($bytes_sent out of $send_total_data bytes) == " \
			| tee -a $output

		# We're done; delete the snapshot
		btrfs subvolume delete $fspath/snap
		sync
		;;
	7)	# Scrubbing, backup and defrag
		scargs=""

		echo "- Running filebench" | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
			elif [ $running == 1 ]; then
				running=2
				# Start collecting idle interval data
				if [[ $do_idle == 1 ]]; then
					collect_idle
				fi

				# Take a snapshot
				btrfs subvolume snapshot -r $fspath $fspath/snap
				sync

				# Start scrubber
				echo "- Starting the scrubber..." | tee -a $output
				sudo btrfs scrub start $fspath $scargs | tee -a $output &
				echo -ne "" > scrub_iter_work.tmp
				scrub_num_work=0

				# Start sending backup to /dev/null
				echo "- Starting the backup process (to /dev/null)..." | tee -a $output
				sudo btrfs send start -B $fspath/snap > /dev/null
				echo -ne "" > send_iter_work.tmp
				send_num_work=0

				# Start defrag
				echo "- Starting the defrag process..." | tee -a $output
				sudo btrfs defrag start -B $fspath
				echo -ne "" > defrag_iter_work.tmp
				defrag_num_work=0
			else
				# Collect data on work being completed
				bytes_scrubbed=""
				bytes_sent=""
				bytes_defrag=""

				while [ "$bytes_scrubbed" == "" ]; do
					bytes_scrubbed=`sudo btrfs scrub status -R $fspath | \
					grep "bytes_scrubbed" | \
					sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`
					sleep 1
				done
				while [ "$bytes_sent" == "" ]; do
					bytes_sent=`sudo btrfs send status $fspath/snap | \
					grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`
					sleep 1
				done
				while [ "$bytes_defrag" == "" ]; do
					bytes_defrag=`sudo btrfs defrag status $fspath | \
					grep "bytes," | sed 's/Defragged //g;s/ bytes.*//g'`
					sleep 1
				done

				echo -ne "$bytes_scrubbed, " >> scrub_iter_work.tmp
				scrub_num_work=$((scrub_num_work+1))
				if [ $(( $scrub_num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> scrub_iter_work.tmp
				fi
				echo -ne "$bytes_sent, " >> send_iter_work.tmp
				send_num_work=$((send_num_work+1))
				if [ $(( $send_num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> send_iter_work.tmp
				fi
				echo -ne "$bytes_defrag, " >> defrag_iter_work.tmp
				defrag_num_work=$((defrag_num_work+1))
				if [ $(( $defrag_num_work % 5 )) -eq 0 ]; then
					echo -ne "\n\t" >> defrag_iter_work.tmp
				fi
			fi
		done

		if [[ $do_idle == 1 ]]; then
			# Stop collecting idleness data
			blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
				tr -s ' ' | cut -d' ' -f 2`"
			echo -ne "- Killing blktrace... " | tee -a $output
			while [[ "$blkpid" != "" ]]; do
				echo -ne "x" | tee -a $output
				sudo kill -2 $blkpid
				sleep 1
				blkpid="`ps aux | grep blktrace | grep -v "grep\|sudo" | \
					tr -s ' ' | cut -d' ' -f 2`"
			done
			echo -e " ...Done." | tee -a $output
		fi

		# Grab work stats and delete tmp files
		scrub_iter_work="`cat scrub_iter_work.tmp`"
		send_iter_work="`cat send_iter_work.tmp`"
		defrag_iter_work="`cat defrag_iter_work.tmp`"
		rm scrub_iter_work.tmp send_iter_work.tmp defrag_iter_work.tmp

		# Wait for filebench to finish, then poll tasks for stats
		bytes_scrubbed=`sudo btrfs scrub status -R $fspath | \
			grep "bytes_scrubbed" | \
			sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`
		scrub_iter_work="$scrub_iter_work$bytes_scrubbed"

		bytes_sent=`sudo btrfs send status $fspath/snap | \
			grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`
		send_iter_work="$send_iter_work$bytes_sent"

		bytes_defrag=`sudo btrfs defrag status $fspath | \
			grep "bytes," | sed 's/Defragged //g;s/ bytes.*//g'`
		defrag_iter_work="$defrag_iter_work$bytes_defrag"

		echo "- Waiting for the scrubber and collecting stats" | tee -a $output
		while [ 1 ]; do
			scrubstats=`sudo btrfs scrub status $fspath | \
				grep "scrub started at .* and finished after .* seconds"`
			if [ "$scrubstats" ]; then
				sudo btrfs scrub status $fspath | tee -a $output
				break
			fi
			sleep 2
		done

		echo -ne "- Waiting for send to terminate... " | tee -a $output
		while [ 1 ]; do
			sendstats=`sudo btrfs send status $fspath/snap | grep "finished"`
			if [ "$sendstats" ]; then
				echo "Done." | tee -a $output
				sudo btrfs send status $fspath/snap | tee -a $output
				break
			fi
			sleep 2
		done

		echo -ne "- Waiting for defrag to terminate... " | tee -a $output
		while [ 1 ]; do
			defragstats=`sudo btrfs defrag status $fspath | grep "finished"`
			if [ "$defragstats" ]; then
				echo "Done." | tee -a $output
				sudo btrfs defrag status $fspath | tee -a $output
				break
			fi
			sleep 2
		done

		# Grab total bytes scrubbed
		scrub_total_data=`sudo btrfs scrub status -R $fspath | \
			grep "bytes_scrubbed" | \
			sed 's/.*_bytes_scrubbed: //g' | paste -sd+ | bc`

		# Scrubbing stats
		scrub_iter_time="`sudo btrfs scrub status $fspath | grep "scrub started" | \
			sed 's/.*after //' | sed 's/ sec.*//'`"
		scrub_iter_data="`sudo btrfs scrub status $fspath | grep "verified" | \
			sed 's/.*verified: //' | sed 's/.iB.*//'`"

		#periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		#iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_scrubbed=`echo "scale=2; $bytes_scrubbed*100/$scrub_total_data" | bc`
		echo " == When filebench finished, we had scrubbed $p_scrubbed%" \
			"of the data ($bytes_scrubbed out of $scrub_total_data bytes) == " | \
			tee -a $output

		# Grab total bytes sent
		send_total_data=`sudo btrfs send status $fspath/snap | \
			grep "bytes," | sed 's/Sent //g;s/ bytes.*//g'`

		# Backup stats
		send_iter_time="`sudo btrfs send status $fspath/snap | \
			grep "finished" | sed 's/.*after //g;s/ sec.//g'`"
		send_iter_data="`sudo btrfs send status $fspath/snap | grep Sent | \
			sed 's/Sent //g;s/ bytes.*//g' | paste -sd- | bc`"

		#periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		#iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_sent=`echo "scale=2; $bytes_sent*100/$send_total_data" | bc`
		echo " == When filebench finished, we had sent $p_sent% of" \
			"the data ($bytes_sent out of $send_total_data bytes) == " \
			| tee -a $output

		# Grab total bytes defragged
		defrag_total_data=`sudo btrfs defrag status $fspath | \
			grep "bytes," | sed 's/Defragged //g;s/ bytes.*//g'`

		# Defrag stats
		defrag_iter_time="`sudo btrfs defrag status $fspath | \
			grep "finished" | sed 's/.*after //g;s/ sec.//g'`"
		defrag_iter_data="`sudo btrfs defrag status $fspath | grep Defragged | \
			sed 's/Defragged //g;s/ bytes.*//g' | paste -sd- | bc`"

		#periods="`printf "%.0f" $(echo "$iter_time/$profgran" | bc)`"
		#iter_slat="mean(${rpfx}_wdat${expiter}[1:$periods])"

		p_defrag=`echo "scale=2; $bytes_defrag*100/$defrag_total_data" | bc`
		echo " == When filebench finished, we had defragged $p_defrag% of" \
			"the data ($bytes_defrag out of $defrag_total_data bytes) == " \
			| tee -a $output

		# We're done; delete the snapshot
		btrfs subvolume delete $fspath/snap
		sync
		;;
	8)	# rsync
		if [[ $dueton == 1 ]]; then
			rsyncarg="--out-of-order"
		else
			rsyncarg=""
		fi

		echo "- Running filebench" | tee -a $output
		sudo filebench -f $fbprof 2>&1 | \
		tee -a $output $tmpout /dev/tty | \
		grep --line-buffered "Running..." | \
		while read; do
			if [ $running == 0 ]; then
				running=1
				sync
			elif [ $running == 1 ]; then
				running=2

				# Start rsync between $fspath and $sinkpath
				echo "- Starting rsync process ($fspath to $sinkpath)..." | tee -a $output
				echo -ne "" > rsync_log.tmp
				sudo $rsyncpath -rvu --log-file=rsync_log.tmp --partial $rsyncarg \
					--info=stats2,duet1 $fspath/ $sinkpath/ & #>/dev/null
			else
				# We're not doing periodic data collection for rsync
				# but we need to check if rsync is done
				fbpid="`ps aux | grep "sudo filebench" | grep -v grep | awk '{print $2}'`"
				rspid="`ps aux | grep "rsync -rvu --log" | grep sudo | awk '{print $2}'`"
				if [[ -z $rspid ]]; then
					# Kill filebench
					kill -INT $fbpid
				fi
			fi
		done

		# If rsync is still going, we have a problem
		rspid="`ps aux | grep "rsync -rvu --log" | grep -v grep | awk '{print $2}'`"
		if [[ -n $rspid ]]; then
			echo "ERROR: Filebench finished before rsync. I quit!" | tee -a $output
			exit 1
		fi

		# Grab rsync stats and delete tmp file
		iter_work="`tail -18 rsync_log.tmp`"
		#rm rsync_log.tmp

		# Rsync stats: I/O performed (bytes), and runtime (secs)
		iter_time="`echo -e "$iter_work" | grep "Total runtime" | \
			sed 's/.*: //g;s/ seconds//g;s/,//g'`"
		iter_data="`echo -e "$iter_work" | grep "Total bytes sent" | \
			sed 's/.*sent.*: //g;s/,//g' | paste -sd- | bc`"
		;;
	esac

	# Workload stats
	iter_lats="`grep "IO Summary" $tmpout | sed 's/.*op,\s*//' | sed 's/ms.*//' | \
		awk 'BEGIN { pos=0; }
		{ if (pos > 1 && (pos-1)%10 == 0) printf(",\n\t");
		  else if (pos > 1) printf(", ");
		  if(pos) printf("%s",$0);
		  pos++; }'`"
	iter_xpts="`grep "IO Summary" $tmpout | sed 's/.*r\/w),\s*//' | sed 's/mb.*//' | \
		awk 'BEGIN { pos=0; }
		{ if (pos > 1 && (pos-1)%10 == 0) printf(",\n\t");
		  else if (pos > 1) printf(", ");
		  if(pos) printf("%s",$0);
		  pos++; }'`"

	if [[ $do_idle == 1 ]]; then
		# Calculate idle intervals
		sync
		iter_idle="`cat iter_idle.tmp | sort -n | \
		awk -F'\t' 'BEGIN { lend=0; pos=0; }
		{
			if (pos == 0) lend = $1
			pos++; tstart = $1; tend = $2; idle = tstart - lend;
			if (idle > 0) printf("%f\n", idle)
			if ((tstart < lend && lend < tend) || (tstart >= lend))
				lend = tend
		}'`"
		mv iter_idle.tmp $outdir${rpfx}_ir${expiter}.dat
		echo -e "${iter_idle}" >> $outdir${rpfx}_idles${expiter}.dat
	fi

	echo -ne "Exporting collected stats... " | tee -a $output
	echo -e "${rpfx}_wdat${expiter} <- c($iter_lats);
${rpfx}_xdat${expiter} <- c($iter_xpts);" >> $rfname

	case $bgtask in
	0|4)	# No maintenance task
		echo "${rpfx}_kdat${expiter} <- c();" >> $rfname
		;;
	6)	# Two maintenance tasks: scrubbing, backup
		echo "${rpfx}_k1dat${expiter} <- c($scrub_iter_work) /" \
			"$scrub_total_data;" >> $rfname
		echo "${rpfx}_k2dat${expiter} <- c($send_iter_work) /" \
			"$send_total_data;" >> $rfname
		;;
	7)	# Three maintenance tasks: scrubbing, backup, defrag
		echo "${rpfx}_k1dat${expiter} <- c($scrub_iter_work) /" \
			"$scrub_total_data;" >> $rfname
		echo "${rpfx}_k2dat${expiter} <- c($send_iter_work) /" \
			"$send_total_data;" >> $rfname
		echo "${rpfx}_k3dat${expiter} <- c($defrag_iter_work) /" \
			"$defrag_total_data;" >> $rfname
		;;
	8)	# One maintenance task (rsync)
		echo "${rpfx}_kdat${expiter} <- \"$iter_work\";" >> $rfname
		;;
	*)	# One maintenance task (not rsync)
		echo "${rpfx}_kdat${expiter} <- c($iter_work) /" \
			"$total_data;" >> $rfname
			#"(seq($profgran,$explen,length.out=$explen/$profgran)" \
			#"* $total_data / $explen);" >> $rfname
		;;
	esac
	echo -ne "Done.\n" | tee -a $output

	sep=""
	if [ $expiter -gt 1 ]; then
		sep=", "
	fi

	wlatx="$wlatx${sep}mean(${rpfx}_wdat${expiter})"
	xputx="$xputx${sep}mean(${rpfx}_xdat${expiter})"
	case $bgtask in
	6)	# Two maintenance tasks: scrubbing, backup
		datax1="$datax1${sep}$scrub_iter_data"
		datax2="$datax2${sep}$send_iter_data"
		timex1="$timex1${sep}$scrub_iter_time"
		timex2="$timex2${sep}$send_iter_time"
		workx1="$workx1${sep}${rpfx}_k1dat${expiter}"
		workx2="$workx2${sep}${rpfx}_k2dat${expiter}"
		;;
	7)	# Three maintenance tasks: scrubbing, backup, defrag
		datax1="$datax1${sep}$scrub_iter_data"
		datax2="$datax2${sep}$send_iter_data"
		datax3="$datax3${sep}$defrag_iter_data"
		timex1="$timex1${sep}$scrub_iter_time"
		timex2="$timex2${sep}$send_iter_time"
		timex3="$timex3${sep}$defrag_iter_time"
		workx1="$workx1${sep}${rpfx}_k1dat${expiter}"
		workx2="$workx2${sep}${rpfx}_k2dat${expiter}"
		workx3="$workx3${sep}${rpfx}_k3dat${expiter}"
		;;
	8)	# One maintenance task (rsync)
		datax="$datax${sep}$iter_data"
		timex="$timex${sep}$iter_time"
		workx="$workx${sep}${rpfx}_kdat${expiter}"
		;;
	*)	# No/One maintenance task (not rsync)
		slatx="$slatx${sep}$iter_slat"
		datax="$datax${sep}$iter_data"
		timex="$timex${sep}$iter_time"
		workx="$workx${sep}${rpfx}_kdat${expiter}"
		;;
	esac
}

# This function does all the work of running one experiment.
# @1: banner announcing the experiment
# @2: experiment code
# @3: maintenance task
#       0 = no maintenance
#       1 = scrubbing
#	2 = throttled scrubbing
#	3 = backup
#	4 = no maintenance (uses restored fs)
#	5 = defrag
#	6 = scrubbing and backup
#	7 = scrubbing, backup and defrag
#	8 = rsync
# @4: duet framework on
# @5: number of repeats
runexp () {
	banner="$1"
	output="${logpfx}_$2"
	rpfx="${varpfx}_$2"
	bgtask=$3
	dueton=$4
	numrep=$5

	case $bgtask in
	[0-5]|8)
		wlatx=""; slatx=""; timex=""; datax=""; xputx=""; workx=""
		;;
	6)
		wlatx=""; xputx=""
		timex1=""; datax1=""; workx1=""
		timex2=""; datax2=""; workx2=""
		;;
	7)
		wlatx=""; xputx=""
		timex1=""; datax1=""; workx1=""
		timex2=""; datax2=""; workx2=""
		timex3=""; datax3=""; workx3=""
		;;
	esac

	if [[ $bgtask -ne 4 && $bgtask -ne 5 && $bgtask -ne 7 ]]; then
		defrag_reuse=0

		echo -ne "- Removing all files from '$fspath'..." | tee -a $output
		if [[ $fspath == /media/* ]]; then
			rm -Rf $fspath/*
		else
			echo "Oops. $fspath does not fall under /media. Rethink this decision."
			exit 1
		fi
		echo " Done." | tee -a $output

		if [ -n "${sinkpath:+1}" ]; then
			echo -ne "- Removing all files from '$sinkpath'..." | tee -a $output
			if [[ $sinkpath == /media/* ]]; then
				rm -Rf $sinkpath/*
			else
				echo "Oops. $sinkpath does not fall under /media. Rethink this decision."
				exit 1
			fi
			echo " Done." | tee -a $output
		fi

		echo -ne "- Compiling filebench profile..." | tee -a $output
		compileprof $wkld $util $hotper $real_accesses
		echo " Done." | tee -a $output

		if [[ $hotper -ge 0 && $hotper -lt 100 ]]; then
			echo -ne "- Placing cold data ($((100-hotper))%)... " >> $output
			echo "- Placing cold data ($((100-hotper))%):"
			sudo ./bar.sh $coldset | tar xzf - -C $fspath
			echo -ne "Done.\n" >> $output
		fi
	else
		defrag_reuse=1
		xzpref="frag_fsimgs/${wkld}_h${hotper}_f5.part"

		echo -ne "- Restoring filesystem from '$xzpref'\n  " | tee -a $output
		./prepfs.sh restore $fspart $xzpref

		echo "- Mounting btrfs filesystem on $fspath..." | tee -a $output
		mount $fspart $fspath

		echo -ne "- Compiling filebench profile..." | tee -a $output
		compileprof $wkld $util $hotper $real_accesses
		echo " Done." | tee -a $output
	fi

	for expiter in $(seq 1 $numrep); do
		echo "$banner ($expiter of $numrep)" | tee -a $output $rfname
		runiter
	done

	# *_l = vectors of average latency during (entire run, scrubbing)
	# *_x = average throughput during the run
	# *_tX = maintenance time for maintenance task X
	# *_dX = total data transferred for maintenance task X
	# *_wX = vectors of (average, overall) %age of work completed for task X
	case $bgtask in
	[0-5])
		echo -e "${rpfx}_l <- rbind(c($wlatx),\n\tc($slatx));
${rpfx}_t <- c($timex);
${rpfx}_d <- c($datax);
${rpfx}_x <- c($xputx);
${rpfx}_w <- list($workx);\n" >> $rfname
		;;
	6)
		echo -e "${rpfx}_l <- c($wlatx);
${rpfx}_x <- c($xputx);
${rpfx}_t1 <- c($timex1);
${rpfx}_t2 <- c($timex2);
${rpfx}_d1 <- c($datax1);
${rpfx}_d2 <- c($datax2);
${rpfx}_w1 <- list($workx1);
${rpfx}_w2 <- list($workx2);\n" >> $rfname
		;;
	7)
		echo -e "${rpfx}_l <- c($wlatx);
${rpfx}_x <- c($xputx);
${rpfx}_t1 <- c($timex1);
${rpfx}_t2 <- c($timex2);
${rpfx}_t3 <- c($timex3);
${rpfx}_d1 <- c($datax1);
${rpfx}_d2 <- c($datax2);
${rpfx}_d3 <- c($datax3);
${rpfx}_w1 <- list($workx1);
${rpfx}_w2 <- list($workx2);
${rpfx}_w3 <- list($workx3);\n" >> $rfname
		;;
	esac

	# Keep syslog output in $output.log
	echo "- Appending syslog to $output.syslog" | tee -a $output
	cp /var/log/syslog $output.syslog
}

runconfigs () {
	echo -e "\n================= Starting experiments ================="
	# Legend: F (filebench), S (scrubber), B (backup), D (defrag),
	#         t (throttling), d (duet enabled)

	# Baseline
	runexp "# Experiment F"		"xx" 0 0 $numreps

	# Scrubbing baseline
#	runexp "# Experiment F+S"	"sx" 1 0 $numreps
	# Synergistic scrubbing
#	runexp "# Experiment F+S[d]"	"sd" 1 1 $numreps
	# Throttled synergistic scrubbing
#	runexp "# Experiment F+S[t]"	"st" 2 1 $numreps

	# Backup baseline
#	runexp "# Experiment F+B"	"bx" 3 0 $numreps
	# Synergistic backup
#	runexp "# Experiment F+B[d]"	"bd" 3 1 $numreps

	# Baseline on restored fs (for defrag)
#	runexp "# Experiment F*"	"Rx" 4 0 $numreps
	# Defrag baseline
#	runexp "# Experiment F+D"	"dx" 5 0 $numreps
	# Synergistic defrag
#	runexp "# Experiment F+D[d]"	"dd" 5 1 $numreps

	# Scrubbing and backup baseline
#	runexp "# Experiment F+SB"	"sbx" 6 0 $numreps
	# Synergistic scrubbing and backup
#	runexp "# Experiment F+SB[d]"	"sbd" 6 1 $numreps

	# Scrubbing, backup and defrag baseline
#	runexp "# Experiment F+SBD"	"sbdx" 7 0 $numreps
	# Synergistic scrubbing, backup and defrag baseline
#	runexp "# Experiment F+SBD[d]"	"sbdd" 7 1 $numreps

	# Synergistic rsync
#	runexp "# Experiment F+R[d]"	"rd" 8 1 $numreps
	# Rsync baseline
#	runexp "# Experiment F+R"	"rx" 8 0 $numreps

	echo -e "\n===== Evaluation completed. See ${outdir}* for output ====="
}

echo -e "================== Setting up environment =================="

# Setup some workload- and machine-specific variables
case $workload in
"webserver")	wkld="wsv"	;;
"varmail")	wkld="var"	;;
"webproxy")	wkld="wpx"	;;
"fileserver")	wkld="fsv"	;;
*)
	echo "I don't recognize workload '$workload'. Goodbye."
	exit 1
	;;
esac

for hotper in ${hotcold[@]}; do
	case $hotper in
	25)	# Run with 25% hot data and uniform accesses
		coldset="./cold.big.tgz"
		real_accesses=0
		;;
	50)	# Run with 50% hot data and uniform accesses
		coldset="./cold.med.tgz"
		real_accesses=0
		;;
	75)	# Run with 75% hot data and uniform accesses
		coldset="./cold.sml.tgz"
		real_accesses=0
		;;
	100)	# Run with 100% hot data and uniform accesses
		coldset=""	# we'll never use this
		real_accesses=0
		;;
	-100)	# Run with 100% hot data and realistic accesses
		coldset=""	# we'll never use this
		hotper=100
		real_accesses=1
		;;
	1025)	# Run with 25% hot data and uniform accesses
		coldset="./cold.test.tgz"
		hotper=25
		real_accesses=0
		;;
	*)
		echo "I don't support this hot/cold ratio ($hotper). Goodbye."
		exit 1
		;;
	esac

	# Create filesystem and mount it
	sudo umount $fspart
	sudo logrotate -f /etc/logrotate.conf
	echo "- Creating btrfs filesystem on $fspart..." | tee -a $output
	sudo btrfs dev scan
	sudo mkfs.btrfs -n 4096 -f $fspart
	echo "- Mounting btrfs filesystem on $fspath..." | tee -a $output
	sudo mount $fspart $fspath

	# If rsync's sink mount point is defined, create a filesystem and mount it there
	if [ -n "${sinkpath:+1}" ]; then
		sudo umount $sinkpart
		echo "- Creating ext4 sink on $sinkpath..." | tee -a $output
		sudo btrfs dev scan
		sudo mkfs.btrfs -n 4096 -f $sinkpart
		echo "- Mounting ext4 sink on $sinkpath..." | tee -a $output
		sudo mount $sinkpart $sinkpath
	fi

	# Do what filebench wants
	sudo sh -c "echo 0 > /proc/sys/kernel/randomize_va_space"

	# Run experiments for different utilizations
	datstr="$(date +%y%m%d-%H%M)"		# Date string
	exppfx="${wkld}_h${hotper}"		# Experiment prefix
	rfname="$outdir${exppfx}_${datstr}.R"	# R file name
	echo -e "library(bigmemory)" > $rfname

	for util in ${wkldutil[@]}; do
		varpfx="${exppfx}_u${util}"		# R variable prefix
		logpfx="$outdir${varpfx}_${datstr}"	# Log file prefix

		runconfigs
	done

	# If rsync's sink mount is defined, unmount
	if [ -n "${sinkpath:+1}" ]; then
		echo "- Unmounting btrfs sink on $sinkpath..." | tee -a $output
		sudo umount $sinkpath
	fi

	# Cleanup temporary files and unmount fs
	rm $tmpout $fbprof fbperson.f
	echo "- Unmounting btrfs filesystem on $fspath..." | tee -a $output
	sudo umount $fspath
done
