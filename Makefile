# Out-of-tree build wrapper. The driver itself lives in drivers/gpu/drm/fl2000/
# laid out exactly as it would be in the kernel tree.

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build
SRC := $(CURDIR)/drivers/gpu/drm/fl2000

all: modules

# Older kernels use previous names for a few DRM APIs: probe the target
# headers and let compat/fl2000_compat.h map the new names onto them
COMPAT := $(shell grep -qs 'struct drm_atomic_commit {' $(KSRC)/include/drm/drm_atomic.h || \
		echo -DFL2000_COMPAT_ATOMIC_STATE) \
	  $(shell grep -qs 'atomic_create_state' $(KSRC)/include/drm/drm_bridge.h || \
		echo -DFL2000_COMPAT_BRIDGE_RESET)
COMPAT_CFLAGS := $(if $(strip $(COMPAT)),$(COMPAT) -include $(CURDIR)/compat/fl2000_compat.h)

modules:
	$(MAKE) -C $(KSRC) M=$(SRC) CONFIG_DRM_FL2000=m KCFLAGS="$(COMPAT_CFLAGS)" modules
	cp $(SRC)/fl2000.ko $(CURDIR)/

clean:
	$(MAKE) -C $(KSRC) M=$(SRC) clean
	rm -f $(CURDIR)/fl2000.ko

.PHONY: all modules clean
