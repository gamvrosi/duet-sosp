#!/bin/bash -x
# Inode reuse test

gen_file() {
	old_ino="$1"
	sizek="$2"
	del="$3"

	# Generate a 10K file
	file=$(mktemp ./tmp.XXXXXX)
	dd if=/dev/urandom of="$file" bs=1K count=$sizek

	# Grab file's ino and compare with given ino
	file_ino=$(stat -c "%i" "$file")

	same=0
	if [[ $file_ino == $old_ino ]]; then
		same=1
	fi

	echo "Generated file $file (ino: $file_ino). Looking for $old_ino"

	# Delete file
	if [[ $del == 1 ]]; then
		rm $file
	fi
}

# Register with Duet
sudo duet status start || die
tid=`sudo duet task register -n "inode_reuse_test" -b 1 -m 1000f -p "$PWD"`
if [[ $? != 0 ]]; then
	die
fi

tid=`echo $tid | sed 's/.*ID //g;s/)//'`

# Generate 10K files until the inode number is reused
gen_file 0 20 1
while [[ $same == 0 ]]; do
	gen_file $file_ino 8 0
done

# Fetch Duet events
sudo duet task fetch -i $tid

# Deregister with Duet
sudo duet task deregister -i $tid || die

