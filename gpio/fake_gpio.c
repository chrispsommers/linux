// Fake GPIO driver
//
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include<linux/slab.h>
#include<linux/uaccess.h>
#include <linux/err.h>
#include <linux/mutex.h>

#define GPIO_BUF_SZ        10
#define DEV_CLASS "fake_gpio_class"
#define DEV_NAME "fake_gpio"
 
dev_t dev = 0;
static struct class *dev_class;
static struct cdev fake_gpio_cdev;
char *gpio_buf;
char *buf_head;
char *buf_tail;
char *buf_start;
char *buf_end;
size_t buf_count = 0;
struct kobject *gpio_kobj;

DEFINE_MUTEX(buf_mutex);

typedef enum gpio_mode_e {
        FIFO = 0,
        BITSTREAM =1
} gpio_mode_t;

volatile gpio_mode_t gpio_mode = 0;

static int      __init fake_gpio_driver_init(void);
static void     __exit fake_gpio_driver_exit(void);
static int      fake_gpio_open(struct inode *inode, struct file *file);
static int      fake_gpio_release(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_read(struct file *filp, char __user *buf, size_t len,loff_t * off);
static ssize_t  fake_gpio_write(struct file *filp, const char *buf, size_t len, loff_t * off);
static ssize_t  gpio_mode_get(struct kobject *kobj, 
                        struct kobj_attribute *attr, char *buf);
static ssize_t  gpio_mode_set(struct kobject *kobj, 
                        struct kobj_attribute *attr,const char *buf, size_t count);
struct kobj_attribute gpio_mode_attr = __ATTR(gpio_mode, 0660, gpio_mode_get, gpio_mode_set);

// File ops structure
static struct file_operations fops =
{
        .owner          = THIS_MODULE,
        .read           = fake_gpio_read,
        .write          = fake_gpio_write,
        .open           = fake_gpio_open,
        .release        = fake_gpio_release,
};
 
/*
 * Open device file
 */
static int fake_gpio_open(struct inode *inode, struct file *file)
{
        pr_info("fake_gpio: opened\n");
        return 0;
}

/*
 * Close device file
 */
static int fake_gpio_release(struct inode *inode, struct file *file)
{
        pr_info("fake_gpio: device file closed\n");
        return 0;
}

// push bytes onto circular buffer, does not overwrite
size_t buffer_push(const char __user *buf, size_t len) {
        const char __user *rp;
        int rc;
        size_t i;
        rp = buf;
        for (i = 0; i < len; i++) {
                if (buf_count == GPIO_BUF_SZ) {
                        pr_info("fake_gpio: buffer_push(): buffer full upon writing %ld bytes\n", i);
                        return i;
                }
                rc = copy_from_user(buf_head, rp, 1);
                if(rc) {
                        pr_err("fake_gpio: buffer_push() copy_from_user(): ERROR %d!\n", rc);
                        return i;
                }
                buf_count++;
                rp++;
                buf_head++;
                if (buf_head > buf_end) {
                        buf_head = buf_start; // wraparound
                }
                pr_info("fake_gpio: buffer_push() buf_count=%ld, buf_head=%p\n", buf_count, buf_head);
        }
        return i;
}

// pop bytes off of circular buffer; no underrun allowed
size_t buffer_pop(char __user *buf, size_t len) {
        char *wp;
        int rc;
        size_t i;
        wp = buf;
        pr_info("fake_gpio: buffer_pop(): buf_count= %ld\n", buf_count);
        for (i = 0; i < len; i++) {
                if (buf_count == 0) {
                        pr_info("fake_gpio: buffer_pop(): buffer empty upon reading %ld bytes\n", i);
                        return i;
                }
                rc = copy_to_user(wp, buf_tail, 1);
                if(rc) {
                        pr_err("fake_gpio: buffer_pop() copy_to_user(): ERROR %d!\n", rc);
                        return i;
                }
                buf_count--;
                buf_tail++;
                wp++;
                if (buf_tail > buf_end) {
                        buf_tail = buf_start; // wraparound
                }
                pr_info("fake_gpio: buffer_pop() buf_count=%ld, buf_tail=%p\n", buf_count, buf_tail);
        }
        return i;
}

/*
 * Read from device file
 */
static ssize_t fake_gpio_read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
        int rc = 0;
        size_t rlen = 0;
        size_t actual_rlen=0;
        rc = mutex_lock_interruptible(&buf_mutex);
        if (rc) {
            pr_err("fake_gpio: fake_gpio_write(): mutex interrupted, err = %d\n", rc);
            return rc;
        }
        //Copy the data from the kernel space to the user-space
        if (buf_count == 0) {
                pr_info("fake_gpio: fake_gpio_read() - buffer empty, nothing to read\n");
                mutex_unlock(&buf_mutex);
                return 0;
        }

        rlen = (len < buf_count)?len:buf_count;
        pr_info("fake_gpio: fake_gpio_read() request to read %ld bytes, will actually try to read %ld bytes\n", len, rlen);
        actual_rlen = buffer_pop(buf, rlen);
        pr_info("fake_gpio: fake_gpio_read(): popped %ld bytes\n", actual_rlen);
        mutex_unlock(&buf_mutex);
        return actual_rlen;
}

