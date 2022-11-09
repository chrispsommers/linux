import pytest
import os, subprocess

kmodule='fake_gpio'
devname='fake_gpio'
sysfsname='fake_gpio_sysfs'
modpath = os.path.realpath(os.path.dirname(__file__))+'/../%s.ko' % kmodule
print ('modpath=',modpath)

def call_os_cmd(cmd_string):
    return subprocess.check_output(cmd_string.split(' '), universal_newlines=True)

def test_kmod_filesystems():
    dev = '/dev/%s' % devname
    print("Checking existence of", dev)
    assert(os.path.exists(dev))

    sysfs = '/sys/kernel/%s' % sysfsname
    print("Checking existence of", sysfs)
    assert(os.path.exists(sysfs))

    sysfs_buf_count = '/sys/kernel/%s/buf_count' % sysfsname
    print("Checking existence of", sysfs_buf_count)
    assert(os.path.exists(sysfs_buf_count))

    sysfs_gpio_mode = '/sys/kernel/%s/gpio_mode' % sysfsname
    print("Checking existence of", sysfs_gpio_mode)
    assert(os.path.exists(sysfs_gpio_mode))


def test_gpio_mode0():
    print('Test default gpio mode == 0')
    result = call_os_cmd('cat /sys/kernel/%s/gpio_mode' % sysfsname)
    assert(result=='0')
