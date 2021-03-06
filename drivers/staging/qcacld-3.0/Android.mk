# Android makefile for the WLAN Module

# Assume no targets will be supported
WLAN_CHIPSET :=

ifeq ($(BOARD_HAS_QCOM_WLAN), true)

# Check if this driver needs be built for current target
ifneq ($(findstring qca_cld3,$(WIFI_DRIVER_BUILT)),)
	WLAN_CHIPSET := qca_cld3
	WLAN_SELECT  := CONFIG_QCA_CLD_WLAN=m
endif

# Build/Package only in case of supported target
ifneq ($(WLAN_CHIPSET),)

LOCAL_PATH := $(call my-dir)

# This makefile is only for DLKM
ifneq ($(findstring vendor,$(LOCAL_PATH)),)

ifneq ($(findstring opensource,$(LOCAL_PATH)),)
	WLAN_BLD_DIR := vendor/qcom/opensource/wlan
endif # opensource

# DLKM_DIR was moved for JELLY_BEAN (PLATFORM_SDK 16)
ifeq ($(call is-platform-sdk-version-at-least,16),true)
	DLKM_DIR := $(TOP)/device/qcom/common/dlkm
else
	DLKM_DIR := build/dlkm
endif # platform-sdk-version

# Build wlan.ko as $(WLAN_CHIPSET)_wlan.ko
###########################################################
# This is set once per LOCAL_PATH, not per (kernel) module
KBUILD_OPTIONS := WLAN_ROOT=$(WLAN_BLD_DIR)/qcacld-3.0
KBUILD_OPTIONS += WLAN_COMMON_ROOT=../qca-wifi-host-cmn
KBUILD_OPTIONS += WLAN_COMMON_INC=$(WLAN_BLD_DIR)/qca-wifi-host-cmn

# We are actually building wlan.ko here, as per the
# requirement we are specifying <chipset>_wlan.ko as LOCAL_MODULE.
# This means we need to rename the module to <chipset>_wlan.ko
# after wlan.ko is built.
KBUILD_OPTIONS += MODNAME=wlan
KBUILD_OPTIONS += BOARD_PLATFORM=$(TARGET_BOARD_PLATFORM)
KBUILD_OPTIONS += $(WLAN_SELECT)

##########################################################
# Copy the unstrip file and corresponding elf file to  out symbols folders
WLAN_SYMBOLS_OUT     := $(TARGET_OUT_UNSTRIPPED)/$(LOCAL_PATH)
UNSTRIPPED_MODULE    := $(WLAN_CHIPSET)_wlan.ko.unstripped
UNSTRIPPED_FILE_PATH := $(TARGET_OUT_INTERMEDIATES)/$(LOCAL_PATH)/$(UNSTRIPPED_MODULE)

ifneq ($(filter msm8998 sdm845 sdm670 sdm710, $(TARGET_BOARD_PLATFORM)),)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/wlan_proc/build/ms/WLAN_MERGED.elf
else ifneq ($(filter sdm660, $(TARGET_BOARD_PLATFORM)),)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.1.0.1/wlan_proc/build/ms/WLAN_MERGED.elf
else ifneq ($(filter sm6150, $(TARGET_BOARD_PLATFORM)),)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.3.0.1/wlan_proc/build/ms/*.elf
else ifneq ($(filter trinket, $(TARGET_BOARD_PLATFORM)),)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.3.0.2/wlan_proc/build/ms/WLAN_MERGED.elf
else ifneq ($(filter lito, $(TARGET_BOARD_PLATFORM)),)
    ifeq ($(WLAN_FW_ALT_VER), 5)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.3.2.5/wlan_proc/build/ms/Msaipan_WLAN_MERGED.elf
    else ifeq ($(WLAN_FW_ALT_VER), 3)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.3.3.1/wlan_proc/build/ms/Mbitra_WLAN_MERGED.elf
    else
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.3.2.1/wlan_proc/build/ms/Msaipan_WLAN_MERGED.elf
    endif
else ifneq ($(filter bengal, $(TARGET_BOARD_PLATFORM)),)
    WLAN_ELF_FILE_PATH    := vendor/qcom/nonhlos/WLAN.HL.3.2.4/wlan_proc/build/ms/WLAN_MERGED.elf
endif

INSTALL_WLAN_UNSTRIPPED_MODULE := mkdir -p $(WLAN_SYMBOLS_OUT); \
   cp -rf $(UNSTRIPPED_FILE_PATH) $(WLAN_SYMBOLS_OUT); \
   cp -rf $(WLAN_ELF_FILE_PATH) $(WLAN_SYMBOLS_OUT)

include $(CLEAR_VARS)
LOCAL_MODULE              := $(WLAN_CHIPSET)_wlan.ko
LOCAL_MODULE_KBUILD_NAME  := wlan.ko
LOCAL_MODULE_DEBUG_ENABLE := true
ifeq ($(PRODUCT_VENDOR_MOVE_ENABLED),true)
    ifeq ($(WIFI_DRIVER_INSTALL_TO_KERNEL_OUT),true)
        LOCAL_MODULE_PATH := $(KERNEL_MODULES_OUT)
    else
        LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/modules/$(WLAN_CHIPSET)
    endif
else
    LOCAL_MODULE_PATH := $(TARGET_OUT)/lib/modules/$(WLAN_CHIPSET)
endif

# Once unstripped file is generated, copy the same to out symbols folder
LOCAL_POST_INSTALL_CMD := $(INSTALL_WLAN_UNSTRIPPED_MODULE)

include $(DLKM_DIR)/AndroidKernelModule.mk
###########################################################

# Create Symbolic link
ifneq ($(findstring $(WLAN_CHIPSET),$(WIFI_DRIVER_DEFAULT)),)
ifeq ($(PRODUCT_VENDOR_MOVE_ENABLED),true)
ifneq ($(WIFI_DRIVER_INSTALL_TO_KERNEL_OUT),)
$(shell mkdir -p $(TARGET_OUT_VENDOR)/lib/modules; \
	ln -sf /$(TARGET_COPY_OUT_VENDOR)/lib/modules/$(WLAN_CHIPSET)/$(LOCAL_MODULE) $(TARGET_OUT_VENDOR)/lib/modules/wlan.ko)
endif
else
$(shell mkdir -p $(TARGET_OUT)/lib/modules; \
	ln -sf /system/lib/modules/$(WLAN_CHIPSET)/$(LOCAL_MODULE) $(TARGET_OUT)/lib/modules/wlan.ko)
endif
endif

ifeq ($(PRODUCT_VENDOR_MOVE_ENABLED),true)
$(shell ln -sf /mnt/vendor/persist/wlan_mac.bin $(TARGET_OUT_VENDOR)/firmware/wlan/qca_cld/wlan_mac.bin)
else
$(shell ln -sf /mnt/vendor/persist/wlan_mac.bin $(TARGET_OUT_ETC)/firmware/wlan/qca_cld/wlan_mac.bin)
endif
endif # DLKM check
endif # supported target check
endif # WLAN enabled check
