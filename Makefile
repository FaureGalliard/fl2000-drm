# Out-of-tree build wrapper. The driver itself lives in drivers/gpu/drm/fl2000/
# laid out exactly as it would be in the kernel tree, and follows the current
# kernel API. For older kernels the sources are first translated with the sed
# rules in compat/ (see compat/README).

KVER ?= $(shell uname -r)
KSRC ?= /lib/modules/$(KVER)/build
SRC := $(CURDIR)/drivers/gpu/drm/fl2000
BUILD := $(CURDIR)/.compat-build
HDR := $(KSRC)/include/drm

# probe <file> <pattern> <rule>: use compat/<rule> if the pattern is missing
probe = $(shell grep -qs '$(2)' $(HDR)/$(1) || echo $(CURDIR)/compat/$(3))
COMPAT_RULES := $(strip \
	$(call probe,drm_atomic.h,struct drm_atomic_commit {,atomic-commit.sed) \
	$(call probe,drm_crtc.h,atomic_create_state,create-state.sed) \
	$(call probe,drm_bridge.h,atomic_create_state,bridge-state.sed))

ifeq ($(COMPAT_RULES),)
MSRC := $(SRC)
else
MSRC := $(BUILD)
endif

all: modules

modules:
ifneq ($(COMPAT_RULES),)
	@echo "Older kernel API, applying: $(notdir $(COMPAT_RULES))"
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cp $(SRC)/Kconfig $(SRC)/Makefile $(SRC)/*.[ch] $(BUILD)/
	sed -i $(foreach r,$(COMPAT_RULES),-f $(r)) $(BUILD)/*.[ch]
endif
	$(MAKE) -C $(KSRC) M=$(MSRC) CONFIG_DRM_FL2000=m modules
	cp $(MSRC)/fl2000.ko $(CURDIR)/

clean:
	$(MAKE) -C $(KSRC) M=$(SRC) clean
	rm -rf $(BUILD) $(CURDIR)/fl2000.ko

.PHONY: all modules clean
