
obj-m += fake_gpio.o 
CFLAGS+=-std=gnu99

all: driver utils
driver:
	 make -C /lib/modules/$(shell uname -r)/build M=$(shell pwd) modules

utils: rgpio flush

.PHONY: test
test:
	sudo python3 -m pytest -s

clean:
	make -C /lib/modules/$(shell uname -r)/build  M=$(shell pwd) clean
	rm rgpio flush
