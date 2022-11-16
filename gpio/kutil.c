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
                KLOG_INFO("data = '%c', buf->count=%ld, buf->head=%p\n", data, buf->count, buf->head);
        } else {
                KLOG_INFO("buffer full (%ld), cannot push\n", buf->count);
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
                        KLOG_ERR("copy_from_user(): ERROR %d!\n", rc);
                        break;
                }
                if (kcirc_bufpush_one(buf, data) == 0) {
                        KLOG_INFO("buffer full upon writing %ld bytes\n", i);
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
        KLOG_INFO("data='%c',  buf->count=%ld, buf->tail=%p\n", *data, buf->count, buf->tail);
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
        KLOG_INFO("kcirc_bufpop(): buf->count= %ld\n", buf->count);
        for (i = 0; i < len; i++) {
                if (!kcirc_bufpop_one(buf, &data)) {
                        KLOG_INFO("buffer empty upon reading %ld bytes\n", i);
                        return i;
                }
                rc = copy_to_user(wp++, &data, 1);
                if(rc) {
                        KLOG_ERR("copy_to_user(): ERROR %d!\n", rc);
                        return i;
                }
        }
        return i;
}

// flush buffer - no mutex protection, caller responsible
void kcirc_buf_flush(circ_buf_t *buf) {
        KCIRC_BUF_RESET(buf);
}