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
#include <linux/ioctl.h>

#include "kutil.h"
#include "gpio.h"

dev_t byte_dev = 0;
static struct class *byte_dev_class = NULL;
static struct cdev fake_gpio_byte_cdev;
circ_buf_t byte_buf;

// output "bits" buffer
dev_t bit_dev = 0;
static struct class *bit_dev_class = NULL;
static struct cdev fake_gpio_bit_cdev;
circ_buf_t bit_buf;

struct kobject *gpio_kobj = NULL;
static struct task_struct *byte_drain_poll_loop_thread = NULL;
bool byte_drain_poll_loop_thread_running = false;

static struct task_struct *byte_drain_wait_loop_thread = NULL;
bool byte_drain_wait_loop_thread_running = false;

static struct task_struct *bit_drain_wait_loop_thread = NULL;
bool bit_drain_wait_loop_thread_running = false;

DECLARE_WAIT_QUEUE_HEAD(gpio_byte_buffer_wq);
DECLARE_WAIT_QUEUE_HEAD(gpio_bit_buffer_wq);
DEFINE_MUTEX(byte_buf_mutex);
DEFINE_MUTEX(bit_buf_mutex);
static struct proc_dir_entry *gpio_proc_root;
bool proc_read_buf_count_done = false;
bool proc_read_gpio_mode_done = false;


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
                        
struct kobj_attribute gpio_mode_attr = __ATTR(SYSFS_GPIO_MODE_NAME, 0660, gpio_mode_get, gpio_mode_set);
struct kobj_attribute buf_count_attr = __ATTR(SYSFS_BUF_COUNT_NAME, 0660, buf_count_get, buf_count_set);
void byte_drain_buffer_blocking(void);

