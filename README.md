# MaxLinear MXL371x MoCA 2.5 PHY Driver

Linux kernel driver for MaxLinear MXL371x MoCA 2.5 PHY.

## Overview

This driver provides basic support for MaxLinear's MXL371x MoCA 2.5 PHY. It handles firmware loading, configuration, statistics collection, and temperature monitoring.

## Features

### Core Functionality
- **Automatic PHY detection** via MDIO bus (OUI-based)
- **Firmware loading** from `/lib/firmware/`
  - Warm boot detection (skips reload if already running)
- **SGMII/HSGMII support**
  - 1000 Mbps (SGMII) for MoCA 2.0
  - 2500 Mbps (HSGMII) for MoCA 2.5
  - Device tree configuration or auto-detection

### MoCA Network Management
- **MoCA GUID management**
  - Uses MaxLinear OUI (`02:24:3E`) + random bytes
  - Persistent across reboots
  - Configurable via sysfs
- **Link status monitoring**
  - Real-time link state (up/down/scanning)
  - MoCA version detection (1.1, 2.0, 2.5)
  - PHY rate reporting
  - Node ID and Network Coordinator tracking
  - Last Operating Frequency (LOF)
  - Active nodes bitmask

### Statistics & Monitoring
- **Temperature monitoring** via Linux hwmon subsystem
  - Real-time chip temperature via `sensors` command
  - Accessible via `/sys/class/hwmon/`
- **Network statistics** via ethtool
  - TX/RX packets, bytes, errors
  - Broadcast/multicast counters
  - Drop counters
- **MoCA-specific sysfs attributes**
  - Link status, version, PHY rate
  - Node IDs, LOF, network state
  - Security status
  - Chip type and firmware version

### Power Management
- **Suspend/resume support**
- **Warm boot optimization**

## Requirements

### Kernel Version
- Linux 5.10 or newer (tested on 6.12+)
- CONFIG_PHYLIB enabled
- CONFIG_HWMON enabled

### Firmware Files
Required firmware must be placed in `/lib/firmware/`:

**For MXL3710/MXL3711 (Leucadia):**
```
/lib/firmware/ccpu.elf.leucadia
```

**For older Cardiff chips:**
```
/lib/firmware/ccpu.elf.cardiff
```

## Building

