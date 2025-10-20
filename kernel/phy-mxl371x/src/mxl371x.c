// SPDX-License-Identifier: GPL-2.0+
/*
 * Driver for MaxLinear MXL371x MoCA 2.5 PHYs
 *
 * Copyright (c) 2025 Kenneth Kasilag <kenneth@kasilag.me>
 *
 */

#include <linux/module.h>
#include <linux/phy.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_net.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/hwmon.h>
#include <linux/random.h>

/* MaxLinear OUI and PHY IDs */
#define MXL371X_OUI			0x0243E000
#define MXL371X_OUI_MASK		0xFFFFF000

#define MXL3710_PHY_ID			0x02434770
#define MXL3711_PHY_ID			0x02434771

/* Firmware files */
#define MXL371X_FW_LEUCADIA		"ccpu.elf.leucadia"
#define MXL371X_FW_CARDIFF		"ccpu.elf.cardiff"
#define MXL371X_MAX_FW_SIZE		(4 * 1024 * 1024)

/* MoCA SoC Chip Types */
#define MXL_MOCA_SOC_TYPE_LEUCADIA	0
#define MXL_MOCA_SOC_TYPE_CARDIFF	1

/* Standard PHY Registers */
#define MXL371X_BMCR			0x00
#define MXL371X_BMSR			0x01
#define MXL371X_PAGE_SELECT		0x1f

/* System Resource Engine (SRE) Registers */
#define SRE_PRODUCT_FAMILY_ID		0x08200000
#define SRE_DEVICE_ID			0x08200004
#define SRE_REVISION_ID_OFFSET		16
#define SRE_CPU_SRC_SEL_CSR		0x08200010

/* Temperature Sensor Registers */
#define MXL371X_TSENS_CTRL_REG		0x08200200
#define MXL371X_TSENS_DATA_REG		0x08200204
#define MXL371X_RADIO_TSENS_REG1	0x0c14c110
#define MXL371X_RADIO_TSENS_REG2	0x0c14c100
#define MXL371X_RADIO_TSENS_REG3	0x0c14c108

/* Temperature calculation constants */
#define MXL371X_TSENS_COEFF_A		1338680
#define MXL371X_TSENS_COEFF_B		277770
#define MXL371X_TSENS_RSSI_MAX		524288

/* Firmware Status */
#define MXL371X_FW_BASE_ADDR		0x00000000
#define MXL371X_FW_STATUS_REG		0x08200100
#define MXL371X_FW_LOADED		BIT(0)
#define MXL371X_FW_RUNNING		BIT(1)
#define MXL371X_FW_ERROR		BIT(2)

/* MDIO Communication */
#define MXL371X_MDIO_ADDR_REG		0x0e
#define MXL371X_MDIO_DATA_REG		0x0f

/* SGMII/HSGMII Configuration */
#define MXL371X_SGMII_CTRL		0xa000
#define MXL371X_SGMII_MODE_MASK		0xff
#define MXL371X_SGMII_MODE_SGMII	0x02
#define MXL371X_SGMII_MODE_HSGMII	0x03
#define MXL371X_SGMII_MODE_1000BASE_X	0x04

/* MoCA Statistics Registers */
#define MOCA_STATS_BASE			0x0c000000
#define MOCA_STATS_TX_TOTAL_PKTS	(MOCA_STATS_BASE + 0x00)
#define MOCA_STATS_TX_TOTAL_BYTES	(MOCA_STATS_BASE + 0x08)
#define MOCA_STATS_TX_DROPPED_PKTS	(MOCA_STATS_BASE + 0x10)
#define MOCA_STATS_TX_BCAST_PKTS	(MOCA_STATS_BASE + 0x18)
#define MOCA_STATS_TX_MCAST_PKTS	(MOCA_STATS_BASE + 0x20)
#define MOCA_STATS_RX_TOTAL_PKTS	(MOCA_STATS_BASE + 0x28)
#define MOCA_STATS_RX_TOTAL_BYTES	(MOCA_STATS_BASE + 0x30)
#define MOCA_STATS_RX_DROPPED_PKTS	(MOCA_STATS_BASE + 0x38)
#define MOCA_STATS_RX_ERROR_PKTS	(MOCA_STATS_BASE + 0x40)

/* MoCA Link Status */
#define MOCA_LINK_STATUS_REG		0x0c100000
#define MOCA_LINK_STATUS_MASK		0x07
#define MOCA_LINK_PHY_RATE_REG		0x0c100004
#define MOCA_LINK_MOCA_VER_REG		0x0c100008
#define MOCA_LINK_NODE_ID_REG		0x0c10000c
#define MOCA_LINK_NC_NODE_ID_REG	0x0c100010
#define MOCA_LINK_LOF_REG		0x0c100014
#define MOCA_LINK_NETWORK_STATE_REG	0x0c100018
#define MOCA_LINK_ACTIVE_NODES_REG	0x0c10001c

/* MoCA Link States */
#define MOCA_LINK_DOWN			0
#define MOCA_LINK_UP			1
#define MOCA_LINK_SCANNING		2

/* MoCA Version */
#define MOCA_VER_1_1			0x11
#define MOCA_VER_2_0			0x20
#define MOCA_VER_2_5			0x25

