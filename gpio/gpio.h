#ifndef __GPIO_H__
#define __GPIO_H__

#define IOCTL_GPIO_BYTES_FLUSH _IOW('g','F',int32_t *)
#define IOCTL_GPIO_BYTES_COUNT _IOR('g','B',size_t *)

#define BYTE_BUF_SZ        10
#define BYTE_DEV_CLASS "fake_gpio_byte_class"
#define BYTE_BUF_DEV_NAME "fake_gpio"
#define SYSFS_DIR "fake_gpio_sysfs"
#define SYSFS_BUF_COUNT_NAME buf_count
#define SYSFS_GPIO_MODE_NAME gpio_mode
#define PROC_DIR "fake_gpio_procfs"
#define PROC_BUF_COUNT_NAME "buf_count"
#define PROC_GPIO_MODE_NAME "gpio_mode"

#define BIT_BUF_SZ        (12*BYTE_BUF_SZ) // byte + preamable
#define BIT_DEV_CLASS "fake_gpio_bit_class"
#define BIT_BUF_DEV_NAME "fake_gpio_bits"

typedef enum gpio_mode_e {
        MODE_FIFO_ONLY = 0,                             // can read and write buffer, no serialzation
        MODE_SERIALIZE_BLOCKING =1,                     // writes cause draining & serialization; blocking
        MODE_SERIALIZE_NONBLOCKING_POLLED = 2,          // writes cause draining & serialization; non-blocking via polling thread
        MODE_SERIALIZE_NONBLOCKING_WAITQ = 3,            // writes cause draining & serialization; non-blocking via wait queue
        MODE_MAX_VALUEPLUSONE
} gpio_mode_t;

#define VALID_MODE(_x) ((_x >= 0) && (_x < MODE_MAX_VALUEPLUSONE))

#endif // __GPIO_H__