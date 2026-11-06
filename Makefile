# Out-of-tree build wrapper. The driver itself lives in drivers/gpu/drm/fl2000/
# laid out exactly as it would be in the kernel tree, and follows the current
# kernel API. For older kernels the sources are first translated with the sed
# rules in compat/, and DRM helpers the target kernel was built without are
# compiled into the module from compat/drm-helpers/ (see compat/README).

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

# helper <symbol> <source>: compile the kernel's own drivers/gpu/drm/display/
# <source>.c into the module if the target kernel does not export <symbol>.
# The Kconfig selects them in a kernel build; distribution kernels for PCs
# usually lack them (DRM_BRIDGE_CONNECTOR is only selected by SoC drivers).
helper = $(shell grep -qsw '$(1)' $(KSRC)/Module.symvers || echo $(2))
COMPAT_HELPERS := $(strip \
	$(call helper,drm_bridge_connector_init,drm_bridge_connector) \
	$(call helper,drm_atomic_helper_connector_hdmi_check,drm_hdmi_state_helper) \
	$(call helper,drm_connector_hdmi_audio_init,drm_hdmi_audio_helper) \
	$(call helper,drmm_connector_hdmi_cec_register,drm_hdmi_cec_helper))
KREL := $(shell cat $(KSRC)/include/config/kernel.release 2>/dev/null || echo $(KVER))
HELPERS := $(CURDIR)/compat/drm-helpers/$(shell echo $(KREL) | sed -nE 's/^([0-9]+\.[0-9]+).*/\1/p')

ifeq ($(COMPAT_RULES)$(COMPAT_HELPERS),)
MSRC := $(SRC)
else
MSRC := $(BUILD)
endif

all: modules

modules:
ifneq ($(COMPAT_RULES)$(COMPAT_HELPERS),)
	rm -rf $(BUILD) && mkdir -p $(BUILD)
	cp $(SRC)/Kconfig $(SRC)/Makefile $(SRC)/*.[ch] $(BUILD)/
endif
ifneq ($(COMPAT_RULES),)
	@echo "Older kernel API, applying: $(notdir $(COMPAT_RULES))"
	sed -i $(foreach r,$(COMPAT_RULES),-f $(r)) $(BUILD)/*.[ch]
endif
ifneq ($(COMPAT_HELPERS),)
	@echo "Kernel built without them, compiling in: $(COMPAT_HELPERS)"
	@for h in $(COMPAT_HELPERS); do [ -f $(HELPERS)/$$h.c ] || missing=1; done; \
	if [ -n "$$missing" ]; then \
		$(CURDIR)/compat/fetch-drm-helpers.sh $(KREL) $(HELPERS) $(COMPAT_HELPERS) && \
		echo "New helper sources in $(HELPERS): commit them"; \
	fi
	cd $(HELPERS) && sha256sum --check --quiet SHA256SUMS
	cp $(foreach h,$(COMPAT_HELPERS),$(HELPERS)/$(h).c) $(BUILD)/
	echo 'fl2000-y += $(addsuffix .o,$(COMPAT_HELPERS))' >> $(BUILD)/Makefile
endif
	$(MAKE) -C $(KSRC) M=$(MSRC) CONFIG_DRM_FL2000=m modules
	cp $(MSRC)/fl2000.ko $(CURDIR)/

clean:
	$(MAKE) -C $(KSRC) M=$(SRC) clean
	rm -rf $(BUILD) $(CURDIR)/fl2000.ko

.PHONY: all modules clean
