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

modpath = os.path.realpath(os.path.dirname(__file__))+'/../%s.ko' % kmodule
print ('modpath=',modpath)

#define IOCTL_GPIO_FLUSH _IOW('g','f',int32_t *)
_IOCTL_GPIO_FLUSH = ioctl_opt.IOW(ord('g'), ord('f'), ctypes.c_void_p)

#define IOCTL_GPIO_COUNT _IOR('g','b',size_t *)
_IOCTL_GPIO_COUNT = ioctl_opt.IOR(ord('g'), ord('b'), ctypes.c_void_p)


def call_os_cmd(cmd_string):
    return subprocess.check_output(cmd_string.split(' '), universal_newlines=True)

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

    print('Verify default gpio mode reading %s == 0' % sysfs_gpio_mode)
    result = call_os_cmd('cat %s' % sysfs_gpio_mode)
    assert(result=='0')

    print('Verify buf_count reading %s == 0' % sysfs_buf_count)
    result = call_os_cmd('cat %s' % sysfs_buf_count)
    assert(result=='0')

    print('Verify default gpio mode reading %s == 0' % procfs_gpio_mode)
    result = call_os_cmd('cat %s' % procfs_gpio_mode)
    assert(result=='0')

    print('Verify buf_count reading %s == 0' % procfs_buf_count)
    result = call_os_cmd('cat %s' % procfs_buf_count)
    assert(result=='0')

    # device file operations
    print('Verify opening %s' % dev)
    fd = os.open(dev, os.O_RDWR)
    assert (fd is not None)

    print('Verify getting file descriptor for %s' % dev)
    fo = os.fdopen(fd)
    assert(fo is not None)

    print('Verify ioctl read of buf_count == 0')
    rc = fcntl.ioctl(fo, _IOCTL_GPIO_COUNT, buf_count, True)
    assert (rc == 0)
    print ('buf_count=', buf_count)
    assert (buf_count.value == 0)

    print('Verify closing %s' % dev)
    rc = fo.close()
    assert (rc is None)

# @pytest.mark.skip("WIP")
def test_mode_0():
    buf_count = ctypes.c_size_t()
    dummy_int_arg = ctypes.c_int32()

    # device file operations
    print('Verify opening %s' % dev)
    fd = os.open(dev, os.O_RDWR)
    assert (fd is not None)

    print('Verify getting file descriptor for %s' % dev)
    with os.fdopen(fd, 'w') as fo:
        assert(fo is not None)

        print('Verify ioctl read of buf_count == 0')
        rc = fcntl.ioctl(fo, _IOCTL_GPIO_COUNT, buf_count, True)
        assert (rc == 0)
        print ('buf_count=', buf_count)
        assert (buf_count.value == 0)

        val='12345'
        l = len(val)
        print("Write '%s' to device (%d chars)" % (val, l))
        rc = fo.write(val)
        assert (rc == l)

    fd = os.open(dev, os.O_RDWR)
    assert (fd is not None)
    with os.fdopen(fd, 'w') as fo:
        assert(fo is not None)
        print('Verify ioctl read of buf_count == %d' % l)
        rc = fcntl.ioctl(fo, _IOCTL_GPIO_COUNT, buf_count, True)
        print ('buf_count=', buf_count)
        assert (rc == 0)
        assert (buf_count.value == l)

    print('Verify buf_count reading %s == %d' % (procfs_buf_count, l))
    result = call_os_cmd('cat %s' % procfs_buf_count)
    assert(result=='%s'%l)

    print('Verify buf_count reading %s == %d' % (sysfs_buf_count, l))
    result = call_os_cmd('cat %s' % sysfs_buf_count)
    assert(result=='%s'%l)

    fd = os.open(dev, os.O_RDWR)
    assert (fd is not None)
    with os.fdopen(fd, 'w') as fo:
        assert(fo is not None)
        print('Flush buffer using IOCTL_GPIO_FLUSH')
        rc = fcntl.ioctl(fo, _IOCTL_GPIO_FLUSH, dummy_int_arg, True)
        assert (rc == 0)

        print('Verify ioctl read of buf_count == 0')
        rc = fcntl.ioctl(fo, _IOCTL_GPIO_COUNT, buf_count, True)
        assert (rc == 0)
        print ('buf_count=', buf_count)
        assert (buf_count.value == 0)
