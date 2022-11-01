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
#include <linux/delay.h>
#include <linux/kthread.h>

#define GPIO_BUF_SZ        10
#define DEV_CLASS "fake_gpio_class"
#define DEV_NAME "fake_gpio"
#define SYSFS_DIR "fake_gpio_sysfs"
 
dev_t dev = 0;
static struct class *dev_class = NULL;
static struct cdev fake_gpio_cdev;
char *gpio_buf = NULL;
char *buf_head = NULL;
char *buf_tail = NULL;
char *buf_start = NULL;
char *buf_end = NULL;
size_t buf_count = 0;
struct kobject *gpio_kobj = NULL;
static struct task_struct *drain_loop_thread = NULL;
bool drain_loop_thread_running = false;

DEFINE_MUTEX(buf_mutex);

typedef enum gpio_mode_e {
        MODE_FIFO_ONLY = 0,                             // can read and write buffer, no serialzation
        MODE_SERIALIZE_BLOCKING =1,                     // writes cause draining & serialization; blocking
        MODE_SERIALIZE_NONBLOCKING_POLLED = 2           // writes cause draining & serialization; non-blocking via polling thread
} gpio_mode_t;

volatile gpio_mode_t gpio_mode = MODE_FIFO_ONLY;

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
static ssize_t  buf_count_get(struct kobject *kobj, 
                        struct kobj_attribute *attr, char *buf);
static ssize_t  buf_count_set(struct kobject *kobj, 
                        struct kobj_attribute *attr,const char *buf, size_t count);
                        
struct kobj_attribute gpio_mode_attr = __ATTR(gpio_mode, 0660, gpio_mode_get, gpio_mode_set);
struct kobj_attribute buf_count_attr = __ATTR(buf_count, 0660, buf_count_get, buf_count_set);
void drain_buffer(void);

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

// push one byte into buffer
// return 1 if success, 0 if no room available
// no synchronization, caller must ensure e.g. via mutex
int buffer_push_one(char data) {
        if (buf_count < GPIO_BUF_SZ) {
                buf_count++;
                *buf_head++ = data;
                if (buf_head > buf_end) {
                        buf_head = buf_start; // wraparound
                }
                pr_info("fake_gpio: buffer_push_one(): data = '%c', buf_count=%ld, buf_head=%p\n", data, buf_count, buf_head);
        } else {
                pr_info("fake_gpio: buffer_push_one(): buffer full, cannot push\n");
                return 0;
        }
        return 1;
}

// push bytes onto circular buffer, does not overwrite
// no synchronization, caller must ensure e.g. via mutex
size_t buffer_push(const char __user *buf, size_t len) {
        const char __user *rp;
        int rc;
        char data;
        size_t i;
        rp = buf;
        for (i = 0; i < len; i++) {
                // copy userspace to kernel space one byte at a time & push to FIFO
                rc = copy_from_user(&data, rp++, 1);
                if(rc) {
                        pr_err("fake_gpio: buffer_push() copy_from_user(): ERROR %d!\n", rc);
                        break;
                }
                if (buffer_push_one(data) == 0) {
                        pr_info("fake_gpio: buffer_push(): buffer full upon writing %ld bytes\n", i);
                        break;
                }
        }
        return i;
}

// pop one byte off buffer
// return 0 if empty, 1 if byte was available
// no synchronization, caller must ensure e.g. via mutex
int buffer_pop_one(char  *data) {
        if (buf_count == 0) {
                // pr_info("fake_gpio: buffbuffer_pop_one(): buffer empty\n");
                return 0;
        }
        *data = *buf_tail;
        buf_count--;
        buf_tail++;
        if (buf_tail > buf_end) {
                buf_tail = buf_start; // wraparound
        }
        pr_info("fake_gpio: buffer_pop_one(): data='%c',  buf_count=%ld, buf_tail=%p\n", *data, buf_count, buf_tail);
        return 1;
}