/* MoCA Network States */
#define MOCA_NET_STATE_IDLE		0
#define MOCA_NET_STATE_SEARCHING	1
#define MOCA_NET_STATE_NETWORK_MODE	2

/* MoCA MAC Address Registers (GUID) */
#define MOCA_MAC_ADDR_HI		0x0c100020
#define MOCA_MAC_ADDR_LO		0x0c100024

/* Privacy/Security Status */
#define MOCA_SECURITY_STATUS_REG	0x0c100200
#define MOCA_SECURITY_ENABLED		BIT(0)

struct mxl371x_priv {
	bool fw_loaded;
	u32 soc_chip_type;
	u32 device_id;
	u32 revision_id;
	u32 link_status;
	u32 moca_version;
	u32 phy_rate;
	u32 node_id;
	u32 nc_node_id;
	u32 lof;
	u32 network_state;
	u32 active_nodes;
	bool security_enabled;
	const char *fw_name;
	char soc_version[64];

	/* Statistics */
	struct {
		u64 tx_packets;
		u64 tx_bytes;
		u64 tx_dropped;
		u64 tx_broadcast;
		u64 tx_multicast;
		u64 rx_packets;
		u64 rx_bytes;
		u64 rx_dropped;
		u64 rx_errors;
	} stats;

	struct delayed_work stats_poll;
	struct device *hwmon_dev;
};

static int mxl371x_read_page(struct phy_device *phydev)
{
	return __phy_read(phydev, MXL371X_PAGE_SELECT);
}

static int mxl371x_write_page(struct phy_device *phydev, int page)
{
	return __phy_write(phydev, MXL371X_PAGE_SELECT, page);
}

static int mxl371x_read_mem32(struct phy_device *phydev, u32 addr, u32 *val)
{
	int ret;
	u16 data_hi, data_lo;

	ret = phy_write(phydev, MXL371X_MDIO_ADDR_REG, (addr >> 16) & 0xffff);
	if (ret < 0)
		return ret;

	ret = phy_write(phydev, MXL371X_MDIO_ADDR_REG + 1, addr & 0xffff);
	if (ret < 0)
		return ret;

	ret = phy_read(phydev, MXL371X_MDIO_DATA_REG);
	if (ret < 0)
		return ret;
	data_hi = ret;

	ret = phy_read(phydev, MXL371X_MDIO_DATA_REG + 1);
	if (ret < 0)
		return ret;
	data_lo = ret;

	*val = ((u32)data_hi << 16) | data_lo;
	return 0;
}

static int mxl371x_read_mem64(struct phy_device *phydev, u32 addr, u64 *val)
{
	u32 val_lo, val_hi;
	int ret;

	ret = mxl371x_read_mem32(phydev, addr, &val_lo);
	if (ret < 0)
		return ret;

	ret = mxl371x_read_mem32(phydev, addr + 4, &val_hi);
	if (ret < 0)
		return ret;

	*val = ((u64)val_hi << 32) | val_lo;
	return 0;
}

static int mxl371x_write_mem32(struct phy_device *phydev, u32 addr, u32 val)
{
	int ret;

	ret = phy_write(phydev, MXL371X_MDIO_ADDR_REG, (addr >> 16) & 0xffff);
	if (ret < 0)
		return ret;

	ret = phy_write(phydev, MXL371X_MDIO_ADDR_REG + 1, addr & 0xffff);
	if (ret < 0)
		return ret;

	ret = phy_write(phydev, MXL371X_MDIO_DATA_REG, (val >> 16) & 0xffff);
	if (ret < 0)
		return ret;

	return phy_write(phydev, MXL371X_MDIO_DATA_REG + 1, val & 0xffff);
}

/* Temperature sensor reading */
static int mxl371x_read_temp_raw(struct phy_device *phydev, u32 *t0, u32 *t1)
{
	int ret;

	/* Get T0 reading */
	ret = mxl371x_write_mem32(phydev, MXL371X_RADIO_TSENS_REG1, 0x31000001);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MXL371X_RADIO_TSENS_REG2, 0x00000401);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MXL371X_RADIO_TSENS_REG3, 0x00000001);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MXL371X_TSENS_CTRL_REG, 0x01130103);
	if (ret < 0)
		return ret;

	usleep_range(30000, 40000);

	ret = mxl371x_read_mem32(phydev, MXL371X_TSENS_DATA_REG, t0);
	if (ret < 0)
		return ret;

	/* Get T1 reading */
	ret = mxl371x_write_mem32(phydev, MXL371X_TSENS_CTRL_REG, 0x01130003);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MXL371X_RADIO_TSENS_REG2, 0x00000411);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MXL371X_TSENS_CTRL_REG, 0x01130003);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MXL371X_TSENS_CTRL_REG, 0x01130103);
	if (ret < 0)
		return ret;

	usleep_range(30000, 40000);

	ret = mxl371x_read_mem32(phydev, MXL371X_TSENS_DATA_REG, t1);
	if (ret < 0)
		return ret;

	return 0;
}