/*
 * Write to device file
 */
static ssize_t fake_gpio_write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
        size_t wlen;
        size_t actual_wlen = 0;
        int rc = 0;

        pr_info("fake_gpio: fake_gpio_write() request to write %ld bytes\n", len);
        rc = mutex_lock_interruptible(&buf_mutex);
        if (rc) {
            pr_err("fake_gpio: fake_gpio_write(): mutex interrupted, err = %d\n", rc);
            return rc;
        }
        if (buf_count == GPIO_BUF_SZ) {
                pr_info("fake_gpio: fake_gpio_write() - buffer full, nothing to write\n");
                mutex_unlock(&buf_mutex);
                return ENOSPC;
        }        
        if (len > GPIO_BUF_SZ) {
                wlen = GPIO_BUF_SZ;
                pr_info("fake_gpio: fake_gpio_write() - truncating write to %ld bytes\n", wlen);

        } else {
                wlen = len;
                pr_info("fake_gpio: fake_gpio_write() - actually writing %ld bytes\n", wlen);
        }
        mutex_unlock(&buf_mutex);
        actual_wlen = buffer_push(buf, wlen);
        pr_info("fake_gpio: pushed %ld bytes\n", actual_wlen);
        return actual_wlen;
}

/*
 * sysfsfs mode read op
 */
static ssize_t gpio_mode_get(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
        ssize_t len;
        len = sprintf(buf, "%d", gpio_mode);
        pr_info("gpio_mode_get() => '%s'\n", buf);
        return len;
}

/*
 * sysfsfs mode write op
 */
static ssize_t gpio_mode_set(struct kobject *kobj, 
                struct kobj_attribute *attr,const char *buf, size_t count)
{
        ssize_t len;
        len = sscanf(buf,"%d",(int *)(&gpio_mode));
        pr_info("gpio_mode_set() => %d\n", gpio_mode);
        return count;
}

/*
 * Module insert
 */
static int __init fake_gpio_driver_init(void)
{
        // Get dynamic Major char dev number
        if((alloc_chrdev_region(&dev, 0, 1, DEV_NAME)) <0){
                pr_err("fake_gpio: fake_gpio_driver_init(): Cannot allocate major number\n");
                return -1;
        }
 
        // Create char dev struct
        cdev_init(&fake_gpio_cdev,&fops);
         if((cdev_add(&fake_gpio_cdev,dev,1)) < 0){
            pr_err("fake_gpio: fake_gpio_driver_init(): Cannot add the device to the system\n");
            goto destroy_chrdev_region;
        }
 
        // Create class
        if(IS_ERR(dev_class = class_create(THIS_MODULE,DEV_CLASS))){
            pr_err("fake_gpio: fake_gpio_driver_init(): Cannot create the struct class\n");
            goto destroy_chrdev_region;
        }
 
        // Finally, create the device
        if(IS_ERR(device_create(dev_class,NULL,dev,NULL, DEV_NAME))){
            pr_err("fake_gpio: fake_gpio_driver_init(): Cannot create the Device 1\n");
            goto destroy_device;
        }
        
        // Alloc buffer for gpio
        if((gpio_buf = kmalloc(GPIO_BUF_SZ , GFP_KERNEL)) == 0){
            pr_err("fake_gpio: fake_gpio_driver_init(): Cannot allocate memory in kernel\n");
            goto destroy_device;
        }
        buf_head = gpio_buf;
        buf_tail = gpio_buf;
        buf_start = gpio_buf;
        buf_end = gpio_buf+GPIO_BUF_SZ-1;
        pr_info("fake_gpio: fake_gpio_driver_init() buf_start=%p, buf_end=%p, buf_head=%p, buf_tail=%p\n", buf_start, buf_end, buf_head, buf_tail);
    
        pr_info("fake_gpio: fake_gpio_driver_init(): inserted device %s, class %s, MAJOR=%d, MINOR=%d\n", DEV_NAME, DEV_CLASS, MAJOR(dev), MINOR(dev));

        // Create sysfs directory in /sys/kernel/
        gpio_kobj = kobject_create_and_add("fake_gpio_sysfs",kernel_kobj);
 
        // Create sysfs file node for gpio_mode
        if(sysfs_create_file(gpio_kobj,&gpio_mode_attr.attr)){
                pr_err("Cannot create sysfs file......\n");
                goto destroy_sysfs;
        }
        return 0;

 
destroy_sysfs:
        kobject_put(gpio_kobj); 
        sysfs_remove_file(kernel_kobj, &gpio_mode_attr.attr);

destroy_device:
        class_destroy(dev_class);

destroy_chrdev_region:
        unregister_chrdev_region(dev,1);
        return -1;
}

/*
 * Module remove
 */
static void __exit fake_gpio_driver_exit(void)
{
	kfree(gpio_buf);
        device_destroy(dev_class,dev);
        class_destroy(dev_class);
        cdev_del(&fake_gpio_cdev);
        unregister_chrdev_region(dev, 1);
        pr_info("fake_gpio: device driver removed\n");
}
 
module_init(fake_gpio_driver_init);
module_exit(fake_gpio_driver_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Sommers");
MODULE_DESCRIPTION("Fake GPIO driver");
MODULE_VERSION("1.0");
