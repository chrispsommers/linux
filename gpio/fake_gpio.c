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
#include <linux/sched.h>
#include<linux/proc_fs.h>

#define GPIO_BUF_SZ        10
#define DEV_CLASS "fake_gpio_class"
#define DEV_NAME "fake_gpio"
#define SYSFS_DIR "fake_gpio_sysfs"
#define PROC_DIR "fake_gpio_sysfs"
#define PROC_ENTRY_NAME "buf_count"
 
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
static struct task_struct *drain_poll_loop_thread = NULL;
bool drain_poll_loop_thread_running = false;
static struct task_struct *drain_wait_loop_thread = NULL;
bool drain_wait_loop_thread_running = false;
DECLARE_WAIT_QUEUE_HEAD(gpio_buffer_wq);
DEFINE_MUTEX(buf_mutex);
static struct proc_dir_entry *gpio_proc_root;
bool gpio_proc_read_done = false;


typedef enum gpio_mode_e {
        MODE_FIFO_ONLY = 0,                             // can read and write buffer, no serialzation
        MODE_SERIALIZE_BLOCKING =1,                     // writes cause draining & serialization; blocking
        MODE_SERIALIZE_NONBLOCKING_POLLED = 2,          // writes cause draining & serialization; non-blocking via polling thread
        MODE_SERIALIZE_NONBLOCKING_WAITQ = 3            // writes cause draining & serialization; non-blocking via wait queue
} gpio_mode_t;

volatile gpio_mode_t gpio_mode = MODE_FIFO_ONLY;

static int      __init fake_gpio_driver_init(void);
static void     __exit fake_gpio_driver_exit(void);
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
void drain_buffer_blocking(void);

