#!/bin/bash

# Find location of this setup.sh
STARTDIR="$(pwd)"
cd "$(dirname "$0")"
BASEDIR="$(pwd)"
cd "${STARTDIR}"

usage() {
    echo "Usage: $0 [OPTION]..."
    cat <<EOF

  -d   Install dependencies
  -c   Compile Duet version of the kernel, duet-progs, f2fs-tools and rsync
  -i   Install compiled packages and programs
EOF
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

while getopts ":dci" opt; do
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
	c)
		# Prep the environment for future recompiles
		export CLEAN_SOURCE=no
		export CONCURRENCY_LEVEL="$(expr `nproc` + 1)"
		echo CLEAN_SOURCE=$CLEAN_SOURCE
		echo CONCURRENCY_LEVEL=$CONCURRENCY_LEVEL

		# (re)compile the kernel
		cd "${BASEDIR}/linux-3.13.6+duet"
		time fakeroot make-kpkg --initrd --append-to-version=+duet \
			kernel_image kernel_headers || die

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

		exit 0
		;;
	i)
		# Install the kernel
		sudo dpkg -i "${BASEDIR}/linux-headers-3.13.6+duet_3.13.6+duet-10.00.Custom_amd64.deb"
		sudo dpkg -i "${BASEDIR}/linux-image-3.13.6+duet_3.13.6+duet-10.00.Custom_amd64.deb"

		# Install the btrfs tools (in /usr/local/bin)
		cd "${BASEDIR}/btrfs-progs-3.12+duet"
		sudo make install

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
	\?)
		echo "Invalid option: -$OPTARG" >&2
        usage
        exit 1
		;;
	esac
done

usage
exit 1
