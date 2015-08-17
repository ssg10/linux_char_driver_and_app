/*
 *
 *  Linux kernel character driver.
 *
 *  Intended as a reusable sample code of a complete char driver framework.
 *  The driver lets user-space program to read/write/mmap a region in 
 *  the physical memory in byte-size operations. 
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Created by: Sani Gosali
 */

#include "hv_cdev.h"
#include "hv_cmd.h"
#include <linux/mutex.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/intel-iommu.h>
#include "hv_cdev_uapi.h"

#define HV_CDEV_N_MINORS		1
#define HV_CDEV_FIRST_MINOR		0
#define HV_CDEV_BUFF_SIZE		(4096 * 8)
#define HV_CDEV_CLASS_NAME		"hv_cdev_class"
#define HV_CDEV_DEVICE_FILENAME	"hv_cdev"

static int hv_mmap_type;
module_param(hv_mmap_type, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(hv_mmap_type, "HV mem mmap type: 0: wb, 1: wc, 2: uncached");

int hv_cdev_major;

/* 1 = if cmd drv reports no hv hw or ramdisk	*/
/* driver will use privatedata.buff[] as buffer	*/
static int use_static_buff;

dev_t hv_cdev_device_num;	/* contains major and minor #. For temp var */

static struct HV_MMLS_IO_t mmls_io_data;

typedef struct privatedata {
	int nminor;
	char buff[HV_CDEV_BUFF_SIZE];
	struct cdev cdev;
	struct fasync_struct *fasync;
	struct class *hv_cdev_class;
	struct mutex cmutex;
	struct device *hv_cdev_device;
	u64 dev_size;			/* bytes */
					/* phys_addr_t is word size of any arch */
	phys_addr_t phys_start;		/* physical address of mmls (mmls_start) */
	void __iomem *mmls_iomem;	/* kernel virtual address ptr of iomem   */
	unsigned long pfn;		/* page frame number of physical mem */
	unsigned long mmls_nsectors;	/* num of sectors */
} hv_cdev_private;

hv_cdev_private devices[HV_CDEV_N_MINORS];

extern int get_use_mmls_cdev(void);

static int hv_cdev_open(struct inode *inode, struct file *filp)
{
	hv_cdev_private *priv = container_of(inode->i_cdev, hv_cdev_private, cdev);

	filp->private_data = priv;

	PINFO("%s:\n", __func__);
	PDEBUG("%s: iminor = %d\n", __func__, iminor(inode));
	PDEBUG("%s: nminor = %d\n", __func__, priv->nminor);

	return 0;
}

static int hv_cdev_release(struct inode *inode, struct file *filp)
{
	hv_cdev_private *priv;

	priv = filp->private_data;

	/* flush data */
	priv = 0;

	PINFO("%s:\n", __func__);

	return 0;
}

static ssize_t hv_cdev_read(struct file *filp,
	char __user *ubuff, size_t count, loff_t *f_pos)
{
	int n = 0;
	hv_cdev_private *priv;

	priv = filp->private_data;

	PINFO("%s:\n", __func__);
	PDEBUG("ubuf=%p, count=%zu, f_pos=%lld, priv->dev_size=%llu\n",
				ubuff, count, *f_pos, priv->dev_size);

	if (mutex_lock_interruptible(&priv->cmutex)) {
		PERR("%s: Failed to acquire cmutex\n", __func__);
		return -ERESTARTSYS;
	}

	/* Check if file position over the dev size */
	if (*f_pos >= priv->dev_size) {
		PERR("file position exceeds disk size\n");
		goto out;
	}

	/* Trim if total req. read bytes over disk size */
	if ((*f_pos + count) > priv->dev_size)
		count = priv->dev_size - *f_pos;

	if (use_static_buff) {
		if (copy_to_user(ubuff, priv->buff + *f_pos, count)) {
			n = -EFAULT;
			PERR("Error: Copy_to_user failed\n");
			goto out;
		}
	} else {
		mmls_read_command(1, count/512, (*f_pos)/512, (unsigned long)priv->mmls_iomem, 0, NULL);
		if (copy_to_user(ubuff, priv->mmls_iomem + *f_pos, count)) {
			n = -EFAULT;
			PERR("Error: Copy_to_user failed\n");
			goto out;
		}
	}

	*f_pos += count;
	n = count;

out:
	mutex_unlock(&priv->cmutex);

	return n;
}

static ssize_t hv_cdev_write(struct file *filp,
	const char __user *ubuff, size_t count, loff_t *f_pos)
{
	int n = 0;
	hv_cdev_private *priv = filp->private_data;

	PINFO("%s:\n", __func__);

	PDEBUG("ubuf=%p, count=%zu, f_pos=%lld, priv->dev_size=%llu\n",
				ubuff, count, *f_pos, priv->dev_size);

	if (mutex_lock_interruptible(&priv->cmutex)) {
		PERR("%s: Failed to acquire cmutex\n", __func__);
		return -ERESTARTSYS;
	}

	/* Check if file position over the disksize */
	if (*f_pos >= priv->dev_size) {
		PERR("file position exceeds disk size");
		goto out;
	}

	/* Trim if total req. read bytes over disk size */
	if ((*f_pos + count) > priv->dev_size)
		count = priv->dev_size - *f_pos;

	/* Copy from user buffer to kernel buff */
	if (use_static_buff) {
		if (__copy_from_user_nocache(priv->buff + *f_pos, ubuff, count)) {
			n = -EFAULT;
			PERR("Error: Copy_from_user failed\n");
			goto out;
		}
	} else {
		if (__copy_from_user_nocache(priv->mmls_iomem + *f_pos, ubuff, count)) {
			n = -EFAULT;
			PERR("Error: Copy_from_user failed\n");
			goto out;
		}
		mmls_write_command(1, count/512, (*f_pos)/512, (unsigned long)priv->mmls_iomem, 0, NULL);
	}

	*f_pos += count;
	n = count;

out:
	mutex_unlock(&priv->cmutex);

	return n;
}

static long hv_cdev_ioctl(
		struct file *filp, unsigned int cmd , unsigned long arg)
{
	hv_cdev_private *priv;
	int i;

	PINFO("%s:\n", __func__);

	priv = filp->private_data;

	switch (cmd) {
	case HV_MMLS_SIZE:
		return put_user(priv->dev_size, (u64 __user *)arg);

	case HV_MMLS_FLUSH_RANGE:
	{
		struct hv_mmls_range range;

		if (copy_from_user(&range,
					(struct hv_mmls_range __user *)arg,
					sizeof(struct hv_mmls_range)))
			return -EFAULT;

		if (range.offset >= priv->dev_size)
			return -EINVAL;

		if (range.offset + range.size > priv->dev_size)
			range.size = priv->dev_size - range.offset;

		if (mutex_lock_interruptible(&priv->cmutex))
			return -ERESTARTSYS;

		if (use_static_buff) {
			/*  parm: virtual start address, size */
			clflush_cache_range(priv->buff + range.offset, range.size);
		} else {
			clflush_cache_range(
				priv->mmls_iomem + range.offset,
				range.size);
		}

		mutex_unlock(&priv->cmutex);
		break;
	}

	case HV_MMLS_DUMP_MEM:
	{
		struct hv_mmls_range range;

		if (copy_from_user(&range,
					(struct hv_mmls_range __user *)arg,
					sizeof(struct hv_mmls_range)))
			return -EFAULT;

		if (range.offset >= priv->dev_size)
			return -EINVAL;

		if (range.offset + range.size > priv->dev_size)
			range.size = priv->dev_size - range.offset;

		PDEBUG("mmls_iomem=%p, offset=%llu, size=%llu\n",
				priv->mmls_iomem,
				range.offset,
				range.size);

		if (mutex_lock_interruptible(&priv->cmutex))
			return -ERESTARTSYS;

		for (i = 0 + range.offset; i < range.size; i++)
			pr_info("mmls_iomem[%d]=0x%02X\n", i, ioread8(priv->mmls_iomem+i));

		mutex_unlock(&priv->cmutex);
		break;
	}

	default:
		return -ENOTTY;
	}

	return 0;
}

/* Asynchronous notification will be used to send SIGIO signal to user process
   Add kill_fasync(struct fasync_struct ** , int signo , int band); used
   to send signal along with the operation to be performed in the user process
   e.g, use in read or write signo will be SIGIO and band is POLL_IN for read
   POLL_OUT for write
 */
static int hv_cdev_fasync(int fd , struct file *filp , int mode)
{
	hv_cdev_private *priv;

	priv = filp->private_data;

	return fasync_helper(fd , filp , mode , &priv->fasync);
}

static loff_t hv_cdev_llseek(struct file *filp, loff_t off, int whence)
{
	loff_t newposition;

	PDEBUG("%s: loff_t = %lld, whence = %d", __func__, off, whence);

	switch (whence) {
	case 0: /* SEEK_SET */
		newposition = off;
		break;

	case 1: /* SEEK_CUR */
		newposition = filp->f_pos + off;
		break;

	case 2: /* SEEK_END */
		newposition = HV_CDEV_BUFF_SIZE + off;
		break;

	default:
		return -EINVAL;
	}

	if (newposition < 0)
		return -EINVAL;

	filp->f_pos = newposition;

	return newposition;
}


/*
 * mmap operation
 *
 * A request from userspace to map some source into its virtual addr space
 * the source is represented in file descriptor (supplied by char drv)
 * user space - only see virtual addresses. (its VA,  not Kernel VA)
 *
 */

static int
hv_cdev_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	return VM_FAULT_SIGBUS;
}

