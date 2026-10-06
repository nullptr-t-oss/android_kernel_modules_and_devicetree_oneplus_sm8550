#Kalama and Camano specific build rules
ifeq ($(call is-board-platform-in-list, kalama crow),true)
TARGET_USES_ST_ESE := true
BOARD_VENDOR_KERNEL_MODULES += $(KERNEL_MODULES_OUT)/stm_st54se_gpio.ko
endif
