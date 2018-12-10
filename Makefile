###############################################################################
#
# File: Makefile
#
# Copyright 2014 TiVo Inc. All Rights Reserved.
#
###############################################################################

ISM_DEPTH := ..
include $(ISM_DEPTH)/ismdefs

HAXELIB_NAME := hxcpp

# This is defined because this haxelib already has a haxelib.json file and
# doesn't need one to be generated
SUPPRESS_HAXELIB_JSON := 1

# To ensure that the libs are built before the bom files are generated
# (avoiding the problem where the bom process will miss the libs), we
# assign our targets in PRE_BOM_TARGETS, not TARGETS.

# Built targets of hxcpp haxelib
PRE_BOM_TARGETS += BuildBuildN BuildHxcppN BuildRunN BuildLibs

ifeq ($(HAXE_BUILD_TARGET),android)
PRE_BOM_TARGETS += BuildNative
endif

ifeq ($(HAXE_BUILD_TARGET),ios)
PRE_BOM_TARGETS += BuildNative
endif

ifeq ($(HAXE_BUILD_TARGET),tvos)
PRE_BOM_TARGETS += BuildNative
endif

LTDIRT += $(HAXELIB_STAGED_DIR)/build.n
LTDIRT += $(HAXELIB_STAGED_DIR)/hxcpp.n
LTDIRT += $(HAXELIB_STAGED_DIR)/run.n

ifneq ($(HAXE_BUILD_TARGET),html5)
ifneq ($(HAXE_BUILD_TARGET),java)
ifneq ($(HAXE_BUILD_TARGET),android)
ifneq ($(HAXE_BUILD_TARGET),ios)
ifneq ($(HAXE_BUILD_TARGET),tvos)

# Special leaf bom targets that contribute bom fragments that can use
# variables from this Makefile
LEAF_BOM_FRAGMENTS = leaf.bom

endif
endif
endif
endif
endif

include $(ISMRULES)

HAXELIB_PATH = $(HAXELIB_STAGED_DIR)/../..
export HAXELIB_PATH

# Manually build build.n using the same command as described by
# tools/build/compile.hxml, except with the output written to
# $(HAXELIB_STAGED_DIR) instead of into the source directory
.PHONY: BuildBuildN
BuildBuildN: $(HAXELIB_STAGED_DIR)/build.n
$(HAXELIB_STAGED_DIR)/build.n: $(STAGE_HAXELIB_TARGET) tools/build
	@$(call verify-checksum,tools/build/compile.hxml,13e6717471c0b9e5af8d524699b5954cf0de0df4)
	@$(ECHO) -n "$(ISMCOLOR)$(ISM_NAME)$(UNCOLOR): "; \
	$(ECHO) "$(COLOR)Rebuilding hxcpp build.n for $(HAXE_HOST_SYSTEM)$(UNCOLOR)";
	$(Q) $(HAXE) -neko $@ -main Build -cp tools/build -D neko_v1 -D tivo -lib hxcpp -debug

# Manually build run.n using the same command as described by
# tools/run/compile.hxml, except with the output written to
# $(HAXELIB_STAGED_DIR) instead of into the source directory
.PHONY: BuildRunN
BuildRunN: $(HAXELIB_STAGED_DIR)/run.n
$(HAXELIB_STAGED_DIR)/run.n: $(STAGE_HAXELIB_TARGET) tools/run
	@$(call verify-checksum,tools/run/compile.hxml,4536eeb99dfe2eeac686b82416a7cbbe2e9ebfda)
	@$(ECHO) -n "$(ISMCOLOR)$(ISM_NAME)$(UNCOLOR): "; \
	$(ECHO) "$(COLOR)Rebuilding hxcpp run.n for $(HAXE_HOST_SYSTEM)$(UNCOLOR)";
	$(Q) $(HAXE) -neko $@ -main RunMain -cp tools/run -D neko_v1 -D tivo -debug

# Manually build hxcpp.n using the same command as described by
# tools/hxcpp/compile.hxml, except with the output written to
# $(HAXELIB_STAGED_DIR) instead of into the source directory
.PHONY: BuildHxcppN
BuildHxcppN: $(HAXELIB_STAGED_DIR)/hxcpp.n
$(HAXELIB_STAGED_DIR)/hxcpp.n: $(STAGE_HAXELIB_TARGET) tools/hxcpp
	@$(call verify-checksum,tools/hxcpp/compile.hxml,9dacb70641bd96199abd22e259fd473609f0805c)
	@$(ECHO) -n "$(ISMCOLOR)$(ISM_NAME)$(UNCOLOR): "; \
	$(ECHO) "$(COLOR)Rebuilding hxcpp hxcpp.n for $(HAXE_HOST_SYSTEM)$(UNCOLOR)";
	$(Q) $(HAXE) -neko $@ -main BuildTool -cp tools/hxcpp -D neko_v1 -D tivo -debug

# Manually build the native code using the build.n created above.
.PHONY: BuildLibs
BuildLibs: BuildBuildN BuildHxcppN BuildRunN
	@$(ECHO) -n "$(ISMCOLOR)$(ISM_NAME)$(UNCOLOR): "; \
	$(ECHO) "$(COLOR)Rebuilding hxcpp libs for $(HAXE_HOST_SYSTEM)$(UNCOLOR)";
	$(Q) cd $(HAXELIB_STAGED_DIR)/project; \
      neko $(HAXELIB_STAGED_DIR)/build.n \
           $(HAXE_HOST_SYSTEM)-m$(HAXE_HOST_BITS) -DHXCPP_VERBOSE=1 \
           $(HXCPP_ARGS)

# Manually build the native code using the build.n created above
.PHONY: BuildNative
BuildNative: BuildBuildN BuildHxcppN BuildRunN
	@$(ECHO) -n "$(ISMCOLOR)$(ISM_NAME)$(UNCOLOR): "; \
	$(ECHO) "$(COLOR)Rebuilding hxcpp libs for $(HAXE_BUILD_TARGET)$(UNCOLOR)";
	$(Q) cd $(HAXELIB_STAGED_DIR)/project; \
      HXCPP_CONFIG=$(ISM_TOPDIR)/lib/hxcpp_config.xml neko \
           $(HAXELIB_STAGED_DIR)/build.n $(HAXE_BUILD_TARGET) \
               -DHXCPP_VERBOSE=1 $(HXCPP_ARGS)
