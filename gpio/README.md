# gpio - Fake Linux char device driver
Demonstrates some mechanisms:
* Create character device & class
* Create sysfs entries for device "mode" configuration
* Mutex-protected fifo buffer to store data in flight
* Non-blocking serialization into fake "I2C" databus using timers, notifications, etc.