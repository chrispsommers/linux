import pytest
import os, subprocess
import ctypes
import fcntl
import ioctl_opt # https://github.com/vpelletier/python-ioctl-opt

kmodule='fake_gpio'
devname='fake_gpio'
sysfsname='fake_gpio_sysfs'
procfsname='fake_gpio_procfs'
dev = '/dev/%s' % devname
sysfs = '/sys/kernel/%s' % sysfsname
sysfs_buf_count = '%s/buf_count' % sysfs
sysfs_gpio_mode = '%s/gpio_mode' % sysfs
procfs = '/proc/%s' % procfsname
procfs_buf_count = '%s/buf_count' % procfs
procfs_gpio_mode = '%s/gpio_mode' % procfs

# absolute path to build kernel module
modpath = os.path.realpath(os.path.dirname(__file__))+'/../%s.ko' % kmodule
print ('modpath=',modpath)

#define IOCTL_GPIO_BYTES_FLUSH _IOW('g','F',int32_t *)
_IOCTL_GPIO_BYTES_FLUSH = ioctl_opt.IOW(ord('g'), ord('F'), ctypes.c_void_p)

#define IOCTL_GPIO_BYTES_COUNT _IOR('g','B',size_t *)
_IOCTL_GPIO_BYTES_COUNT = ioctl_opt.IOR(ord('g'), ord('B'), ctypes.c_void_p)

# =================== HELPERS ======================
def call_os_cmd(cmd):
    print ("  Calling '%s' ..." % cmd)
    return subprocess.check_output(cmd.split(' '), universal_newlines=True)

def read_sysfs_buf_count():
    return int(call_os_cmd('cat %s' % sysfs_buf_count))

def read_sysfs_gpio_mode():
    return int(call_os_cmd('cat %s' % sysfs_gpio_mode))

def read_procfs_buf_count():
    return int(call_os_cmd('cat %s' % procfs_buf_count))

def read_procfs_gpio_mode():
    return int(call_os_cmd('cat %s' % procfs_gpio_mode))

def ioctl_cmd(fo, cmd, value):
        v = eval(cmd)
        print('  calling IOCTL(%s=%d) ...' % (cmd, v))
        rc = fcntl.ioctl(fo, eval(cmd), value, True)
        assert (rc == 0)
        # print ('  value=', value)
        return value.value

def read_ioctl_buf_count(fo = None):
    '''
    IOCTL read buffer count
    if fo is None, open file on demand as R-O then close  
    '''
    buf_count = ctypes.c_size_t()
    if fo:
        return ioctl_cmd(fo, '_IOCTL_GPIO_BYTES_COUNT', buf_count)
    else:
        fd = os.open(dev, os.O_WRONLY)
        assert (fd is not None)
        with os.fdopen(fd, 'w') as fo:
            assert(fo is not None)
            return ioctl_cmd(fo, '_IOCTL_GPIO_BYTES_COUNT', buf_count)

def write_ioctl_gpio_flush(fo = None):
    '''
    IOCTL read buffer count
    if fo is None, open file on demand as W-O then close  
    '''
    dummy_int_arg = ctypes.c_int32()
    if fo:
        return ioctl_cmd(fo, '_IOCTL_GPIO_BYTES_FLUSH', dummy_int_arg)
    else:
        fd = os.open(dev, os.O_WRONLY)
        assert (fd is not None)
        with os.fdopen(fd, 'w') as fo:
            assert(fo is not None)
            return ioctl_cmd(fo, '_IOCTL_GPIO_BYTES_FLUSH', dummy_int_arg)

def write_gpio_buffer(data):
    fd = os.open(dev, os.O_WRONLY)
    assert (fd is not None)

    # print('Verify getting file descriptor for %s' % dev)
    with os.fdopen(fd, 'w') as fo:
        assert(fo is not None)
        data_len = len(data)
        print("  Write '%s' to device (%d chars)" % (data, data_len))
        return fo.write(data)


def read_gpio_buffer():
    fd = os.open(dev, os.O_RDONLY)
    assert (fd is not None)

    # print('Verify getting file descriptor for %s' % dev)
    with os.fdopen(fd, 'r') as fo:
        assert(fo is not None)
        result = fo.read()
        print ("  Read %d bytes: '%s'" % (len(result), result))
        return result

def assert_buf_count_multi(expected):

    print('Verify buf_count == %d via sysfs' % expected)
    assert(read_sysfs_buf_count() == expected)

    print('Verify buf_count == %d via procfs' % expected)
    assert(read_procfs_buf_count() == expected)

    print('Verify buf_count == %d via ioctl' % expected)
    assert (read_ioctl_buf_count() == expected)

def assert_gpio_mode_multi(expected):
    print('Verify gpio mode == %d via sysfs' % expected)
    assert(read_sysfs_gpio_mode() == expected)

    print('Verify gpio mode == %d via procfs' % expected)
    assert(read_procfs_gpio_mode() == expected)    

# =================== TESTS ======================

def test_initial_conditions():
    buf_count = ctypes.c_size_t()

    print("Checking existence of", dev)
    assert(os.path.exists(dev))

    print("Checking existence of", sysfs)
    assert(os.path.exists(sysfs))

    print("Checking existence of", sysfs_buf_count)
    assert(os.path.exists(sysfs_buf_count))

    print("Checking existence of", sysfs_gpio_mode)
    assert(os.path.exists(sysfs_gpio_mode))

    print("Checking existence of", procfs)
    assert(os.path.exists(procfs))

    print("Checking existence of", procfs_buf_count)
    assert(os.path.exists(procfs_buf_count))

    print("Checking existence of", procfs_gpio_mode)
    assert(os.path.exists(procfs_gpio_mode))

    assert_gpio_mode_multi(0)
    assert_buf_count_multi(0)

def test_mode_0():
    # Ensure clean starting condition
    print('Flush buffer using IOCTL_GPIO_BYTES_FLUSH')
    assert (write_ioctl_gpio_flush() == 0)
    assert (read_ioctl_buf_count() == 0)

    data='12345'
    data_len = len(data)
    print ("Write data then flush")
    assert(write_gpio_buffer(data) == data_len)
    assert_buf_count_multi(data_len)

    print('Flush buffer using IOCTL_GPIO_BYTES_FLUSH')
    assert (write_ioctl_gpio_flush() == 0)
    assert_buf_count_multi(0)

    # write data, read back
    print ("Write data")
    assert(write_gpio_buffer(data) == data_len)
    assert_buf_count_multi(data_len)

    print ("Verify read-back data")
    assert(read_gpio_buffer() == data)
    assert_buf_count_multi(0)
