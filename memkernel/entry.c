// MIT License
/*
 * Memory Operation driver for Linux Android
 *
 * Original author:  Jiang-Night
 * Current maintainer: Poko
 *
*/

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "comm.h"
#include "memory.h"

#define DEVICE_NAME "oplus_bsp_state"

static DEFINE_MUTEX(driver_mutex);


static int dispatch_open(struct inode *node, struct file *file) {
	if (!mutex_trylock(&driver_mutex))
		return -EBUSY;
	return 0;
}

static int dispatch_close(struct inode *node, struct file *file) {
	mutex_unlock(&driver_mutex);
	return 0;
}

static long dispatch_ioctl(struct file *const file, unsigned int const cmd, unsigned long const arg)
{
	struct CopyMemory cm;

	switch (cmd)
	{
	case OP_READ_MEM:
	{
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0) {
			return -1;
		}
		return readwrite_process_memory(cm.pid, cm.addr, cm.buffer, cm.size, false);
	}
	case OP_WRITE_MEM:
	{
		if (copy_from_user(&cm, (void __user *)arg, sizeof(cm)) != 0) {
			return -1;
		}
		return readwrite_process_memory(cm.pid, cm.addr, cm.buffer, cm.size, true);
	}
	default:
		break;
	}
	return 0;
}

static struct file_operations dispatch_functions = {
	.owner = THIS_MODULE,
	.open = dispatch_open,
	.release = dispatch_close,
	.unlocked_ioctl = dispatch_ioctl,
};

static struct miscdevice misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = DEVICE_NAME,
	.fops = &dispatch_functions,
};

int __init oplus_bsp_dfr_state_init(void)
{
	int ret;
	printk("[+] oplus_bsp_dfr_state_init");
	ret = misc_register(&misc);
	return ret;
}

void __exit oplus_bsp_dfr_state_exit(void)
{
	printk("[+] oplus_bsp_dfr_state_exit");
	misc_deregister(&misc);
}

module_init(oplus_bsp_dfr_state_init);
module_exit(oplus_bsp_dfr_state_exit);

MODULE_DESCRIPTION("Linux Kernel.");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Linux");
