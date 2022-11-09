# Call make in each subdirectory
.ONESHELL:
SHELL = /bin/bash
.SHELLFLAGS += -e
SUBDIRS := $(wildcard */.)


define FOREACH
    for DIR in $(SUBDIRS); do \
        $(MAKE) -C $$DIR $(1); \
    done
endef

all: $(SUBDIRS)
	$(call FOREACH,all)


clean: $(SUBDIRS)
	$(call FOREACH,clean)

.PHONY: test
test:
	$(call FOREACH,test)

install-requirements:
	sudo apt update
	sudo apt install -y build-essential python3 python3-pip
	pip3 install -r requirements.txt

.PHONY: all $(SUBDIRS)