static int mxl371x_calc_temp(u32 t0, u32 t1)
{
	s64 delta, temp;

	if (t1 < t0)
		return -EINVAL;

	delta = (s64)(t1 - t0);
	temp = (delta * MXL371X_TSENS_COEFF_A) / MXL371X_TSENS_RSSI_MAX;
	temp -= MXL371X_TSENS_COEFF_B;

	return (int)temp;
}

/* Update statistics from hardware */
static void mxl371x_update_stats(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	struct device *dev = &phydev->mdio.dev;
	int ret;

	ret = mxl371x_read_mem64(phydev, MOCA_STATS_TX_TOTAL_PKTS,
				 &priv->stats.tx_packets);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_TX_TOTAL_BYTES,
				  &priv->stats.tx_bytes);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_TX_DROPPED_PKTS,
				  &priv->stats.tx_dropped);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_TX_BCAST_PKTS,
				  &priv->stats.tx_broadcast);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_TX_MCAST_PKTS,
				  &priv->stats.tx_multicast);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_RX_TOTAL_PKTS,
				  &priv->stats.rx_packets);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_RX_TOTAL_BYTES,
				  &priv->stats.rx_bytes);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_RX_DROPPED_PKTS,
				  &priv->stats.rx_dropped);
	ret |= mxl371x_read_mem64(phydev, MOCA_STATS_RX_ERROR_PKTS,
				  &priv->stats.rx_errors);

	if (ret < 0)
		dev_warn_ratelimited(dev, "Failed to update MoCA statistics\n");
}

/* Update MoCA status */
static void mxl371x_read_moca_status(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	struct device *dev = &phydev->mdio.dev;
	u32 val;
	int ret = 0;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_STATUS_REG, &val) == 0)
		priv->link_status = val & MOCA_LINK_STATUS_MASK;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_PHY_RATE_REG, &val) == 0)
		priv->phy_rate = val & 0xffff;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_MOCA_VER_REG, &val) == 0)
		priv->moca_version = val & 0xff;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_NODE_ID_REG, &val) == 0)
		priv->node_id = val & 0xff;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_NC_NODE_ID_REG, &val) == 0)
		priv->nc_node_id = val & 0xff;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_LOF_REG, &val) == 0)
		priv->lof = val;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_NETWORK_STATE_REG, &val) == 0)
		priv->network_state = val & 0xff;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_LINK_ACTIVE_NODES_REG, &val) == 0)
		priv->active_nodes = val;
	else
		ret = -EIO;

	if (mxl371x_read_mem32(phydev, MOCA_SECURITY_STATUS_REG, &val) == 0)
		priv->security_enabled = !!(val & MOCA_SECURITY_ENABLED);
	else
		ret = -EIO;

	if (ret < 0)
		dev_warn_ratelimited(dev, "Failed to read MoCA status\n");
}

static void mxl371x_stats_poll_work(struct work_struct *work)
{
	struct mxl371x_priv *priv = container_of(work, struct mxl371x_priv,
						 stats_poll.work);
	struct phy_device *phydev = container_of((void *)priv,
						  struct phy_device, priv);

	if (priv->fw_loaded && phydev->attached_dev) {
		mxl371x_update_stats(phydev);
		mxl371x_read_moca_status(phydev);
	}

	schedule_delayed_work(&priv->stats_poll, HZ);
}

/* Standard ethtool PHY statistics */
static void mxl371x_get_phy_stats(struct phy_device *phydev,
				  struct ethtool_eth_phy_stats *phy_stats,
				  struct ethtool_phy_stats *phydev_stats)
{
	struct mxl371x_priv *priv = phydev->priv;

	phydev_stats->rx_packets = priv->stats.rx_packets;
	phydev_stats->rx_bytes = priv->stats.rx_bytes;
	phydev_stats->rx_errors = priv->stats.rx_errors;
	phydev_stats->tx_packets = priv->stats.tx_packets;
	phydev_stats->tx_bytes = priv->stats.tx_bytes;
	phydev_stats->tx_errors = priv->stats.tx_dropped;
}

/* HWMON temperature sensor support */
static int mxl371x_hwmon_read(struct device *dev, enum hwmon_sensor_types type,
			      u32 attr, int channel, long *val)
{
	struct phy_device *phydev = dev_get_drvdata(dev);
	u32 t0, t1;
	int ret, temp;

	if (type != hwmon_temp)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_temp_input:
		ret = mxl371x_read_temp_raw(phydev, &t0, &t1);
		if (ret < 0)
			return ret;

		temp = mxl371x_calc_temp(t0, t1);
		if (temp == -EINVAL)
			return -EINVAL;

		*val = temp;
		break;

	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

static umode_t mxl371x_hwmon_is_visible(const void *data,
					enum hwmon_sensor_types type,
					u32 attr, int channel)
{
	if (type != hwmon_temp)
		return 0;

	switch (attr) {
	case hwmon_temp_input:
		return 0444;
	default:
		return 0;
	}
}

static const struct hwmon_ops mxl371x_hwmon_ops = {
	.is_visible = mxl371x_hwmon_is_visible,
	.read = mxl371x_hwmon_read,
};

static const struct hwmon_channel_info *mxl371x_hwmon_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	NULL
};

