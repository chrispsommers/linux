#ifndef __KUTIL_H__
#define __KUTIL_H__

#define KLOG_INFO(fmt,...) \
        pr_info("%s: %s()-%d: " fmt, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__ )

#define KLOG_ERR(fmt,...) \
        pr_err("%s: %s()-%d: " fmt, __FILE__, __FUNCTION__, __LINE__, ##__VA_ARGS__ )


// Circular byte buffer
typedef struct s_circ_buf {
        char *data;
        char *head;
        char *tail;
        char *start;
        char *end;
        size_t count;
        size_t size;
} circ_buf_t;

// Initialize and allocate a circular byte buffer
#define KCIRC_BUF_RESET(_bufptr) \
        (_bufptr)->head = (_bufptr)->data; \
        (_bufptr)->tail = (_bufptr)->data; \
        (_bufptr)->start = (_bufptr)->data; \
        (_bufptr)->end= (_bufptr)->data + (_bufptr)->size - 1; \
        (_bufptr)->count = 0;

// Initialize and allocate a circular byte buffer
#define KCIRC_BUF_ALLOC_AND_INIT(_bufptr,_size) \
        (_bufptr)->data = kmalloc(_size, GFP_KERNEL); \
        (_bufptr)->size = _size; \
        KCIRC_BUF_RESET((_bufptr))
        

#define KCIRC_BUF_DEALLOC(_bufptr) \
        if ((_bufptr)->data) kfree((_bufptr)->data)

#define KCIRC_BUF_COUNT(_bufptr) (_bufptr)->count


int kcirc_bufpush_one(circ_buf_t *buf, char data);
size_t kcirc_bufpush(circ_buf_t *buf, const char __user *ubuf, size_t len);
int kcirc_bufpop_one(circ_buf_t *buf, char  *data);
size_t kcirc_bufpop(circ_buf_t *buf, char __user *ubuf, size_t len);
void kcirc_buf_flush(circ_buf_t *buf);

#endif // __KUTIL_H__
