# SPDX-License-Identifier: GPL-2.0-or-later

include $(TOPDIR)/rules.mk

PKG_NAME:=mxl-firmware
PKG_VERSION:=1.0.0
PKG_RELEASE:=1
# PKG_SOURCE_URL:=https://www.verizon.com/content/dam/verizon/support/consumer/documents/open-source/Verizon%20Router%20(CR1000A)%20source%20code%20release%204.0.22.40.zip

PKG_MAINTAINER:=Kenneth Kasilag <kenneth@kasilag.me>
PKG_LICENSE:=Proprietary
PKG_LICENSE_FILES:=

include $(INCLUDE_DIR)/package.mk

define Package/mxl-firmware
  SECTION:=firmware
  CATEGORY:=Firmware
  TITLE:=MaxLinear MXL371x MoCA PHY firmware.
  URL:=
endef

define Package/mxl-firmware/description
  Firmware file for the MaxLinear MXL371x MoCA 2.5 PHY.
endef

define Build/Compile
endef

define Package/mxl-firmware/install
	$(INSTALL_DIR) $(1)/lib/firmware
	$(INSTALL_DATA) \
		./ccpu.elf.leucadia \
		$(1)/lib/firmware/
endef

$(eval $(call BuildPackage,mxl-firmware))