static const struct hwmon_chip_info mxl371x_hwmon_chip_info = {
	.ops = &mxl371x_hwmon_ops,
	.info = mxl371x_hwmon_info,
};

static int mxl371x_hwmon_init(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	struct device *dev = &phydev->mdio.dev;
	struct device *hwmon_dev;

	hwmon_dev = devm_hwmon_device_register_with_info(dev, "mxl371x",
							 phydev,
							 &mxl371x_hwmon_chip_info,
							 NULL);
	if (IS_ERR(hwmon_dev))
		return PTR_ERR(hwmon_dev);

	priv->hwmon_dev = hwmon_dev;
	return 0;
}

/* Check if firmware is already running (warm boot) */
static int mxl371x_check_firmware_running(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	u32 fw_status;
	int ret;

	ret = mxl371x_read_mem32(phydev, MXL371X_FW_STATUS_REG, &fw_status);
	if (ret < 0) {
		dev_warn(dev, "Cannot read firmware status: %d\n", ret);
		return 0;
	}

	if (fw_status & MXL371X_FW_RUNNING) {
		dev_info(dev, "Firmware already running (warm boot detected)\n");
		return 1;
	}

	if (fw_status & MXL371X_FW_ERROR) {
		dev_warn(dev, "Firmware in error state, will reload\n");
		return 0;
	}

	return 0;
}

/* MoCA GUID management */
static int mxl371x_set_default_guid(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	u32 mac_hi, mac_lo;
	u8 mac[ETH_ALEN];
	int ret;

	/* 1. Check if already set in hardware (non-zero) */
	ret = mxl371x_read_mem32(phydev, MOCA_MAC_ADDR_HI, &mac_hi);
	if (ret == 0 && mac_hi != 0) {
		ret = mxl371x_read_mem32(phydev, MOCA_MAC_ADDR_LO, &mac_lo);
		if (ret == 0 && (mac_hi != 0 || mac_lo != 0)) {
			mac[0] = (mac_hi >> 24) & 0xff;
			mac[1] = (mac_hi >> 16) & 0xff;
			mac[2] = (mac_hi >> 8) & 0xff;
			mac[3] = (mac_hi >> 0) & 0xff;
			mac[4] = (mac_lo >> 24) & 0xff;
			mac[5] = (mac_lo >> 16) & 0xff;

			dev_info(dev, "Using existing MoCA GUID: %pM\n", mac);
			return 0;
		}
	}

	/* 2. Try to get from device tree */
	if (dev->of_node) {
		/* of_get_mac_address returns 0 on success */
		if (of_get_mac_address(dev->of_node, mac) == 0) {
			dev_info(dev, "Using MoCA GUID from device tree: %pM\n", mac);
			goto set_guid;
		}
	}

	/* 3. Try to derive from attached netdev (if available) */
	if (phydev->attached_dev && 
	    !is_zero_ether_addr(phydev->attached_dev->dev_addr)) {
		ether_addr_copy(mac, phydev->attached_dev->dev_addr);
		/* Modify to distinguish from host MAC */
		mac[0] |= 0x02;  /* Set locally administered bit */
		mac[5] ^= 0x01;  /* Flip last bit */
		dev_info(dev, "Using MoCA GUID derived from netdev: %pM\n", mac);
		goto set_guid;
	}

	/* 4. Generate using MaxLinear OUI + random bytes */
	/* MaxLinear OUI: 00:24:3E, with locally administered bit set */
	mac[0] = 0x02;  /* Locally administered (bit 1 set) */
	mac[1] = 0x24;  /* MaxLinear OUI byte 2 */
	mac[2] = 0x3e;  /* MaxLinear OUI byte 3 */

	/* Random bytes for last 3 octets */
	get_random_bytes(&mac[3], 3);

	dev_info(dev, "Generated MoCA GUID: %pM (MaxLinear OUI + random)\n", mac);

set_guid:
	mac_hi = (mac[0] << 24) | (mac[1] << 16) | (mac[2] << 8) | mac[3];
	mac_lo = (mac[4] << 24) | (mac[5] << 16);

	ret = mxl371x_write_mem32(phydev, MOCA_MAC_ADDR_HI, mac_hi);
	if (ret < 0)
		return ret;

	ret = mxl371x_write_mem32(phydev, MOCA_MAC_ADDR_LO, mac_lo);
	if (ret < 0)
		return ret;

	return 0;
}

/* Sysfs attributes */
static ssize_t moca_link_status_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;
	const char *status;

	switch (priv->link_status) {
	case MOCA_LINK_UP:
		status = "up";
		break;
	case MOCA_LINK_SCANNING:
		status = "scanning";
		break;
	default:
		status = "down";
		break;
	}

	return sprintf(buf, "%s\n", status);
}
static DEVICE_ATTR_RO(moca_link_status);

static ssize_t moca_version_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%u.%u\n", priv->moca_version >> 4,
		       priv->moca_version & 0xf);
}
static DEVICE_ATTR_RO(moca_version);

static ssize_t moca_phy_rate_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%u\n", priv->phy_rate);
}
static DEVICE_ATTR_RO(moca_phy_rate);