// pop bytes off of circular buffer; no underrun allowed
// no synchronization, caller must ensure e.g. via mutex
size_t buffer_pop(char __user *buf, size_t len) {
        char *wp;
        int rc;
        size_t i;
        char data;
        wp = buf;
        pr_info("fake_gpio: buffer_pop(): buf_count= %ld\n", buf_count);
        for (i = 0; i < len; i++) {
                if (!buffer_pop_one(&data)) {
                        pr_info("fake_gpio: buffer_pop(): buffer empty upon reading %ld bytes\n", i);
                        return i;
                }
                rc = copy_to_user(wp++, &data, 1);
                if(rc) {
                        pr_err("fake_gpio: buffer_pop() copy_to_user(): ERROR %d!\n", rc);
                        return i;
                }
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

        if (buf_count == 0) {
                pr_info("fake_gpio: fake_gpio_read() - buffer empty, nothing to read\n");
                mutex_unlock(&buf_mutex);
                return 0;
        }

        rlen = (len < buf_count)?len:buf_count;
        pr_info("fake_gpio: fake_gpio_read() request to read %ld bytes, will actually try to read %ld bytes\n", len, rlen);
        // finally pop data from internal FIFO buffer
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
        // finally push data into internal FIFO buffer
        actual_wlen = buffer_push(buf, wlen);
        mutex_unlock(&buf_mutex);
        pr_info("fake_gpio: pushed %ld bytes\n", actual_wlen);
        if (gpio_mode == MODE_SERIALIZE_BLOCKING) {
                pr_info("fake_gpio: draining buffer (blocking)...\n");
                drain_buffer();
        }
        return actual_wlen;
}

#define WAIT_DLY_MIN_USEC 100000      // .100 sec
#define WAIT_DLY_MAX_USEC 101000      // .101 sec
#define BIT_DURATION_MIN_USEC 100000  // .100 sec
#define BIT_DURATION_MAX_USEC 101000  // .101 sec

char preamble[2] = {0,1}; // bits represented as bytes for simplicity to change
char postamble[2] = {0,1};

void clear_output(void) {
        pr_info("fake_gpio: clear_output() sent 0\n");
}

void set_output(void) {
        pr_info("fake_gpio: set_output()   sent 1\n");
}

void send_bit(bool bit) {
        if (bit) {
                set_output();
        } else {
                clear_output();
        }
        usleep_range(BIT_DURATION_MIN_USEC,BIT_DURATION_MAX_USEC);
}

void send_x_amble(char *data, ssize_t len) {
        int i;
        pr_info("fake_gpio: sending framing %ld bits\n", len);
        for (i = 0; i< len; i++) {
                send_bit(data[i]);
        }
}

void serialize_byte(char data) {
        int i;
        pr_info("fake_gpio: serialize_byte 0x%02x\n", data);

        send_x_amble(preamble, sizeof(preamble)/sizeof(preamble[0]));
        pr_info("fake_gpio: sending data 8 bits\n");
        for (i = 0; i < 8; i++) {
                send_bit(data & 1<<i);
        }
        send_x_amble(postamble, sizeof(postamble)/sizeof(postamble[0]));
}

// Drain the buffer & serialize it to FAKE I2C bus
// https://www.kernel.org/doc/html/latest/timers/timers-howto.html
void drain_buffer(void) {
        char data;
        int rc = 1;
        while (rc) {
                mutex_lock(&buf_mutex);
                rc = buffer_pop_one(&data);
                mutex_unlock(&buf_mutex);
                if (rc) {
                        serialize_byte(data);
                }
        }
}
int drain_buffer_thread(void *context) {
        char data;
        int rc = 1;
        pr_info("fake_gpio: drain_buffer_thread(): entered thread\n");
        drain_loop_thread_running = true;
        while(!kthread_should_stop()) {
                mutex_lock(&buf_mutex);
                rc = buffer_pop_one(&data);
                mutex_unlock(&buf_mutex);
                if (rc) {
                        serialize_byte(data);
                }
                usleep_range(WAIT_DLY_MIN_USEC,WAIT_DLY_MAX_USEC);
        }
        drain_loop_thread_running = false;
        pr_info("fake_gpio: drain_buffer_thread(): leaving thread\n");
        return 0;
}

/*
 * /sys/kernel/SYSFS_DIR/gpio_mode read op
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
 * /sys/kernel/SYSFS_DIR/gpio_mode write op
 */
static ssize_t gpio_mode_set(struct kobject *kobj, 
                struct kobj_attribute *attr,const char *buf, size_t count)
{
        ssize_t len;
        len = sscanf(buf,"%d",(int *)(&gpio_mode));
        pr_info("gpio_mode_set() => %d\n", gpio_mode);

        if ( (gpio_mode != MODE_SERIALIZE_NONBLOCKING_POLLED) && \
                drain_loop_thread ) {
                        kthread_stop(drain_loop_thread);
                }
        switch (gpio_mode) {
        case MODE_FIFO_ONLY:
                break;

        case MODE_SERIALIZE_BLOCKING:
                break;

        case MODE_SERIALIZE_NONBLOCKING_POLLED:
                if (!drain_loop_thread) {
                        pr_info("fake_gpio: gpio_mode_set(): Creating kthread...\n");
                        drain_loop_thread = kthread_create(drain_buffer_thread,NULL,"drain_buffer_thread");
                }
                if(drain_loop_thread && ! drain_loop_thread_running) {
                        // TODO - prevent race condition as thread is about to exit
                        pr_info("fake_gpio: gpio_mode_set(): waking up thread...\n");
                        wake_up_process(drain_loop_thread);
                } else {
                        pr_err("fake_gpio: gpio_mode_set(): Cannot create kthread\n");
                }
                break;
        }
        return count;
}

/*
 * /sys/kernel/SYSFS_DIR/buf_count read op
 */
static ssize_t buf_count_get(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
        ssize_t len;
        len = sprintf(buf, "%ld", buf_count);
        pr_info("buf_count_get() => '%s'\n", buf);
        return len;
}

/*
 * /sys/kernel/SYSFS_DIR/buf_count write op
 */
static ssize_t buf_count_set(struct kobject *kobj, 
                struct kobj_attribute *attr,const char *buf, size_t count)
{
        pr_info("buf_count_set() => INVALID, not supported\n");
        return EINVAL;
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

        // Create sysfs directory in /sys/kernel/
        gpio_kobj = kobject_create_and_add(SYSFS_DIR,kernel_kobj);
 
        // Create sysfs file node for gpio_mode
        if(sysfs_create_file(gpio_kobj,&gpio_mode_attr.attr)){
                pr_err("Cannot create sysfs/gpio_mode file......\n");
                goto destroy_sysfs;
        }
 
        // Create sysfs file node for buf_count
        if(sysfs_create_file(gpio_kobj,&buf_count_attr.attr)){
                pr_err("Cannot create sysfs/buf_count file......\n");
                goto destroy_sysfs;
        }
    
        pr_info("fake_gpio: fake_gpio_driver_init(): inserted device %s, class %s, MAJOR=%d, MINOR=%d sysfs=%s\n",
                 DEV_NAME, DEV_CLASS, MAJOR(dev), MINOR(dev), SYSFS_DIR);
        return 0;
 
destroy_sysfs:
        kobject_put(gpio_kobj); 
        sysfs_remove_file(kernel_kobj, &gpio_mode_attr.attr);
        sysfs_remove_file(kernel_kobj, &buf_count_attr.attr);

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
        if (drain_loop_thread_running && drain_loop_thread) {
                kthread_stop(drain_loop_thread);
        }
        kobject_put(gpio_kobj); 
        sysfs_remove_file(kernel_kobj, &gpio_mode_attr.attr);
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