static struct vm_operations_struct hv_cdev_vm_ops = {
	.fault = hv_cdev_fault,
};

static int hv_cdev_mmap(struct file *filp, struct vm_area_struct *vma)
{
	hv_cdev_private *priv = filp->private_data;

	int res;

	/* vm_pgoff = the offset of the area in the file, in pages */
	/* shift by PAGE_SHIFT to get physical addr offset         */
	unsigned long off = vma->vm_pgoff << PAGE_SHIFT;

	/* off is decided by user's mmap() offset parm. If 0, off=0 */
	phys_addr_t physical = priv->phys_start + off;

	unsigned long vsize = vma->vm_end - vma->vm_start;
	unsigned long psize = priv->dev_size - off;

	PINFO("%s: enter\n", __func__);
	PINFO("off=%lu, physical=%p, vsize=%lu, psize=%lu\n",
			off, (void *)physical, vsize, psize);

	if (vsize > psize) {
		PERR("%s: requested vma size exceeds disk size\n", __func__);
		return -EINVAL;
	}

	vma->vm_ops = &hv_cdev_vm_ops;

	switch (hv_mmap_type) {
	case 0:
	default:
		break;
	case 1:
		pgprot_writecombine(vma->vm_page_prot);
		break;
	case 2:
		pgprot_noncached(vma->vm_page_prot);
		break;
	}

	vma->vm_flags |= VM_LOCKED;	/* locked from swap */

	PDEBUG("phys_start=%p, page_frame_num=%d\n",
		(void *)priv->phys_start, (int)priv->phys_start >> PAGE_SHIFT);

	/* Remap the phys addr of device into user space virtual mem */
	res = remap_pfn_range(vma,
			vma->vm_start,
			physical >> PAGE_SHIFT,	/* = pfn */
			vsize,
			vma->vm_page_prot);

	if (res) {
		PERR("%s: error from remap_pfn_range()/n", __func__);
		return -EAGAIN;
	} else
		PDEBUG("%s: Physical mem remapped to user VA\n", __func__);

	return 0;
}

