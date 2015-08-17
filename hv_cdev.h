#define DRIVER_NAME "hv_cdev"

/* Debug logs toggle from lower importance to higher */
#define USE_DEBUG_LOG	1
#define USE_INFO_LOG	1

#if USE_DEBUG_LOG
#define PDEBUG(fmt,args...) printk(KERN_DEBUG"%s:"fmt,DRIVER_NAME, ##args)
#else
#define PDEBUG(fmt,args...) { }
#endif

#if USE_INFO_LOG
#define PINFO(fmt,args...) printk(KERN_INFO"%s:"fmt,DRIVER_NAME, ##args)
#else
#define PINFO(fmt,args...) { }
#endif

#define PERR(fmt,args...) printk(KERN_ERR"%s:"fmt,DRIVER_NAME,##args)
#include<linux/capability.h>
#include<linux/cdev.h>
#include<linux/device.h>
#include<linux/fs.h>
#include<linux/init.h>
#include<linux/ioctl.h>
#include<linux/kdev_t.h>
#include<linux/module.h>
#include<linux/moduleparam.h>
#include<linux/types.h>
#include<linux/uaccess.h>

/* Use RAMDISK? */
#define USE_RAMDISK 0
