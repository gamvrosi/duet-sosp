#!/bin/bash
# Install the kernel
sudo dpkg -i linux-headers-3.13.6+duet_3.13.6+duet-10.00.Custom_amd64.deb
sudo dpkg -i linux-image-3.13.6+duet_3.13.6+duet-10.00.Custom_amd64.deb

# Install the btrfs tools (in /usr/local/bin)
cd btrfs-progs-3.12+duet
sudo make install

# Install the duet tools (in /usr/local/bin)
cd ../duet-progs
sudo make install