static ssize_t moca_node_id_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%u\n", priv->node_id);
}
static DEVICE_ATTR_RO(moca_node_id);

static ssize_t moca_nc_node_id_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%u\n", priv->nc_node_id);
}
static DEVICE_ATTR_RO(moca_nc_node_id);

static ssize_t moca_lof_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%u\n", priv->lof);
}
static DEVICE_ATTR_RO(moca_lof);

static ssize_t moca_network_state_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;
	const char *state;

	switch (priv->network_state) {
	case MOCA_NET_STATE_NETWORK_MODE:
		state = "network";
		break;
	case MOCA_NET_STATE_SEARCHING:
		state = "searching";
		break;
	default:
		state = "idle";
		break;
	}

	return sprintf(buf, "%s\n", state);
}
static DEVICE_ATTR_RO(moca_network_state);

static ssize_t moca_active_nodes_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "0x%08x\n", priv->active_nodes);
}
static DEVICE_ATTR_RO(moca_active_nodes);

static ssize_t moca_security_enabled_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%u\n", priv->security_enabled ? 1 : 0);
}
static DEVICE_ATTR_RO(moca_security_enabled);

static ssize_t moca_chip_type_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%s\n",
		       priv->soc_chip_type == MXL_MOCA_SOC_TYPE_LEUCADIA ?
		       "leucadia" : "cardiff");
}
static DEVICE_ATTR_RO(moca_chip_type);

static ssize_t moca_fw_version_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	struct mxl371x_priv *priv = phydev->priv;

	return sprintf(buf, "%s\n", priv->soc_version);
}
static DEVICE_ATTR_RO(moca_fw_version);

/* MoCA GUID - read/write */
static ssize_t moca_guid_show(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct phy_device *phydev = to_phy_device(dev);
	u32 mac_hi, mac_lo;
	u8 mac[ETH_ALEN];

	if (mxl371x_read_mem32(phydev, MOCA_MAC_ADDR_HI, &mac_hi) < 0)
		return -EIO;
	if (mxl371x_read_mem32(phydev, MOCA_MAC_ADDR_LO, &mac_lo) < 0)
		return -EIO;

	mac[0] = (mac_hi >> 24) & 0xff;
	mac[1] = (mac_hi >> 16) & 0xff;
	mac[2] = (mac_hi >> 8) & 0xff;
	mac[3] = (mac_hi >> 0) & 0xff;
	mac[4] = (mac_lo >> 24) & 0xff;
	mac[5] = (mac_lo >> 16) & 0xff;

	return sprintf(buf, "%pM\n", mac);
}

static ssize_t moca_guid_store(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct phy_device *phydev = to_phy_device(dev);
	u8 mac[ETH_ALEN];
	u32 mac_hi, mac_lo;

	if (!mac_pton(buf, mac))
		return -EINVAL;

	/* Allow any non-zero MAC for MoCA GUID */
	if (is_zero_ether_addr(mac))
		return -EADDRNOTAVAIL;

	mac_hi = (mac[0] << 24) | (mac[1] << 16) | (mac[2] << 8) | mac[3];
	mac_lo = (mac[4] << 24) | (mac[5] << 16);

	if (mxl371x_write_mem32(phydev, MOCA_MAC_ADDR_HI, mac_hi) < 0)
		return -EIO;
	if (mxl371x_write_mem32(phydev, MOCA_MAC_ADDR_LO, mac_lo) < 0)
		return -EIO;

	dev_info(dev, "MoCA GUID set to %pM\n", mac);
	return count;
}
static DEVICE_ATTR_RW(moca_guid);

static struct attribute *mxl371x_attrs[] = {
	&dev_attr_moca_link_status.attr,
	&dev_attr_moca_version.attr,
	&dev_attr_moca_phy_rate.attr,
	&dev_attr_moca_node_id.attr,
	&dev_attr_moca_nc_node_id.attr,
	&dev_attr_moca_lof.attr,
	&dev_attr_moca_network_state.attr,
	&dev_attr_moca_active_nodes.attr,
	&dev_attr_moca_security_enabled.attr,
	&dev_attr_moca_chip_type.attr,
	&dev_attr_moca_fw_version.attr,
	&dev_attr_moca_guid.attr,
	NULL,
};

static const struct attribute_group mxl371x_attr_group = {
	.attrs = mxl371x_attrs,
};

static int mxl371x_get_device_info(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	struct device *dev = &phydev->mdio.dev;
	u32 val;
	int ret;

	ret = mxl371x_read_mem32(phydev, SRE_PRODUCT_FAMILY_ID, &val);
	if (ret < 0)
		return ret;

	dev_info(dev, "Product Family ID: 0x%08x\n", val);

	ret = mxl371x_read_mem32(phydev, SRE_DEVICE_ID, &val);
	if (ret < 0)
		return ret;

	priv->device_id = val & 0xffff;
	priv->revision_id = (val >> SRE_REVISION_ID_OFFSET) & 0xffff;

	if (priv->device_id == 0x3710 || priv->device_id == 0x3711) {
		priv->soc_chip_type = MXL_MOCA_SOC_TYPE_LEUCADIA;
		priv->fw_name = MXL371X_FW_LEUCADIA;
	} else {
		priv->soc_chip_type = MXL_MOCA_SOC_TYPE_CARDIFF;
		priv->fw_name = MXL371X_FW_CARDIFF;
	}

	snprintf(priv->soc_version, sizeof(priv->soc_version),
		 "%s Device 0x%04x Rev 0x%04x",
		 priv->soc_chip_type == MXL_MOCA_SOC_TYPE_LEUCADIA ?
		 "Leucadia" : "Cardiff", priv->device_id, priv->revision_id);

	dev_info(dev, "%s\n", priv->soc_version);

	return 0;
}

