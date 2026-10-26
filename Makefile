# Out-of-tree build wrapper. The driver itself lives in drivers/gpu/drm/fl2000/
# laid out exactly as it would be in the kernel tree.

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build
SRC := $(CURDIR)/drivers/gpu/drm/fl2000

all: modules

modules:
	$(MAKE) -C $(KSRC) M=$(SRC) CONFIG_DRM_FL2000=m modules
	cp $(SRC)/fl2000.ko $(CURDIR)/

clean:
	$(MAKE) -C $(KSRC) M=$(SRC) clean
	rm -f $(CURDIR)/fl2000.ko

.PHONY: all modules clean