// File ops callbacks & structure
static int      fake_gpio_byte_open(struct inode *inode, struct file *file);
static int      fake_gpio_byte_release(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_byte_read(struct file *fp, char __user *buf, size_t len,loff_t * off);
static ssize_t  fake_gpio_byte_write(struct file *fp, const char *buf, size_t len, loff_t * off);
static long     fake_gpio_byte_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct file_operations byte_fops =
{
        .owner          = THIS_MODULE,
        .read           = fake_gpio_byte_read,
        .write          = fake_gpio_byte_write,
        .open           = fake_gpio_byte_open,
        .unlocked_ioctl = fake_gpio_byte_ioctl,
        .release        = fake_gpio_byte_release
};

static int      fake_gpio_bit_open(struct inode *inode, struct file *file);
static int      fake_gpio_bit_release(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_bit_read(struct file *fp, char __user *buf, size_t len,loff_t * off);
static ssize_t  fake_gpio_bit_write(struct file *fp, const char *buf, size_t len, loff_t * off);
static long     fake_gpio_bit_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct file_operations bit_fops =
{
        .owner          = THIS_MODULE,
        .read           = fake_gpio_bit_read,
        .write          = fake_gpio_bit_write,
        .open           = fake_gpio_bit_open,
        .unlocked_ioctl = fake_gpio_bit_ioctl,
        .release        = fake_gpio_bit_release
};

// procfs KCIRC_BUF_COUNT(&byte_buf) operation callbacks & structure
static int      fake_gpio_buf_count_open_proc(struct inode *inode, struct file *file);
static int      fake_gpio_buf_count_release_proc(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_buf_count_read_proc(struct file *fp, char __user *buffer, size_t length,loff_t * offset);

static struct proc_ops proc_buf_count_fops = {
        .proc_open = fake_gpio_buf_count_open_proc,
        .proc_read = fake_gpio_buf_count_read_proc,
        .proc_write = NULL, // no writes
        .proc_release = fake_gpio_buf_count_release_proc
}; 

// procfs gpio_mode operation callbacks & structure
static int      fake_gpio_gpio_mode_open_proc(struct inode *inode, struct file *file);
static int      fake_gpio_gpio_mode_release_proc(struct inode *inode, struct file *file);
static ssize_t  fake_gpio_gpio_mode_read_proc(struct file *fp, char __user *buffer, size_t length,loff_t * offset);
static ssize_t  fake_gpio_gpio_mode_write_proc(struct file *fp, const char *buff, size_t len, loff_t * off);

static struct proc_ops proc_gpio_mode_fops = {
        .proc_open = fake_gpio_gpio_mode_open_proc,
        .proc_read = fake_gpio_gpio_mode_read_proc,
        .proc_write = fake_gpio_gpio_mode_write_proc,
        .proc_release = fake_gpio_gpio_mode_release_proc
}; 

//=========================================================================================
//                             BYTE device
//=========================================================================================

// Open byte device file
static int fake_gpio_byte_open(struct inode *inode, struct file *file)
{
        KLOG_INFO("device file /dev/%s opened\n", BYTE_BUF_DEV_NAME);
        return 0;
}

// Close byte device file
static int fake_gpio_byte_release(struct inode *inode, struct file *file)
{
        KLOG_INFO("device file /dev/%s closed\n", BYTE_BUF_DEV_NAME);
        return 0;
}

// Read from byte device file
static ssize_t fake_gpio_byte_read(struct file *fp, char __user *ubuf, size_t len, loff_t *off)
{
        int rc = 0;
        size_t rlen = 0;
        size_t actual_rlen=0;
        rc = mutex_lock_interruptible(&byte_buf_mutex);
        if (rc) {
            KLOG_ERR("mutex interrupted, err = %d\n", rc);
            return rc;
        }

        if (KCIRC_BUF_COUNT(&byte_buf) == 0) {
                KLOG_INFO("buffer empty, nothing to read\n");
                mutex_unlock(&byte_buf_mutex);
                return 0;
        }

        rlen = (len < KCIRC_BUF_COUNT(&byte_buf))?len:KCIRC_BUF_COUNT(&byte_buf);
        KLOG_INFO("request to read %ld bytes, will actually try to read %ld bytes\n", len, rlen);
        // finally pop data from internal FIFO buffer
        actual_rlen = kcirc_bufpop(&byte_buf, ubuf, rlen);
        KLOG_INFO("popped %ld bytes\n", actual_rlen);
        mutex_unlock(&byte_buf_mutex);
        return actual_rlen;
}

// Write to byte device file
static ssize_t fake_gpio_byte_write(struct file *fp, const char __user *ubuf, size_t len, loff_t *off)
{
        size_t wlen;
        size_t actual_wlen = 0;
        int rc = 0;

        KLOG_INFO("request to write %ld bytes\n", len);
        rc = mutex_lock_interruptible(&byte_buf_mutex);
        if (rc) {
            KLOG_ERR("mutex interrupted, err = %d\n", rc);
            return rc;
        }
        if (KCIRC_BUF_COUNT(&byte_buf) == BYTE_BUF_SZ) {
                KLOG_INFO("buffer full, nothing to write\n");
                mutex_unlock(&byte_buf_mutex);
                return ENOSPC;
        }        
        if (len > BYTE_BUF_SZ) {
                wlen = BYTE_BUF_SZ;
                KLOG_INFO("truncating write to %ld bytes\n", wlen);

        } else {
                wlen = len;
                KLOG_INFO("actually writing %ld bytes\n", wlen);
        }
        // finally push data into internal FIFO buffer
        actual_wlen = kcirc_bufpush(&byte_buf, ubuf, wlen);
        mutex_unlock(&byte_buf_mutex);
        if (actual_wlen > 0) {
                wake_up_interruptible(&gpio_byte_buffer_wq);
        }
        KLOG_INFO("pushed %ld bytes\n", actual_wlen);
        if (gpio_mode == MODE_SERIALIZE_BLOCKING) {
                KLOG_INFO("draining buffer (blocking)...\n");
                byte_drain_buffer_blocking();
        }
        return actual_wlen;
}

// byte device ioctl
static long fake_gpio_byte_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        KLOG_INFO("received cmd=%d arg=%ld\n", cmd, arg);

         switch(cmd) {
                case IOCTL_GPIO_BYTES_FLUSH:
                        mutex_lock(&byte_buf_mutex);
                        KLOG_INFO("IOCTL_GPIO_BYTES_FLUSH flushing buffer of %ld bytes...\n", KCIRC_BUF_COUNT(&byte_buf));
                        // byte_buffer_flush();
                        kcirc_buf_flush(&byte_buf);
                        mutex_unlock(&byte_buf_mutex);
                        break;

                case IOCTL_GPIO_BYTES_COUNT:
                        KLOG_INFO("IOCTL_GPIO_BYTES_COUNT read %ld", KCIRC_BUF_COUNT(&byte_buf));
                        if( copy_to_user((size_t*) arg, &(KCIRC_BUF_COUNT(&byte_buf)), sizeof(KCIRC_BUF_COUNT(&byte_buf))) )
                        {
                                KLOG_ERR("ERROR copying to user\n");
                        }
                        break;
                default:
                        KLOG_INFO("Unknown ioctl call %d, IGNORED\n", cmd);
                        return EINVAL;
        }
        return 0;
}

char preamble[2] = {0,1}; // bits represented as bytes for simplicity to change
char postamble[2] = {0,1};

void output_bit(char bit) {
        KLOG_INFO("set_output() %c\n", bit);
        usleep_range(BIT_DURATION_MIN_USEC,BIT_DURATION_MAX_USEC);
}

void queue_bit(bool bit) {
        mutex_lock(&bit_buf_mutex);
        kcirc_bufpush_one(&bit_buf, bit?'1':'0');
        mutex_unlock(&bit_buf_mutex);
        // KLOG_INFO("Waking up gpio_bit_buffer_wq\n");
        wake_up_interruptible(&gpio_bit_buffer_wq);
}

void send_x_amble(char *data, ssize_t len) {
        int i;
        KLOG_INFO("sending framing %ld bits\n", len);
        for (i = 0; i< len; i++) {
                queue_bit(data[i]);
        }
}

// shift bits out LSB first, surrounded by framing
void serialize_byte(char data) {
        int i;
        KLOG_INFO("serialize_byte 0x%02x = '%c'\n", data, data);

        send_x_amble(preamble, sizeof(preamble)/sizeof(preamble[0]));
        KLOG_INFO("sending data 8 bits\n");
        for (i = 0; i < 8; i++) {
                queue_bit(data & 1<<i);
        }
        send_x_amble(postamble, sizeof(postamble)/sizeof(postamble[0]));
}

// send a byte out if available; mutex-protected
int send_one_byte_conditionally(void) {
        char data;
        int rc = 0;
        mutex_lock(&byte_buf_mutex);
        // rc = byte_buffer_pop_one(&data);
        rc = kcirc_bufpop_one(&byte_buf, &data);
        mutex_unlock(&byte_buf_mutex);
        if (rc) {
                serialize_byte(data);
        }
        return rc;
}

// send a bit out if available; mutex-protected
int send_one_bit_conditionally(void) {
        char data;
        int rc = 0;
        mutex_lock(&bit_buf_mutex);
        rc = kcirc_bufpop_one(&bit_buf, &data);
        mutex_unlock(&bit_buf_mutex);
        if (rc) {
                output_bit(data);
        }
        return rc;
}
// Drain the byte buffer & serialize it to FAKE I2C bus - blocking
// https://www.kernel.org/doc/html/latest/timers/timers-howto.html
void byte_drain_buffer_blocking(void) {
        // char data;
        int rc = 1;
        while (rc) {
                rc = send_one_byte_conditionally();
        }
}

/*
 * Continuously poll byte buffer in a thread
 * Drain a byte at a time if gpio_mode == MODE_SERIALIZE_NONBLOCKING_POLLED
 */
int byte_drain_buffer_poll_thread_fn(void *context) {
        // char data;
        int rc = 0;
        KLOG_INFO("entered thread\n");
        allow_signal(SIGKILL);
        byte_drain_poll_loop_thread_running = true;
        while(!kthread_should_stop()) {
                if (gpio_mode == MODE_SERIALIZE_NONBLOCKING_POLLED) {
                        rc = send_one_byte_conditionally();
                }
                if (signal_pending(byte_drain_poll_loop_thread)) // catch sigkill
                        break;

                usleep_range(WAIT_DLY_MIN_USEC,WAIT_DLY_MAX_USEC);
        }
        byte_drain_poll_loop_thread_running = false;
        KLOG_INFO("leaving thread\n");
        return 0;
}

/*
 * Byte buffer drain thread, wait on mode change, buffer change, signal
 */
int byte_drain_buffer_wait_thread_fn(void *context) {
        gpio_mode_t old_mode = 0;
        int rc = 0;
        KLOG_INFO("entered thread\n");
        allow_signal(SIGKILL);
        byte_drain_wait_loop_thread_running = true;
        while(!kthread_should_stop()) {
                KLOG_INFO("Waiting ...\n");
                // Wake up on sigkill, or wait mode && (data to send or mode changed)
                wait_event_interruptible(gpio_byte_buffer_wq,
                                                (kthread_should_stop() ||
                                                ( (gpio_mode == MODE_SERIALIZE_NONBLOCKING_WAITQ) &&
                                                  ( (KCIRC_BUF_COUNT(&byte_buf) > 0) || (gpio_mode != old_mode))
                                                )
                                        ) );
                KLOG_INFO("woke up!\n");
                old_mode = gpio_mode;
                if(gpio_mode == MODE_SERIALIZE_NONBLOCKING_WAITQ) {
                        rc = send_one_byte_conditionally();
                }
                if (signal_pending(byte_drain_wait_loop_thread)) // catch sigkill
                        break;

                usleep_range(WAIT_DLY_MIN_USEC,WAIT_DLY_MAX_USEC);
        }
        byte_drain_wait_loop_thread_running = false;
        KLOG_INFO("leaving thread\n");
        return 0;
}

/*
 * Bit buffer thread, wait on buffer change, signal
 */
int bit_drain_buffer_wait_thread_fn(void *context) {
        int rc = 0;
        KLOG_INFO("entered thread\n");
        allow_signal(SIGKILL);
        bit_drain_wait_loop_thread_running = true;
        while(!kthread_should_stop()) {
                KLOG_INFO("Waiting ...\n");
                // Wake up on sigkill, or  data to send
                wait_event_interruptible(gpio_bit_buffer_wq,
                                                (kthread_should_stop() || (KCIRC_BUF_COUNT(&bit_buf) > 0) )
                                        );
                KLOG_INFO("woke up!\n");
                rc = send_one_bit_conditionally();
                if (signal_pending(bit_drain_wait_loop_thread)) // catch sigkill
                        break;

                usleep_range(WAIT_DLY_MIN_USEC,WAIT_DLY_MAX_USEC);
        }
        bit_drain_wait_loop_thread_running = false;
        KLOG_INFO("leaving thread\n");
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
        pr_info(" => '%s'\n", buf);
        return len;
}

/*
 * /sys/kernel/SYSFS_DIR/gpio_mode write op
 */
static ssize_t gpio_mode_set(struct kobject *kobj, 
                struct kobj_attribute *attr,const char *buf, size_t count)
{
        ssize_t len;
        gpio_mode_t gpio_mode_tmp;
        len = sscanf(buf,"%d",(int *)(&gpio_mode_tmp));
        if (len != 1) {
                KLOG_ERR("Bad gpio_mode '%s', not an integer\n", buf);
                return EINVAL;
        }
        if (!VALID_MODE(gpio_mode_tmp)) {
                KLOG_ERR("Bad gpio_mode %d, out of range\n", gpio_mode_tmp);
                return EINVAL;
        }
        gpio_mode = gpio_mode_tmp;
        KLOG_INFO(" => '%d'\n", gpio_mode);
        if ((gpio_mode == MODE_SERIALIZE_NONBLOCKING_WAITQ) && byte_drain_wait_loop_thread) {
                wake_up_interruptible(&gpio_byte_buffer_wq);
        }
        return count;
}

/*
 * /sys/kernel/SYSFS_DIR/KCIRC_BUF_COUNT(&byte_buf) read op
 */
static ssize_t buf_count_get(struct kobject *kobj, 
                struct kobj_attribute *attr, char *buf)
{
        ssize_t len;
        len = sprintf(buf, "%ld", KCIRC_BUF_COUNT(&byte_buf));
        // pr_info("buf_count_get() => '%s'\n", buf);
        return len;
}

// /sys/kernel/SYSFS_DIR/KCIRC_BUF_COUNT(&byte_buf) write op
static ssize_t buf_count_set(struct kobject *kobj, 
                struct kobj_attribute *attr,const char *buf, size_t count)
{
        KLOG_INFO(" => INVALID, not supported\n");
        return EINVAL;
}

//================================================
//======== /proc/fake_gpio_procfs/KCIRC_BUF_COUNT(&byte_buf)
//================================================

// Open procfs file
static int fake_gpio_buf_count_open_proc(struct inode *inode, struct file *file)
{
    KLOG_INFO("/proc/%s/%s opened", PROC_DIR, PROC_BUF_COUNT_NAME);
    proc_read_buf_count_done = false;
    return 0;
}

// close the procfs file
static int fake_gpio_buf_count_release_proc(struct inode *inode, struct file *file)
{
    KLOG_INFO("/proc/%s/%s released", PROC_DIR, PROC_BUF_COUNT_NAME);
    return 0;
}

// Read the procfs file
static ssize_t fake_gpio_buf_count_read_proc(struct file *fp, char __user *buffer, size_t length,loff_t * offset)
{
        ssize_t len;
        ssize_t minlen;
        char local_buf[16];
        if (proc_read_buf_count_done) return 0;
        // len = sprintf(local_buf, "%ld", KCIRC_BUF_COUNT(&byte_buf));        
        len = sprintf(local_buf, "%ld", byte_buf.count);        
        minlen = (length < len?length:len);
        if( copy_to_user(buffer,local_buf,minlen) ) {
            KLOG_ERR("copy_to_user_error\n");
        }
        KLOG_INFO("read => '%s'\n", local_buf);
        proc_read_buf_count_done = true;
        return minlen;
}

//================================================
//======== /proc/fake_gpio_procfs/gpio_mode
//================================================
// Open procfs file
static int fake_gpio_gpio_mode_open_proc(struct inode *inode, struct file *file)
{
        KLOG_INFO("/proc/%s/%s opened", PROC_DIR, PROC_GPIO_MODE_NAME);
        proc_read_gpio_mode_done = false;
        return 0;
}

// close the procfs file
static int fake_gpio_gpio_mode_release_proc(struct inode *inode, struct file *file)
{
        KLOG_INFO("/proc/%s/%s released", PROC_DIR, PROC_GPIO_MODE_NAME);
        return 0;
}

// Read the procfs file
static ssize_t fake_gpio_gpio_mode_read_proc(struct file *fp, char __user *buffer, size_t length,loff_t * offset)
{
        ssize_t len;
        ssize_t minlen;
        char local_buf[16];
        if (proc_read_gpio_mode_done) return 0;
        len = sprintf(local_buf, "%d", gpio_mode);        
        minlen = (length < len?length:len);
        if( copy_to_user(buffer,local_buf,minlen) ) {
            KLOG_ERR("copy_to_user_error\n");
        }
        KLOG_INFO("gpio_mode read => '%s'\n", local_buf);
        proc_read_gpio_mode_done = true;
        return minlen;
}

// Write the fake_gpio_procfs file
static ssize_t fake_gpio_gpio_mode_write_proc(struct file *fp, const char *buff, size_t len, loff_t * off)
{
        char local_buf[16];
        gpio_mode_t gpio_mode_tmp;
        if (copy_from_user(local_buf,buff,len) ) {
            KLOG_ERR("copy_to_user_error\n");
            return 0;
        }

        len = sscanf(local_buf,"%d",(int *)(&gpio_mode_tmp));
        if (len != 1) {
                KLOG_ERR("Bad gpio_mode '%s', not an integer\n", local_buf);
                return EINVAL;
        }
        if (!VALID_MODE(gpio_mode_tmp)) {
                KLOG_ERR("Bad gpio_mode %d, out of range\n", gpio_mode_tmp);
                return EINVAL;
        }
        gpio_mode = gpio_mode_tmp;
        KLOG_INFO(" => '%d'\n", gpio_mode);

        KLOG_INFO("gpio_mode write => '%d'\n", gpio_mode);
        return len;
}


//=========================================================================================
//                             BIT SERIAL device
//=========================================================================================

// Open bit device file
static int fake_gpio_bit_open(struct inode *inode, struct file *file)
{
        KLOG_INFO("device file /dev/%s opened\n", BIT_BUF_DEV_NAME);
        return 0;
}

// Close bit device file
static int fake_gpio_bit_release(struct inode *inode, struct file *file)
{
        KLOG_INFO("device file /dev/%s closed\n", BIT_BUF_DEV_NAME);
        return 0;
}


// Read from bit device file
static ssize_t fake_gpio_bit_read(struct file *fp, char __user *ubuf, size_t len, loff_t *off)
{
        int rc = 0;
        size_t rlen = 0;
        size_t actual_rlen=0;
        rc = mutex_lock_interruptible(&bit_buf_mutex);
        if (rc) {
            KLOG_ERR("mutex interrupted, err = %d\n", rc);
            return rc;
        }

        if (KCIRC_BUF_COUNT(&bit_buf) == 0) {
                KLOG_INFO("buffer empty, nothing to read\n");
                mutex_unlock(&bit_buf_mutex);
                return 0;
        }

        rlen = (len < KCIRC_BUF_COUNT(&bit_buf))?len:KCIRC_BUF_COUNT(&bit_buf);
        KLOG_INFO("request to read %ld chars, will actually try to read %ld chars\n", len, rlen);
        // finally pop data from internal FIFO buffer
        actual_rlen = kcirc_bufpop(&bit_buf, ubuf, rlen);
        KLOG_INFO("popped %ld bits\n", actual_rlen);
        mutex_unlock(&bit_buf_mutex);
        return actual_rlen;
}

// Write to bit device file
static ssize_t fake_gpio_bit_write(struct file *fp, const char __user *ubuf, size_t len, loff_t *off)
{
        size_t wlen;
        size_t actual_wlen = 0;
        int rc = 0;

        KLOG_INFO("request to write %ld chars\n", len);
        rc = mutex_lock_interruptible(&bit_buf_mutex);
        if (rc) {
            KLOG_ERR("mutex interrupted, err = %d\n", rc);
            return rc;
        }
        if (KCIRC_BUF_COUNT(&bit_buf) == BIT_BUF_SZ) {
                KLOG_INFO("buffer full, nothing to write\n");
                mutex_unlock(&bit_buf_mutex);
                return ENOSPC;
        }        
        if (len > BIT_BUF_SZ) {
                wlen = BIT_BUF_SZ;
                KLOG_INFO("truncating write to %ld chars\n", wlen);

        } else {
                wlen = len;
                KLOG_INFO("actually writing %ld chars\n", wlen);
        }
        // finally push data into internal FIFO buffer
        actual_wlen = kcirc_bufpush(&bit_buf, ubuf, wlen);
        mutex_unlock(&bit_buf_mutex);
        if (actual_wlen > 0) {
                // KLOG_INFO("Waking up gpio_bit_buffer_wq\n");
                wake_up_interruptible(&gpio_bit_buffer_wq);
        }
        KLOG_INFO("pushed %ld chars\n", actual_wlen);
        return actual_wlen;
}

// bit device ioctl
static long fake_gpio_bit_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
        KLOG_INFO("received cmd=%d arg=%ld\n", cmd, arg);

         switch(cmd) {
                case IOCTL_GPIO_BITS_FLUSH:
                        mutex_lock(&bit_buf_mutex);
                        KLOG_INFO("IOCTL_GPIO_BITS_FLUSH flushing buffer of %ld bits...\n", KCIRC_BUF_COUNT(&bit_buf));
                        // bit_buffer_flush();
                        kcirc_buf_flush(&bit_buf);
                        mutex_unlock(&bit_buf_mutex);
                        break;

                case IOCTL_GPIO_BITS_COUNT:
                        KLOG_INFO("IOCTL_GPIO_BITS_COUNT read %ld", KCIRC_BUF_COUNT(&bit_buf));
                        if( copy_to_user((size_t*) arg, &(KCIRC_BUF_COUNT(&bit_buf)), sizeof(KCIRC_BUF_COUNT(&bit_buf))) )
                        {
                                KLOG_ERR("ERROR copying to user\n");
                        }
                        break;
                default:
                        KLOG_INFO("Unknown ioctl call %d, IGNORED\n", cmd);
                        return EINVAL;
        }
        return 0;
}




//=========================================================================================
//                             MODULE INSERT/REMOVE
//=========================================================================================

static int __init fake_gpio_driver_init(void)
{
        //============ byte-buffer device ===============
        struct proc_dir_entry *proc_entry;

        // Get dynamic Major char byte_dev number
        if((alloc_chrdev_region(&byte_dev, 0, 1, BYTE_BUF_DEV_NAME)) <0){
                KLOG_ERR("Cannot allocate major number for %s\n", BYTE_BUF_DEV_NAME);
                return -1;
        }
        KLOG_INFO("Created chrdev_region %d:%d for %s", MAJOR(byte_dev), MINOR(byte_dev), BYTE_BUF_DEV_NAME);
 
        // Create char byte_dev struct
        cdev_init(&fake_gpio_byte_cdev,&byte_fops);
         if((cdev_add(&fake_gpio_byte_cdev,byte_dev,1)) < 0){
            KLOG_ERR("Cannot add the %s device to the system\n", BYTE_BUF_DEV_NAME);
            goto destroy_chrdev_region1;
        }
        KLOG_INFO("Created cdev struct for device %s\n", BYTE_BUF_DEV_NAME);
 
        // Create class
        if(IS_ERR(byte_dev_class = class_create(THIS_MODULE,BYTE_DEV_CLASS))){
            KLOG_ERR("Cannot create the struct class\n");
            goto destroy_chrdev_region1;
        }
         KLOG_INFO("Created class %s for device %s\n", BYTE_DEV_CLASS, BYTE_BUF_DEV_NAME);

        // Finally, create the device
        if(IS_ERR(device_create(byte_dev_class,NULL,byte_dev,NULL, BYTE_BUF_DEV_NAME))){
            KLOG_ERR("Cannot create the Device 1\n");
            goto destroy_class1;
        }
        KLOG_INFO("Created device %s\n", BYTE_BUF_DEV_NAME);
       
        KCIRC_BUF_ALLOC_AND_INIT(&byte_buf, BYTE_BUF_SZ, "byte_buf");
        // Create sysfs directory in /sys/kernel/
        gpio_kobj = kobject_create_and_add(SYSFS_DIR,kernel_kobj);
 
        // Create sysfs file node for gpio_mode
        if(sysfs_create_file(gpio_kobj,&gpio_mode_attr.attr)){
                pr_err("Cannot create sysfs/gpio_mode file......\n");
                goto destroy_sysfs;
        }
 
        // Create sysfs file node byte_buf
        if(sysfs_create_file(gpio_kobj,&buf_count_attr.attr)){
                pr_err("Cannot create sysfs/buf_count file......\n");
                goto destroy_sysfs;
        }

        // Create proc directory
        // https://www.cs.cmu.edu/afs/grand.central.org/archive/twiki/pub/Main/SumitKumar/procfs-guide.pdf
        // https://github.com/torvalds/linux/blob/master/include/linux/proc_fs.h
        gpio_proc_root = proc_mkdir(PROC_DIR,NULL);
        
        if( gpio_proc_root == NULL )
        {
            KLOG_ERR("Error creating proc directory /proc/%s/", PROC_DIR);
            goto destroy_sysfs;
        }
        
        // Create /proc/fake_gpio/buf_count as read-only
        proc_entry = proc_create(PROC_BUF_COUNT_NAME, 0444, gpio_proc_root, &proc_buf_count_fops);
        if (!proc_entry) {
            KLOG_ERR("Error creating proc entry /proc/%s/%s", PROC_DIR, PROC_BUF_COUNT_NAME);
            goto destroy_proc_entry;
        }
        KLOG_INFO("Created proc entry /proc/%s/%s", PROC_DIR, PROC_BUF_COUNT_NAME);
        
        // Create /proc/fake_gpio/gpio_mode as read-write
        proc_entry = proc_create(PROC_GPIO_MODE_NAME, 0666, gpio_proc_root, &proc_gpio_mode_fops);
        if (!proc_entry) {
            KLOG_ERR("Error creating proc entry /proc/%s/%s", PROC_DIR, PROC_GPIO_MODE_NAME);
            goto destroy_proc_entry;
        }
        KLOG_INFO("Created proc entry /proc/%s/%s", PROC_DIR, PROC_GPIO_MODE_NAME);

        //============ bit-buffer device ===============

        // Get dynamic Major char bit_dev number
        if((alloc_chrdev_region(&bit_dev, 0, 1, BIT_BUF_DEV_NAME)) <0){
                KLOG_ERR("Cannot allocate major number for %s\n", BIT_BUF_DEV_NAME);
                goto destroy_proc_entry;
        }
         KLOG_INFO("Created chrdev_region %d:%d for %s", MAJOR(bit_dev), MINOR(bit_dev), BIT_BUF_DEV_NAME);

        // Create char bit_dev struct
        cdev_init(&fake_gpio_bit_cdev,&bit_fops);
         if((cdev_add(&fake_gpio_bit_cdev,bit_dev,1)) < 0){
            KLOG_ERR("Cannot add the %s device to the system\n", BIT_BUF_DEV_NAME);
            goto destroy_chrdev_region2;
        }
         KLOG_INFO("Created cdev struct for device %s\n", BIT_BUF_DEV_NAME);

        // Create class
        if(IS_ERR(bit_dev_class = class_create(THIS_MODULE,BIT_DEV_CLASS))){
            KLOG_ERR("Cannot create the struct class\n");
            goto destroy_chrdev_region2;
        }
          KLOG_INFO("Created class %s for device %s\n", BIT_DEV_CLASS, BIT_BUF_DEV_NAME);

        // Finally, create the device
        if(IS_ERR(device_create(bit_dev_class,NULL,bit_dev,NULL, BIT_BUF_DEV_NAME))){
            KLOG_ERR("Cannot create the Device 1\n");
            goto destroy_class2;
        }
        KLOG_INFO("Created device %s\n", BYTE_BUF_DEV_NAME);
        
        // Alloc buffer for gpio
        KCIRC_BUF_ALLOC_AND_INIT(&bit_buf, BIT_BUF_SZ, "bit_buf");

        //================= START THREADS ======================
        // start byte buffer drain polling thread; only drains if mode is set appropriately
        KLOG_INFO("Creating byte_drain_poll_loop_thread...\n");
        byte_drain_poll_loop_thread = kthread_create(byte_drain_buffer_poll_thread_fn,NULL,"byte_drain_buffer_poll_thread_fn");
        KLOG_INFO("waking up byte_drain_poll_loop_thread...\n");
        wake_up_process(byte_drain_poll_loop_thread);

        // start byte buffer drain wait queue thread
        KLOG_INFO("Creating byte_drain_wait_loop_thread...\n");
        byte_drain_wait_loop_thread = kthread_create(byte_drain_buffer_wait_thread_fn,NULL,"byte_drain_buffer_wait_thread_fn");
        KLOG_INFO("waking up byte_drain_wait_loop_thread...\n");
        wake_up_process(byte_drain_wait_loop_thread);

        // start bit buffer drain wait queue thread
        KLOG_INFO("Creating bit_drain_wait_loop_thread...\n");
        bit_drain_wait_loop_thread = kthread_create(bit_drain_buffer_wait_thread_fn,NULL,"bit_drain_buffer_wait_thread_fn");
        KLOG_INFO("waking up bit_drain_wait_loop_thread...\n");
        wake_up_process(bit_drain_wait_loop_thread);


        KLOG_INFO("Finished installing module!\n");
        return 0;

 
destroy_class2:
        KLOG_INFO("Removing class %s ... \n", BIT_DEV_CLASS);
        class_destroy(bit_dev_class);

destroy_chrdev_region2:
        KLOG_INFO("Unregistering device %s ... \n", BIT_BUF_DEV_NAME);
        unregister_chrdev_region(bit_dev,1);

destroy_proc_entry:
        KLOG_INFO("Removing /proc/%s ... \n", PROC_DIR);
        remove_proc_entry(PROC_BUF_COUNT_NAME, gpio_proc_root); 

destroy_sysfs:
        KLOG_INFO("Removing /sys/kernel/%s ... \n", SYSFS_DIR);
        kobject_put(gpio_kobj); 
        sysfs_remove_file(kernel_kobj, &gpio_mode_attr.attr);
        sysfs_remove_file(kernel_kobj, &buf_count_attr.attr);

destroy_class1:
        KLOG_INFO("Removing class %s ... \n", BYTE_DEV_CLASS);
        class_destroy(byte_dev_class);

destroy_chrdev_region1:
        KLOG_INFO("Unregistering device %s ... \n", BYTE_BUF_DEV_NAME);
        unregister_chrdev_region(byte_dev,1);

        return -1;
}

/*
 * Module remove
 */
static void __exit fake_gpio_driver_exit(void)
{
        if ( byte_drain_poll_loop_thread) {
                KLOG_INFO("stopping byte_drain_poll_loop_thread...\n");
                kthread_stop(byte_drain_poll_loop_thread);
        }
        if ( byte_drain_wait_loop_thread) {
                KLOG_INFO("stopping byte_drain_wait_loop_thread...\n");
                kthread_stop(byte_drain_wait_loop_thread);
        }
        
        KLOG_INFO("Removing /sys/kernel/%s ... \n", SYSFS_DIR);
        kobject_put(gpio_kobj); 
        KLOG_INFO("Removing /proc/%s ... \n", PROC_DIR);
        proc_remove(gpio_proc_root);
        sysfs_remove_file(kernel_kobj, &gpio_mode_attr.attr);

        KCIRC_BUF_DEALLOC(&byte_buf);
        device_destroy(byte_dev_class,byte_dev);
        KLOG_INFO("Removing class %s ... \n", BYTE_DEV_CLASS);
        class_destroy(byte_dev_class);
        cdev_del(&fake_gpio_byte_cdev);

        KCIRC_BUF_DEALLOC(&bit_buf);
        device_destroy(bit_dev_class,bit_dev);
        KLOG_INFO("Removing class %s ... \n", BIT_DEV_CLASS);
        class_destroy(bit_dev_class);
        cdev_del(&fake_gpio_bit_cdev);

        KLOG_INFO("device driver removed\n");
}
 
module_init(fake_gpio_driver_init);
module_exit(fake_gpio_driver_exit);
 
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Chris Sommers");
MODULE_DESCRIPTION("Fake GPIO driver");
MODULE_VERSION("1.0");
