
obj-m += fake_gpio.o 
CFLAGS+=-std=gnu99

all: driver utils
driver:
	 make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

utils: rgpio flush

test:
	sudo insmod fake_gpio.ko
	sudo rmmod fake_gpio
	dmesg |grep fake_gpio

clean:
	make -C /lib/modules/$(shell uname -r)/build  M=$(shell pwd) clean
	rm rgpio flush