static const struct file_operations hv_cdev_fops = {
	.owner				= THIS_MODULE,
	.open				= hv_cdev_open,
	.release			= hv_cdev_release,
	.read				= hv_cdev_read,
	.write				= hv_cdev_write,
	.unlocked_ioctl			= hv_cdev_ioctl,
	.fasync				= hv_cdev_fasync,
	.llseek				= hv_cdev_llseek,
	.mmap				= hv_cdev_mmap,
};


/*
 * mmls_init()
 *
 * This function returns memory size and phys addr from lower cmd
 * driver. Cmd driver is the direct interface to hardware module.
 *
 * Currently, cmd drv returns one device only.
 * To support more mmls device with 1 major cdev and multiple
 * minor num cdev, then cmd drv and mmls_init fn need to be changed
 */
static int mmls_init(void)
{
	PINFO("%s: enter\n", __func__);

	if (mmls_io_init()) {
		PERR("mmls_start/mmls_size is zero. ret=%d\n", -EINVAL);
		return -EINVAL;
	}

	get_mmls_iodata(&mmls_io_data);

	PINFO("%s: found mmls device with size and addr:\n", __func__);
	PINFO("mmls_io_data.m_size=%lu\n", mmls_io_data.m_size);
	PINFO("mmls_io_data.m_iomem=%p\n", mmls_io_data.m_iomem);
	PINFO("mmls_nsectors=%lu", (mmls_io_data.m_size)/HV_BLOCK_SIZE);

	return 0;
}

