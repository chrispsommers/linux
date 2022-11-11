import pytest
import os, subprocess

kmodule='fake_gpio'
devname='fake_gpio'
sysfsname='fake_gpio_sysfs'
modpath = os.path.realpath(os.path.dirname(__file__))+'/../%s.ko' % kmodule
print ('modpath=',modpath)


# https://stackoverflow.com/questions/40660842/pytest-setup-teardown-hooks-for-session
# @pytest.fixture(autouse=True, scope='session')
@pytest.fixture(autouse=True)
def kmod_fixture():
    print ("Inserting kernel module %s..." % kmodule)
    subprocess.run(['insmod', '%s' % modpath])
    yield
    print ("Removing kernel module %s..." % kmodule)
    subprocess.run(['rmmod', '%s' % kmodule])
    # os.system('dmesg | grep %s' % kmodule)