static int mxl371x_load_firmware(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	const struct firmware *fw;
	struct device *dev = &phydev->mdio.dev;
	int ret;
	u32 i, fw_status;

	/* Check if already loaded */
	if (priv->fw_loaded)
		return 0;

	/* Check if firmware is already running (warm boot) */
	if (mxl371x_check_firmware_running(phydev)) {
		priv->fw_loaded = true;
		dev_info(dev, "Skipping firmware load (already running)\n");
		return 0;
	}

	dev_info(dev, "Loading firmware %s...\n", priv->fw_name);

	ret = request_firmware(&fw, priv->fw_name, dev);
	if (ret) {
		dev_err(dev, "Failed to load firmware: %d\n", ret);
		return ret;
	}

	if (fw->size == 0 || fw->size > MXL371X_MAX_FW_SIZE) {
		dev_err(dev, "Invalid firmware size: %zu\n", fw->size);
		ret = -EINVAL;
		goto release_fw;
	}

	dev_info(dev, "Firmware size: %zu bytes\n", fw->size);

	/* Reset SoC - hold in reset */
	ret = mxl371x_write_mem32(phydev, SRE_CPU_SRC_SEL_CSR, 0x8);
	if (ret < 0) {
		dev_err(dev, "Failed to reset SoC\n");
		goto release_fw;
	}
	msleep(100);

	/* Upload firmware in chunks */
	dev_info(dev, "Uploading firmware...\n");
	for (i = 0; i < fw->size; i += 4) {
		u32 word = 0;
		int j;

		/* Construct 32-bit word from firmware bytes */
		for (j = 0; j < 4 && (i + j) < fw->size; j++)
			word |= ((u32)fw->data[i + j]) << (j * 8);

		ret = mxl371x_write_mem32(phydev, MXL371X_FW_BASE_ADDR + i, word);
		if (ret < 0) {
			dev_err(dev, "Firmware write failed at offset %u\n", i);
			goto release_fw;
		}

		/* Progress indication every 256KB */
		if (i % 262144 == 0 && i > 0)
			dev_dbg(dev, "Uploaded %zu%%\n", (i * 100) / fw->size);
	}

	dev_info(dev, "Firmware upload complete (%zu bytes)\n", fw->size);

	/* Release SoC from reset */
	ret = mxl371x_write_mem32(phydev, SRE_CPU_SRC_SEL_CSR, 0x0);
	if (ret < 0) {
		dev_err(dev, "Failed to release SoC from reset\n");
		goto release_fw;
	}

	/* Wait for firmware to initialize */
	dev_info(dev, "Waiting for firmware to start...\n");
	msleep(500);

	/* Poll for firmware ready status */
	for (i = 0; i < 50; i++) {
		ret = mxl371x_read_mem32(phydev, MXL371X_FW_STATUS_REG, &fw_status);
		if (ret < 0) {
			dev_err(dev, "Cannot read firmware status\n");
			goto release_fw;
		}

		if (fw_status & MXL371X_FW_RUNNING) {
			dev_info(dev, "Firmware started successfully\n");
			priv->fw_loaded = true;
			ret = 0;
			goto release_fw;
		}

		if (fw_status & MXL371X_FW_ERROR) {
			dev_err(dev, "Firmware error detected (status: 0x%08x)\n", 
				fw_status);
			ret = -EIO;
			goto release_fw;
		}

		msleep(100);
	}

	dev_err(dev, "Firmware start timeout (status: 0x%08x)\n", fw_status);
	ret = -ETIMEDOUT;

release_fw:
	release_firmware(fw);
	return ret;
}

/* Detect current SGMII/HSGMII configuration from hardware */
static int mxl371x_detect_sgmii_mode(struct phy_device *phydev, u8 *detected_mode)
{
	struct device *dev = &phydev->mdio.dev;
	int ret;
	u16 val;

	/* Read current SGMII configuration from hardware */
	ret = phy_read_paged(phydev, MXL371X_SGMII_CTRL, 0x10);
	if (ret < 0) {
		dev_err(dev, "Failed to read SGMII config: %d\n", ret);
		return ret;
	}

	val = ret & MXL371X_SGMII_MODE_MASK;

	switch (val) {
	case MXL371X_SGMII_MODE_SGMII:
		*detected_mode = MXL371X_SGMII_MODE_SGMII;
		dev_info(dev, "Detected SGMII mode (1000Mbps)\n");
		break;

	case MXL371X_SGMII_MODE_HSGMII:
		*detected_mode = MXL371X_SGMII_MODE_HSGMII;
		dev_info(dev, "Detected HSGMII mode (2500Mbps)\n");
		break;

	case MXL371X_SGMII_MODE_1000BASE_X:
		*detected_mode = MXL371X_SGMII_MODE_1000BASE_X;
		dev_info(dev, "Detected 1000BASE-X mode\n");
		break;

	default:
		dev_warn(dev, "Unknown SGMII mode: 0x%02x\n", val);
		return -EINVAL;
	}

	return 0;
}