To build `kmod-phy-mxl371x` for OpenWrt, first add this feed to your ``feeds.conf`` in a fully set-up OpenWrt SDK [(read here on how to setup the OpenWrt SDK)](https://openwrt.org/docs/guide-developer/using_the_sdk):

```
echo "src-git mxl https://github.com/hurrian/kmod-phy-mxl371x.git" >> feeds.conf

$ ./scripts/feeds update -a
$ ./scripts/feeds install -a
```

## Device Tree Configuration

```
&mdio0 {
	moca_phy: ethernet-phy@f {
		reg = <0xf>;  /* MDIO address */
		
		/* Required: Specify interface speed */
		phy-mode = "2500base-x";  /* HSGMII @ 2500 Mbps */
		/* or "sgmii" for 1000 Mbps */
	};
};
```

### Device Tree Properties

| Property | Type | Required | Description |
|----------|------|----------|-------------|
| `reg` | integer | Yes | MDIO address (0-31) |
| `phy-mode` | string | Yes | Interface mode: "sgmii" or "2500base-x" |
| `local-mac-address` | byte array | No | MoCA GUID (6 bytes) |
| `reset-gpios` | phandle | No | Reset GPIO specification |

## Usage

### Check Driver Status

```bash
# Check if driver is loaded
lsmod | grep mxl371x

# Check kernel messages
dmesg | grep mxl371x

# Example output (cold boot):
# [    3.234567] mxl371x 90000:0f: Leucadia Device 0x3711 Rev 0x0001
# [    3.345678] mxl371x 90000:0f: Loading firmware ccpu.elf.leucadia...
# [   13.456789] mxl371x 90000:0f: Firmware started successfully
# [   13.567890] mxl371x 90000:0f: Generated MoCA GUID: 02:24:3e:a7:5f:3c
# [   13.678901] mxl371x 90000:0f: Configured HSGMII @ 2500Mbps
# [   13.789012] mxl371x 90000:0f: MoCA PHY initialized (cold boot, MoCA v2.5, 2500Mbps)
```

### Read MoCA Status

```bash
# Find the PHY device
PHY_DEV=$(find /sys/devices -name "moca_guid" | head -1 | xargs dirname)

# Read MoCA GUID
cat $PHY_DEV/moca_guid
# 02:24:3e:a7:5f:3c

# Read link status
cat $PHY_DEV/moca_link_status
# up

# Read MoCA version
cat $PHY_DEV/moca_version
# 2.5

# Read PHY rate (Mbps)
cat $PHY_DEV/moca_phy_rate
# 2400

# Read node ID
cat $PHY_DEV/moca_node_id
# 3

# Read network coordinator node ID
cat $PHY_DEV/moca_nc_node_id
# 1

# Read Last Operating Frequency (MHz)
cat $PHY_DEV/moca_lof
# 1150

# Read network state
cat $PHY_DEV/moca_network_state
# network

# Read active nodes bitmask
cat $PHY_DEV/moca_active_nodes
# 0x0000000e

# Read security status
cat $PHY_DEV/moca_security_enabled
# 1

# Read chip type
cat $PHY_DEV/moca_chip_type
# leucadia

# Read firmware version
cat $PHY_DEV/moca_fw_version
# Leucadia Device 0x3711 Rev 0x0001
```

### Set MoCA GUID

```bash
# Set custom GUID
echo "02:24:3e:11:22:33" > $PHY_DEV/moca_guid

# Verify
cat $PHY_DEV/moca_guid
# 02:24:3e:11:22:33
```

### Monitor Temperature

```bash
# Using sensors command (requires lm-sensors package)
sensors mxl371x-*
# mxl371x-mdio-0-f
# Adapter: MDIO adapter
# temp1:        +45.2°C

# Direct sysfs access
cat /sys/class/hwmon/hwmon*/temp1_input
# 45250  (millidegrees Celsius)

# Convert to Celsius
awk '{print $1/1000 "°C"}' /sys/class/hwmon/hwmon*/temp1_input
# 45.25°C
```

### View Statistics

```bash
# Using ethtool
ethtool -S eth0
# NIC statistics:
#      RxFrames: 123456
#      RxOctets: 987654321
#      RxErrors: 0
#      TxFrames: 654321
#      TxOctets: 123456789
#      TxErrors: 0

# Using ip command
ip -s link show eth0
# 2: eth0: <BROADCAST,MULTICAST,UP,LOWER_UP> mtu 1500 qdisc fq_codel state UP mode DEFAULT group default qlen 1000
#     link/ether 00:11:22:33:44:55 brd ff:ff:ff:ff:ff:ff
#     RX: bytes  packets  errors  dropped overrun mcast
#     987654321  123456   0       0       0       12345
#     TX: bytes  packets  errors  dropped carrier collsns
#     123456789  654321   0       0       0       0
```

## Sysfs Attributes

All attributes are located under `/sys/devices/.../mdio_bus/.../`:

### Read-Only Attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `moca_link_status` | string | Link state: "up", "down", or "scanning" |
| `moca_version` | string | MoCA version: "1.1", "2.0", or "2.5" |
| `moca_phy_rate` | integer | PHY rate in Mbps |
| `moca_node_id` | integer | MoCA node ID (0-15) |
| `moca_nc_node_id` | integer | Network Coordinator node ID |
| `moca_lof` | integer | Last Operating Frequency in MHz |
| `moca_network_state` | string | Network state: "idle", "searching", or "network" |
| `moca_active_nodes` | hex | Bitmask of active nodes |
| `moca_security_enabled` | boolean | Security status: 0 or 1 |
| `moca_chip_type` | string | Chip type: "leucadia" or "cardiff" |
| `moca_fw_version` | string | Firmware version string |

### Read-Write Attributes

| Attribute | Type | Description |
|-----------|------|-------------|
| `moca_guid` | MAC address | MoCA GUID (format: XX:XX:XX:XX:XX:XX) |

## TODO / Future Work

### Userspace Utility (`mocactl`)

A comprehensive userspace utility is planned to provide advanced MoCA management features not suitable for the kernel driver. This tool will use the mailbox interface to communicate with the MoCA firmware.

## Contributing

Contributions are welcome! Please follow Linux kernel coding style.

## License

GPL-2.0+

## Credits

- **Author:** Kenneth Kasilag <kenneth@kasilag.me>

## References

- [ASUS ZenWifi XC5 source](https://github.com/tsg2k2/ASUS-ZenWiFi-XC5/blob/main/asuswrt/release/src/router/mxl371x_sdk/MxLWare/drv/osal/mxl_moca_osal_linux.c)
- [Verizon CR1000A firmware 4.0.22.40](https://www.verizon.com/content/dam/verizon/support/consumer/documents/open-source/Verizon%20Router%20(CR1000A)%20source%20code%20release%204.0.22.40.zip)

## Support

For issues, questions, or feature requests:
- Review kernel logs: `dmesg | grep mxl371x`
- File an issue with:
  - Kernel version
  - Hardware platform
  - Device tree
  - Full dmesg output
  - Output of `cat /sys/devices/.../moca_*`

---
