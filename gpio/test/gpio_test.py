import pytest
import os, subprocess

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

def call_os_cmd(cmd_string):
    return subprocess.check_output(cmd_string.split(' '), universal_newlines=True)

def test_kmod_filesystems():
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

def test_initial_conditions():
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
