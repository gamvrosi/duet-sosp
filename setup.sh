#!/bin/bash

# Find location of this setup.sh
STARTDIR="$(pwd)"
cd "$(dirname "$0")"
BASEDIR="$(pwd)"
cd "${STARTDIR}"

usage() {
	echo "Usage: $0 [OPTION]...

	-d	Install dependencies
	-c	Compile Duet version of the kernel, duet-progs, f2fs-tools and rsync
	-i	Install compiled packages and programs
	-u	Uninstall all but latest Duet kernel, and delete their deb packages
"
}

die() {
	echo "Aborting setup..."
	exit 1
}

# Check that we're running on Ubuntu. We're picky like that.
if [[ ! `grep "DISTRIB_ID=Ubuntu" /etc/lsb-release` ]]; then
	echo "Duet is currently only supported on Ubuntu. Sorry." >&2
	exit 1
fi

KERNEL_VERSION_APPEND="+duet-$(git rev-parse --short HEAD)"

while getopts ":dcigu" opt; do
	case $opt in
	d)
		echo "Installing dependencies..."

		# Install kernel dependencies
		sudo apt-get install build-essential kernel-package \
			libncurses5-dev

		# Install tool dependencies
		sudo apt-get install uuid-dev libblkid-dev libz-dev liblzo2-dev \
			libattr1-dev libacl1-dev e2fslibs-dev autoconf yodl

		echo "Done processing dependencies. Exiting."
		exit 0
		;;
	[cg])
		KDBG=`test $opt == 'g' && echo kernel_debug`

		# Prep the environment for future recompiles
		export CLEAN_SOURCE=no
		export CONCURRENCY_LEVEL="$(expr `nproc` + 1)"
		echo CLEAN_SOURCE=$CLEAN_SOURCE
		echo CONCURRENCY_LEVEL=$CONCURRENCY_LEVEL

		# (re)compile the kernel
		cd "${BASEDIR}/linux-3.13.6+duet"
		time fakeroot make-kpkg --initrd --append-to-version="${KERNEL_VERSION_APPEND}" \
			kernel_image kernel_headers $KDBG || die

		# ...and (re)compile the btrfs tools
		cd "${BASEDIR}/btrfs-progs-3.12+duet"
		#make clean
		make || die

		# ...and (re)compile the duet tools
		cd "${BASEDIR}/duet-progs"
		#make clean
		make || die

		# ...and (re)compile the f2fs tools
		cd "${BASEDIR}/f2fs-tools"
		#make clean
		make || die
 
		# ...and (re)compile rsync
		cd "${BASEDIR}/rsync-3.1.1+duet"
		#make clean
		#make reconfigure
		make || die

		# ...and (re)compile the dummy task
		cd "${BASEDIR}/dummy_task"
		make || die

		exit 0
		;;
	i)
		# Install the kernel
		KPKG_SUFFIX="3.13.6${KERNEL_VERSION_APPEND}_3.13.6${KERNEL_VERSION_APPEND}-10.00.Custom_amd64.deb"
		sudo dpkg -i "${BASEDIR}/linux-headers-${KPKG_SUFFIX}" || die
		sudo dpkg -i "${BASEDIR}/linux-image-${KPKG_SUFFIX}" || die

		# Install the btrfs tools (in /usr/local/bin)
		cd "${BASEDIR}/btrfs-progs-3.12+duet"
		sudo make install || die

		# Install the duet tools (in /usr/local/bin)
		cd "${BASEDIR}/duet-progs"
		sudo make install || die

		# Install the f2fs tools (in /usr/local/bin)
		cd "${BASEDIR}/f2fs-tools"
		sudo make install || die

		# Do not install rsync; it will replace the stock rsync

		cd ..
		exit 0
		;;
	u)
		# Get installed packages for all but latest Duet kernel
		DPKGS="`dpkg --get-selections | grep -E 'linux-.*+duet' | \
			cut -f1 | grep -v $KERNEL_VERSION_APPEND | tr '\n' ' '`"
		sudo dpkg -P $DPKGS || die

		# Get .deb packages in BASEDIR of all but latest Duet kernel
		cd "${BASEDIR}"
		ls | grep -E 'linux-.*.deb' | grep -v $KERNEL_VERSION_APPEND | \
			tr '\n' ' ' | xargs rm -v || die

		exit 0
		;;
	\?)
		echo "Invalid option: -$OPTARG" >&2
        usage
        exit 1
		;;
	esac
done

usage
exit 1
