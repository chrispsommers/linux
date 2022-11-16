#include <linux/kernel.h>
#include<linux/uaccess.h>
#include "kutil.h"

// push one byte into buffer
// return 1 if success, 0 if no room available
// no synchronization, caller must ensure e.g. via mutex
int kcirc_bufpush_one(circ_buf_t *buf, char data) {
        if (buf->count < buf->size) {
                buf->count++;
                *buf->head++ = data;
                if (buf->head > buf->end) {
                        buf->head = buf->start; // wraparound
                }
                KLOG_INFO("%s: data = '%c', buf->count=%ld\n", buf->name, data, buf->count);
        } else {
                KLOG_INFO("%s: buffer full (%ld), cannot push\n", buf->name, buf->count);
                return 0;
        }
        // wake_up_interruptible(&gpio_buffer_wq);
        return 1;
}

// push bytes onto circular buffer, does not overwrite
// no synchronization, caller must ensure e.g. via mutex
size_t kcirc_bufpush(circ_buf_t *buf, const char __user *ubuf, size_t len) {
        const char __user *rp;
        int rc;
        char data;
        size_t i;
        rp = ubuf;
        for (i = 0; i < len; i++) {
                // copy userspace to kernel space one byte at a time & push to FIFO
                rc = copy_from_user(&data, rp++, 1);
                if(rc) {
                        KLOG_ERR("%s: copy_from_user(): ERROR %d!\n", buf->name, rc);
                        break;
                }
                if (kcirc_bufpush_one(buf, data) == 0) {
                        KLOG_INFO("%s: buffer full upon writing %ld bytes\n", buf->name, i);
                        break;
                }
        }
        return i;
}

// pop one byte off buffer
// return 0 if empty, 1 if byte was available
// no synchronization, caller must ensure e.g. via mutex
int kcirc_bufpop_one(circ_buf_t *buf, char  *data) {
        if (buf->count == 0) {
                // KLOG_INFO("buffer empty\n");
                return 0;
        }
        *data = *buf->tail;
        buf->count--;
        buf->tail++;
        if (buf->tail > buf->end) {
                buf->tail = buf->start; // wraparound
        }
        KLOG_INFO("%s: data='%c',  buf->count=%ld\n", buf->name, *data, buf->count);
        return 1;
}

// pop bytes off of circular buffer; no underrun allowed
// no synchronization, caller must ensure e.g. via mutex
size_t kcirc_bufpop(circ_buf_t *buf, char __user *ubuf, size_t len) {
        char *wp;
        int rc;
        size_t i;
        char data;
        wp = ubuf;
        KLOG_INFO("%s: kcirc_bufpop(): buf->count= %ld\n", buf->name, buf->count);
        for (i = 0; i < len; i++) {
                if (!kcirc_bufpop_one(buf, &data)) {
                        KLOG_INFO("%s: buffer empty upon reading %ld bytes\n", buf->name, i);
                        return i;
                }
                rc = copy_to_user(wp++, &data, 1);
                if(rc) {
                        KLOG_ERR("%s: copy_to_user(): ERROR %d!\n", buf->name, rc);
                        return i;
                }
        }
        return i;
}

// flush buffer - no mutex protection, caller responsible
void kcirc_buf_flush(circ_buf_t *buf) {
        KLOG_INFO("%s: flushing buffer of %ld bytes\n", buf->name, buf->count);
        KCIRC_BUF_RESET(buf);
}