// File ops callbacks & structure
static int      fake_gpio_open(struct inode *inode, struct file *file);
static int      fake_gpio_release(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_read(struct file *filp, char __user *buf, size_t len,loff_t * off);
static ssize_t  fake_gpio_write(struct file *filp, const char *buf, size_t len, loff_t * off);

static struct file_operations fops =
{
        .owner          = THIS_MODULE,
        .read           = fake_gpio_read,
        .write          = fake_gpio_write,
        .open           = fake_gpio_open,
        .release        = fake_gpio_release,
};


// procfs operation callbacks & structure
static int      fake_gpio_open_proc(struct inode *inode, struct file *file);
static int      fake_gpio_release_proc(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_read_proc(struct file *filp, char __user *buffer, size_t length,loff_t * offset);
// static ssize_t  fake_gpio_write_proc(struct file *filp, const char *buff, size_t len, loff_t * off);

static struct proc_ops proc_fops = {
        .proc_open = fake_gpio_open_proc,
        .proc_read = fake_gpio_read_proc,
        // .proc_write = fake_gpio_write_proc,
        .proc_write = NULL, // no writes
        .proc_release = fake_gpio_release_proc
}; 

// Open device file
static int fake_gpio_open(struct inode *inode, struct file *file)
{
        pr_info("fake_gpio: opened\n");
        return 0;
}

// Close device file
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
        wake_up_interruptible(&gpio_buffer_wq);
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
                drain_buffer_blocking();
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
        pr_info("fake_gpio:   clear_output() sent 0\n");
}

void set_output(void) {
        pr_info("fake_gpio:   set_output()   sent 1\n");
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

// shift bits out LSB first, surrounded by framing
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

// send a byte out if available; mutex-protected
int send_one_byte_conditionally(void) {
        char data;
        int rc = 0;
        mutex_lock(&buf_mutex);
        rc = buffer_pop_one(&data);
        mutex_unlock(&buf_mutex);
        if (rc) {
                serialize_byte(data);
        }
        return rc;
}

// Drain the buffer & serialize it to FAKE I2C bus - blocking
// https://www.kernel.org/doc/html/latest/timers/timers-howto.html
void drain_buffer_blocking(void) {
        // char data;
        int rc = 1;
        while (rc) {
                rc = send_one_byte_conditionally();
        }
}

/*
 * Continuously poll buffer in a thread
 * Drain a byte at a time if gpio_mode == MODE_SERIALIZE_NONBLOCKING_POLLED
 */
int drain_buffer_poll_thread_fn(void *context) {
        // char data;
        int rc = 0;
        pr_info("fake_gpio: drain_buffer_poll_thread_fn(): entered thread\n");
        allow_signal(SIGKILL);
        drain_poll_loop_thread_running = true;
        while(!kthread_should_stop()) {
                if (gpio_mode == MODE_SERIALIZE_NONBLOCKING_POLLED) {
                        rc = send_one_byte_conditionally();
                }
                if (signal_pending(drain_poll_loop_thread)) // catch sigkill
                        break;

                usleep_range(WAIT_DLY_MIN_USEC,WAIT_DLY_MAX_USEC);
        }
        drain_poll_loop_thread_running = false;
        pr_info("fake_gpio: drain_buffer_poll_thread_fn(): leaving thread\n");
        return 0;
}
/*
 * Thread, wait on gpio_mode == MODE_SERIALIZE_NONBLOCKING_POLLED and drain
  * buffer, otherwise go back to waiting
 */
int drain_buffer_wait_thread_fn(void *context) {
        // char data;
        gpio_mode_t old_mode = 0;
        int rc = 0;
        pr_info("fake_gpio: drain_buffer_wait_thread_fn(): entered thread\n");
        allow_signal(SIGKILL);
        drain_wait_loop_thread_running = true;
        while(!kthread_should_stop()) {
                pr_info("fake_gpio: drain_buffer_wait_thread_fn(): Waiting ...\n");
                // Wake up on sigkill, or wait mode && (data to send or mode changed)
                wait_event_interruptible(gpio_buffer_wq,
                                                (kthread_should_stop() ||
                                                ( (gpio_mode == MODE_SERIALIZE_NONBLOCKING_WAITQ) &&
                                                  ( (buf_count > 0) || (gpio_mode != old_mode))
                                                )
                                        ) );
                pr_info("fake_gpio: drain_buffer_wait_thread_fn(): woke up!\n");
                old_mode = gpio_mode;
                if(gpio_mode == MODE_SERIALIZE_NONBLOCKING_WAITQ) {
                        rc = send_one_byte_conditionally();
                }
                if (signal_pending(drain_wait_loop_thread)) // catch sigkill
                        break;

                usleep_range(WAIT_DLY_MIN_USEC,WAIT_DLY_MAX_USEC);
        }
        drain_wait_loop_thread_running = false;
        pr_info("fake_gpio: drain_buffer_wait_thread_fn(): leaving thread\n");
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
        pr_info("fake_gpio: gpio_mode_set() => %d\n", gpio_mode);
        if ((gpio_mode == MODE_SERIALIZE_NONBLOCKING_WAITQ) && drain_wait_loop_thread) {
                wake_up_interruptible(&gpio_buffer_wq);
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
        // pr_info("buf_count_get() => '%s'\n", buf);
        return len;
}

// /sys/kernel/SYSFS_DIR/buf_count write op
static ssize_t buf_count_set(struct kobject *kobj, 
                struct kobj_attribute *attr,const char *buf, size_t count)
{
        pr_info("fake_gpio: buf_count_set() => INVALID, not supported\n");
        return EINVAL;
}

// Open procfs file
static int fake_gpio_open_proc(struct inode *inode, struct file *file)
{
    pr_info("fake_gpio: fake_gpio_open_proc() proc file opened.....\n");
    gpio_proc_read_done = false;
    return 0;
}

// close the procfs file
static int fake_gpio_release_proc(struct inode *inode, struct file *file)
{
    pr_info("fake_gpio: fake_gpio_release_proc(): file released.....\n");
    return 0;
}

// Read the procfs file
static ssize_t fake_gpio_read_proc(struct file *filp, char __user *buffer, size_t length,loff_t * offset)
{
        ssize_t len;
        ssize_t minlen;
        char local_buf[16];
        if (gpio_proc_read_done) return 0;
        len = sprintf(local_buf, "%ld", buf_count);        
        minlen = (length < len?length:len);
        if( copy_to_user(buffer,local_buf,minlen) ) {
            pr_err("fake_gpio: fake_gpio_read_proc(): copy_to_user_error\n");
        }
        pr_info("fake_gpio: fake_gpio_read_proc => %s\n", local_buf);
        gpio_proc_read_done = true;
        return length;
}

// Write the fake_gpio_procfs file
// static ssize_t fake_gpio_write_proc(struct file *filp, const char *buff, size_t len, loff_t * off)
// {
//     pr_info("proc file write - ingored.....\n");
    
//     return 0;
// }

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

        // Create proc directory
        // https://www.cs.cmu.edu/afs/grand.central.org/archive/twiki/pub/Main/SumitKumar/procfs-guide.pdf
        gpio_proc_root = proc_mkdir(PROC_DIR,NULL);
        
        if( gpio_proc_root == NULL )
        {
            pr_err("fake_gpio: fake_gpio_driver_init(): Error creating proc entry /proc/%s/%s", PROC_DIR, PROC_ENTRY_NAME);
            goto destroy_sysfs;
        }
        
        // Create /proc/fake_gpio/buf_count as read-only
        // Compare to create_proc_read_entry
        proc_create(PROC_ENTRY_NAME, 0444, gpio_proc_root, &proc_fops);
        pr_info("fake_gpio: fake_gpio_driver_init(): Created proc entry /proc/%s/%s", PROC_DIR, PROC_ENTRY_NAME);

        // start buffer drain polling thread; only drains if mode is set appropriately
        pr_info("fake_gpio: fake_gpio_driver_init(): Creating drain_poll_loop_thread...\n");
        drain_poll_loop_thread = kthread_create(drain_buffer_poll_thread_fn,NULL,"drain_buffer_poll_thread_fn");
        pr_info("fake_gpio: fake_gpio_driver_init(): waking up thread...\n");
        wake_up_process(drain_poll_loop_thread);

        // start buffer drain wait queue thread; only drains if mode is set appropriately
        pr_info("fake_gpio: fake_gpio_driver_init(): Creating drain_wait_loop_thread...\n");
        drain_wait_loop_thread = kthread_create(drain_buffer_wait_thread_fn,NULL,"drain_buffer_wait_thread_fn");
        pr_info("fake_gpio: fake_gpio_driver_init(): waking up thread...\n");
        wake_up_process(drain_wait_loop_thread);
    
        pr_info("fake_gpio: fake_gpio_driver_init(): inserted device %s, class %s, MAJOR=%d, MINOR=%d sysfs=%s\n",
                 DEV_NAME, DEV_CLASS, MAJOR(dev), MINOR(dev), SYSFS_DIR);
        return 0;
 
// destroy_proc_entry:
//         remove_proc_entry(PROC_ENTRY_NAME, gpio_proc_root); 

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
        if ( drain_poll_loop_thread) {
                pr_info("fake_gpio: fake_gpio_driver_exit() stopping drain_poll_loop_thread...\n");
                kthread_stop(drain_poll_loop_thread);
        }
        if ( drain_wait_loop_thread) {
                pr_info("fake_gpio: fake_gpio_driver_exit() stopping drain_wait_loop_thread...\n");
                kthread_stop(drain_wait_loop_thread);
        }
        
        kobject_put(gpio_kobj); 
        remove_proc_entry(PROC_ENTRY_NAME, gpio_proc_root); 
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