static int mxl371x_config_sgmii(struct phy_device *phydev)
{
	struct device *dev = &phydev->mdio.dev;
	int ret;
	u8 mode;
	bool mode_set = false;

	/* 1. Try to use interface mode from device tree */
	switch (phydev->interface) {
	case PHY_INTERFACE_MODE_SGMII:
		mode = MXL371X_SGMII_MODE_SGMII;
		phydev->speed = SPEED_1000;
		mode_set = true;
		dev_info(dev, "Using SGMII mode from device tree (1000Mbps)\n");
		break;

	case PHY_INTERFACE_MODE_2500BASEX:
		mode = MXL371X_SGMII_MODE_HSGMII;
		phydev->speed = SPEED_2500;
		mode_set = true;
		dev_info(dev, "Using HSGMII mode from device tree (2500Mbps)\n");
		break;

	case PHY_INTERFACE_MODE_1000BASEX:
		mode = MXL371X_SGMII_MODE_1000BASE_X;
		phydev->speed = SPEED_1000;
		mode_set = true;
		dev_info(dev, "Using 1000BASE-X mode from device tree\n");
		break;

	default:
		/* 2. Try to detect current hardware configuration (warm boot) */
		ret = mxl371x_detect_sgmii_mode(phydev, &mode);
		if (ret == 0) {
			/* Successfully detected existing mode */
			mode_set = true;
			dev_info(dev, "Using detected hardware configuration\n");

			/* Set speed based on detected mode */
			if (mode == MXL371X_SGMII_MODE_HSGMII)
				phydev->speed = SPEED_2500;
			else
				phydev->speed = SPEED_1000;
		}
		break;
	}

	/* 3. If still not set, make an educated guess */
	if (!mode_set) {
		dev_warn(dev, "No device tree phy-mode and detection failed\n");
		dev_info(dev, "Defaulting to SGMII @ 1000Mbps\n");

		mode = MXL371X_SGMII_MODE_SGMII;
		phydev->speed = SPEED_1000;

		/* Could also check MoCA version as fallback:
		 * if (priv->moca_version >= MOCA_VER_2_5) {
		 * mode = MXL371X_SGMII_MODE_HSGMII;
		 * phydev->speed = SPEED_2500;
		 * }
		 */
	}

	/* Configure the SGMII interface */
	ret = phy_modify_paged(phydev, MXL371X_SGMII_CTRL, 0x10,
			       MXL371X_SGMII_MODE_MASK, mode);
	if (ret < 0) {
		dev_err(dev, "Failed to configure SGMII mode: %d\n", ret);
		return ret;
	}

	phydev->duplex = DUPLEX_FULL;
	dev_info(dev, "Configured %s @ %dMbps\n",
		 mode == MXL371X_SGMII_MODE_HSGMII ? "HSGMII" :
		 mode == MXL371X_SGMII_MODE_1000BASE_X ? "1000BASE-X" : "SGMII",
		 phydev->speed);

	return 0;
}

static int mxl371x_config_init(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	struct device *dev = &phydev->mdio.dev;
	int ret;
	bool warm_boot = false;

	/* Get device information */
	ret = mxl371x_get_device_info(phydev);
	if (ret < 0)
		return ret;

	/* Check if this is a warm boot */
	warm_boot = mxl371x_check_firmware_running(phydev);
	if (warm_boot) {
		priv->fw_loaded = true;
		dev_info(dev, "Warm boot detected, skipping firmware load\n");
	}

	/* Load firmware (will be skipped if already running) */
	ret = mxl371x_load_firmware(phydev);
	if (ret < 0) {
		dev_err(dev, "Firmware loading failed: %d\n", ret);
		return ret;
	}

	/* Set default MoCA GUID if not already configured
	 * On warm boot, this will use existing GUID from hardware */
	ret = mxl371x_set_default_guid(phydev);
	if (ret < 0)
		dev_warn(dev, "Failed to set MoCA GUID: %d\n", ret);

	/* Read current MoCA status */
	mxl371x_read_moca_status(phydev);

	/* Configure SGMII/HSGMII interface
	 * Uses device tree phy-mode if set, otherwise detects from hardware */
	ret = mxl371x_config_sgmii(phydev);
	if (ret < 0) {
		dev_err(dev, "SGMII configuration failed: %d\n", ret);
		return ret;
	}

	/* Create sysfs attributes */
	ret = sysfs_create_group(&phydev->mdio.dev.kobj, &mxl371x_attr_group);
	if (ret < 0) {
		dev_err(dev, "Failed to create sysfs attributes: %d\n", ret);
		return ret;
	}

	/* Initialize hwmon temperature sensor */
	ret = mxl371x_hwmon_init(phydev);
	if (ret < 0)
		dev_warn(dev, "Failed to init hwmon: %d\n", ret);

	/* Start statistics polling */
	INIT_DELAYED_WORK(&priv->stats_poll, mxl371x_stats_poll_work);
	schedule_delayed_work(&priv->stats_poll, HZ);

	if (warm_boot) {
		dev_info(dev, "MoCA PHY initialized (warm boot, MoCA v%u.%u, %uMbps)\n",
			 priv->moca_version >> 4, priv->moca_version & 0xf,
			 phydev->speed);
	} else {
		dev_info(dev, "MoCA PHY initialized (cold boot, MoCA v%u.%u, %uMbps)\n",
			 priv->moca_version >> 4, priv->moca_version & 0xf,
			 phydev->speed);
	}

	return 0;
}

