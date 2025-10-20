#
# Copyright (C) 2025 Kenneth Kasilag <kenneth@kasilag.me>
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk
include $(INCLUDE_DIR)/kernel.mk

PKG_NAME:=phy-mxl371x
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0

include $(INCLUDE_DIR)/package.mk

# hax
NETWORK_DEVICES_MENU:=Network Devices

define KernelPackage/phy-mxl371x
  SUBMENU:=$(NETWORK_DEVICES_MENU)
  TITLE:=MaxLinear MXL371x MoCA 2.5 PHY support
  DEPENDS:=+kmod-libphy +kmod-hwmon-core +mxl-firmware
  FILES:= \
	$(PKG_BUILD_DIR)/mxl371x.ko
  AUTOLOAD:=$(call AutoLoad,18,mxl371x,1)
endef

define KernelPackage/phy-mxl371x/description
  Open MaxLinear MXL371x MoCA 2.5 PHY support
endef

define Build/Compile
	$(KERNEL_MAKE) M="$(PKG_BUILD_DIR)" modules
endef

$(eval $(call KernelPackage,phy-mxl371x))
