human_arch	= 64 bit x86
build_arch	= x86_64
header_arch	= $(build_arch)
defconfig	= defconfig
flavours	= generic
build_image	= bzImage
kernel_file	= arch/$(build_arch)/boot/bzImage
install_file	= vmlinuz
loader		= grub
no_dumpfile	= true
uefi_signed     = true

skipabi         = true
skipmodule      = true
do_libc_dev_package=false
do_source_package=false
do_doc_package  = false

do_tools        = true
do_tools_cpupower = true
do_tools_perf   = true
do_tools_x86	= true
do_cloud_tools	= false