static int mxl371x_probe(struct phy_device *phydev)
{
	struct mxl371x_priv *priv;

	priv = devm_kzalloc(&phydev->mdio.dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	phydev->priv = priv;
	return 0;
}

static void mxl371x_remove(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;

	cancel_delayed_work_sync(&priv->stats_poll);
	sysfs_remove_group(&phydev->mdio.dev.kobj, &mxl371x_attr_group);
}

static int mxl371x_read_status(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;
	int ret;

	ret = genphy_read_status(phydev);
	if (ret < 0)
		return ret;

	if (priv->fw_loaded) {
		mxl371x_read_moca_status(phydev);
		phydev->link = (priv->link_status == MOCA_LINK_UP);
	}

	return 0;
}

static int mxl371x_config_aneg(struct phy_device *phydev)
{
	phydev->autoneg = AUTONEG_DISABLE;
	phydev->duplex = DUPLEX_FULL;
	return 0;
}

static int mxl371x_suspend(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;

	cancel_delayed_work_sync(&priv->stats_poll);
	return genphy_suspend(phydev);
}

static int mxl371x_resume(struct phy_device *phydev)
{
	struct mxl371x_priv *priv = phydev->priv;

	priv->fw_loaded = false;
	schedule_delayed_work(&priv->stats_poll, HZ);
	return genphy_resume(phydev);
}

static int mxl371x_match_phy_device(struct phy_device *phydev,
				    const struct phy_driver *drv)
{
	return ((phydev->phy_id & MXL371X_OUI_MASK) == MXL371X_OUI);
}

static struct phy_driver mxl371x_drivers[] = {
	{
		PHY_ID_MATCH_EXACT(MXL3710_PHY_ID),
		.name		= "MaxLinear MXL3710 MoCA 2.5",
		.probe		= mxl371x_probe,
		.remove		= mxl371x_remove,
		.config_init	= mxl371x_config_init,
		.config_aneg	= mxl371x_config_aneg,
		.read_status	= mxl371x_read_status,
		.get_phy_stats	= mxl371x_get_phy_stats,
		.suspend	= mxl371x_suspend,
		.resume		= mxl371x_resume,
		.read_page	= mxl371x_read_page,
		.write_page	= mxl371x_write_page,
	}, {
		PHY_ID_MATCH_EXACT(MXL3711_PHY_ID),
		.name		= "MaxLinear MXL3711 MoCA 2.5",
		.probe		= mxl371x_probe,
		.remove		= mxl371x_remove,
		.config_init	= mxl371x_config_init,
		.config_aneg	= mxl371x_config_aneg,
		.read_status	= mxl371x_read_status,
		.get_phy_stats	= mxl371x_get_phy_stats,
		.suspend	= mxl371x_suspend,
		.resume		= mxl371x_resume,
		.read_page	= mxl371x_read_page,
		.write_page	= mxl371x_write_page,
	}, {
		.match_phy_device = mxl371x_match_phy_device,
		.name		= "MaxLinear MXL371x MoCA 2.5",
		.probe		= mxl371x_probe,
		.remove		= mxl371x_remove,
		.config_init	= mxl371x_config_init,
		.config_aneg	= mxl371x_config_aneg,
		.read_status	= mxl371x_read_status,
		.get_phy_stats	= mxl371x_get_phy_stats,
		.suspend	= mxl371x_suspend,
		.resume		= mxl371x_resume,
		.read_page	= mxl371x_read_page,
		.write_page	= mxl371x_write_page,
	},
};

module_phy_driver(mxl371x_drivers);

static const struct mdio_device_id __maybe_unused mxl371x_tbl[] = {
	{ PHY_ID_MATCH_VENDOR(MXL371X_OUI) },
	{ PHY_ID_MATCH_EXACT(MXL3710_PHY_ID) },
	{ PHY_ID_MATCH_EXACT(MXL3711_PHY_ID) },
	{ }
};

MODULE_DEVICE_TABLE(mdio, mxl371x_tbl);
MODULE_FIRMWARE(MXL371X_FW_LEUCADIA);
MODULE_FIRMWARE(MXL371X_FW_CARDIFF);
MODULE_DESCRIPTION("MaxLinear MXL371x MoCA 2.5 PHY driver");
MODULE_AUTHOR("Kenneth Kasilag");
MODULE_LICENSE("GPL");