static int __init hv_cdev_init(void)
{
	int i, j;
	int res;

	PERR("%s: INIT\n", __func__);

	if (!get_use_mmls_cdev()) {
		PINFO("%s: Not using mmls char driver\n", __func__);
		res = -ENODEV;
		goto not_using_cdev;
	}

	/* Get dev major number assignment from kernel.	*/
	/* Returned in hv_cdev_device_num		*/
	res = alloc_chrdev_region(&hv_cdev_device_num,
				HV_CDEV_FIRST_MINOR,
				HV_CDEV_N_MINORS,
				DRIVER_NAME);
	if (res) {
		PERR("register device no failed\n");
		return -1;
	}

	/* Extract major #. hv_cdev_device_num has both major and minor */
	hv_cdev_major = MAJOR(hv_cdev_device_num);

	/* Create just 1 char dev as HV_CDEV_N_MINORS=1 as now */
	for (i = 0; i < HV_CDEV_N_MINORS; i++) {
		/* First, init mmls device to get size and addr */
		if (mmls_init()) {
			PERR("%s: mmls size module parm is 0.\n", __func__);
			PERR("No ramdisk. Use static buff for test\n");
			devices[i].dev_size = HV_CDEV_BUFF_SIZE;

			/* This needs to be in priv data if >1 mmls dev	*/
			use_static_buff = 1;
		} else {
			/* Populate private data with mmls device info	*/
			/* Assumed only 1 mmls device.			*/
			/* In the future, if more mmls devices are used */
			/* with minor #, then mmls_init fn need change	*/
			/* to return more devices with their specific	*/
			/* size and addresses.				*/
			devices[i].mmls_nsectors =
				(mmls_io_data.m_size)/HV_BLOCK_SIZE;

			devices[i].dev_size = mmls_io_data.m_size;
			devices[i].phys_start = mmls_io_data.phys_start;
			devices[i].mmls_iomem = mmls_io_data.m_iomem;
			use_static_buff = 0;
		}

		hv_cdev_device_num = MKDEV(hv_cdev_major, HV_CDEV_FIRST_MINOR+i);

		cdev_init(&devices[i].cdev , &hv_cdev_fops);
		devices[i].cdev.owner = THIS_MODULE;

		res = cdev_add(&devices[i].cdev, hv_cdev_device_num, 1);
		if (res < 0) {
			PERR("%s: Failed to add char dev\n", __func__);
			return -ENODEV;
		}

		devices[i].nminor = HV_CDEV_FIRST_MINOR+i;

		PINFO("hv_cdev is registered with major#=%d, minor#=%d\n",
				hv_cdev_major,
				HV_CDEV_FIRST_MINOR+i);
		PINFO("hv_cdev owner = %p\n", devices[i].cdev.owner);

		/* Add virtual device class for file access to driver */
		devices[i].hv_cdev_class = class_create(THIS_MODULE, HV_CDEV_CLASS_NAME);

		if (IS_ERR(devices[i].hv_cdev_class)) {
			PERR("%s: Failed to create hv_cdev class\n", __func__);
			res = PTR_ERR(devices[i].hv_cdev_class);
			goto failed_classreg;
		}

		devices[i].hv_cdev_device = device_create(
					devices[i].hv_cdev_class,
					NULL,
					MKDEV(hv_cdev_major, i),
					NULL,
					HV_CDEV_DEVICE_FILENAME "%d",
					i);

		if (IS_ERR(devices[i].hv_cdev_device)) {
			PERR("Failed to create device '%s_%s%d'\n",
					HV_CDEV_CLASS_NAME,
					HV_CDEV_DEVICE_FILENAME,
					i);
			res = PTR_ERR(devices[i].hv_cdev_device);
			goto failed_devreg;
		}

		mutex_init(&devices[i].cmutex);
	}

	PINFO("INIT\n");

	return 0;

failed_devreg:
	/* If fails, i still has the last index of devices[i] that failed */
	for (j = 0; j <= i; j++) {
		hv_cdev_device_num = MKDEV(hv_cdev_major, HV_CDEV_FIRST_MINOR+j);
		device_destroy(devices[j].hv_cdev_class, hv_cdev_device_num);
		class_unregister(devices[j].hv_cdev_class);
		class_destroy(devices[j].hv_cdev_class);
	}

failed_classreg:
	unregister_chrdev(hv_cdev_major, HV_CDEV_CLASS_NAME);

not_using_cdev:
	return res;
}

static void __exit hv_cdev_exit(void)
{
	int i;

	PINFO("EXIT\n");

	if (!get_use_mmls_cdev()) {
		PINFO("%s: nothing to un-init; not using cdev\n", __func__);
		goto not_using_cdev;
	}

	for (i = 0; i < HV_CDEV_N_MINORS; i++) {
		hv_cdev_device_num = MKDEV(hv_cdev_major, HV_CDEV_FIRST_MINOR+i);
		device_destroy(devices[i].hv_cdev_class, hv_cdev_device_num);
		class_unregister(devices[i].hv_cdev_class);
		class_destroy(devices[i].hv_cdev_class);
		cdev_del(&devices[i].cdev);
	}

	unregister_chrdev_region(hv_cdev_device_num, HV_CDEV_N_MINORS);

	mmls_iomem_release();

not_using_cdev:
	return;
}

module_init(hv_cdev_init);
module_exit(hv_cdev_exit);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("S. Gosali");
MODULE_DESCRIPTION("Linux kernel char driver sample");
