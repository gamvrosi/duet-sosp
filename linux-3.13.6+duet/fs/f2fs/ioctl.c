
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/fs.h>

#define DEVICE_NAME "f2fschrdev"


struct file_operations fops = {
	.owner = THIS_MODULE
};

dev_t f2fs_dev;
struct cdev *f2fs_cdev = NULL;
struct class *f2fs_class = NULL;

static int __init f2fs_create_chrdev(void)
{
	int ret;
	struct device *devcp = NULL;
	
	ret = alloc_chrdev_region(&f2fs_dev, 0, 1, DEVICE_NAME);
	if (ret < 0)
	{
		printk(KERN_WARNING "f2fs character device region allocation failed\n");
		return ret;
	}

	f2fs_class = class_create(THIS_MODULE, DEVICE_NAME);
	if(!f2fs_class)
	{
		printk(KERN_WARNING "f2fs character device failed to create device class\n");
		goto bail_reg;
	}

	devcp = device_create(f2fs_class, NULL, f2fs_dev, NULL, DEVICE_NAME);
	if(!devcp)
	{
		printk(KERN_WARNING "f2fs character device failed to create device node\n");
		goto bail_cl;
	}

	f2fs_cdev = cdev_alloc();
	if(!f2fs_cdev)
	{
		printk(KERN_WARNING "f2fs character device failed to allocate char device\n");
		goto bail_dev;
	}

	cdev_init(f2fs_cdev, &fops);
	f2fs_cdev->owner = THIS_MODULE;
	ret = cdev_add(f2fs_cdev, f2fs_dev, 1);
	if(ret) {
		printk(KERN_WARNING "f2fs character device failed to add character device\n");
		goto bail_alloc;
	}

	return 0;
bail_alloc:
	kfree(f2fs_cdev);
bail_dev:
	device_destroy(f2fs_class, f2fs_dev);
bail_cl:
	class_destroy(f2fs_class);
bail_reg:
	unregister_chrdev_region(f2fs_dev, 1);
	return -1;
}

static void __exit f2fs_destroy_chrdev(void)
{
	cdev_del(f2fs_cdev);
	kfree(f2fs_cdev);
	device_destroy(f2fs_class, f2fs_dev);
	class_destroy(f2fs_class);

	unregister_chrdev_region(f2fs_dev, 1);
}

MODULE_AUTHOR("Suvanjan Mukherjee, George Amvrosiadis");
MODULE_DESCRIPTION("Random device");
MODULE_LICENSE("GPL");

module_init(f2fs_create_chrdev);
module_exit(f2fs_destroy_chrdev);
