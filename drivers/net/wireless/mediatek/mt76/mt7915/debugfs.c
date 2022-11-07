// SPDX-License-Identifier: ISC
/* Copyright (C) 2020 MediaTek Inc. */

#include <linux/relay.h>
#include "mt7915.h"
#include "eeprom.h"
#include "mcu.h"
#include "mac.h"

#define FW_BIN_LOG_MAGIC	0x44e98caf
#ifdef MTK_DEBUG
#define FW_BIN_LOG_MAGIC_V2	0x44d9c99a
#endif

/** global debugfs **/

struct hw_queue_map {
	const char *name;
	u8 index;
	u8 pid;
	u8 qid;
};

static int
mt7915_implicit_txbf_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	/* The existing connected stations shall reconnect to apply
	 * new implicit txbf configuration.
	 */
	dev->ibf = !!val;

	return mt7915_mcu_set_txbf(dev, MT_BF_TYPE_UPDATE);
}

static int
mt7915_implicit_txbf_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;

	*val = dev->ibf;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_implicit_txbf, mt7915_implicit_txbf_get,
			 mt7915_implicit_txbf_set, "%lld\n");

/* test knob of system error recovery */
static ssize_t
mt7915_fw_ser_set(struct file *file, const char __user *user_buf,
		  size_t count, loff_t *ppos)
{
#define SER_LEVEL	GENMASK(3, 0)
#define SER_ACTION	GENMASK(11, 8)

	struct mt7915_phy *phy = file->private_data;
	struct mt7915_dev *dev = phy->dev;
	u8 ser_action, ser_set, set_val;
	u8 band_idx = phy->band_idx;
	char buf[16];
	int ret = 0;
	u16 val;
	u32 intr;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	if (kstrtou16(buf, 0, &val))
		return -EINVAL;

	ser_action = FIELD_GET(SER_ACTION, val);
	ser_set = set_val = FIELD_GET(SER_LEVEL, val);

	switch (ser_action) {
	case SER_QUERY:
		/* grab firmware SER stats */
		ser_set = 0;
		break;
	case SER_SET:
		/*
		 * 0x100: disable system error recovery function.
		 * 0x101: enable system error recovery function.
		 * 0x103: enable l0.5 recover function.
		 */
		ser_set = !!set_val;

		dev->ser.reset_enable = ser_set;
		intr = mt76_rr(dev, MT_WFDMA0_MCU_HOST_INT_ENA);
		if (dev->ser.reset_enable)
			intr |= MT_MCU_CMD_WDT_MASK;
		else
			intr &= ~MT_MCU_CMD_WDT_MASK;
		mt76_set(dev, MT_WFDMA0_MCU_HOST_INT_ENA, intr);
		break;
	case SER_ENABLE:
		/*
		 * 0x200: enable system error tracking.
		 * 0x201: enable system error L1 recover.
		 * 0x202: enable system error L2 recover.
		 * 0x203: enable system error L3 rx abort.
		 * 0x204: enable system error L3 tx abort.
		 * 0x205: enable system error L3 tx disable.
		 * 0x206: enable system error L3 bf recover.
		 * 0x207: enable system error all recover.
		 */
		ser_set = set_val > 7 ? 0x7f : BIT(set_val);
		break;
	case SER_RECOVER:
		/*
		 * 0x300: trigger L0.5 recover.
		 * 0x301: trigger L1 recover.
		 * 0x302: trigger L2 recover.
		 * 0x303: trigger L3 rx abort.
		 * 0x304: trigger L3 tx abort
		 * 0x305: trigger L3 tx disable.
		 * 0x306: trigger L3 bf recover.
		 */
		if (!ser_set) {
			if (dev->ser.reset_enable) {
				dev->reset_state |= MT_MCU_CMD_WDT_MASK;
				mt7915_reset(dev);
			} else {
				dev_info(dev->mt76.dev, "SER: chip full recovery not enable\n");
			}
			goto out;
		}
		break;
	default:
		goto out;
	}
	ret = mt7915_mcu_set_ser(dev, ser_action, ser_set, band_idx);
	if (ret)
		return ret;
out:
	return count;
}

static ssize_t
mt7915_fw_ser_get(struct file *file, char __user *user_buf,
		  size_t count, loff_t *ppos)
{
	struct mt7915_phy *phy = file->private_data;
	struct mt7915_dev *dev = phy->dev;
	char *buff;
	int desc = 0;
	ssize_t ret;
	static const size_t bufsz = 600;

	buff = kmalloc(bufsz, GFP_KERNEL);
	if (!buff)
		return -ENOMEM;

	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_STATUS        = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_SER_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PLE_ERR       = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PLE_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PLE_ERR_1     = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PLE1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PLE_ERR_AMSDU = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PLE_AMSDU_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PSE_ERR       = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PSE_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_PSE_ERR_1     = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_PSE1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR6_B0 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR6_BN0_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR6_B1 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR6_BN1_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR7_B0 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR7_BN0_STATS));
	desc += scnprintf(buff + desc, bufsz - desc,
			  "::E  R , SER_LMAC_WISR7_B1 = 0x%08x\n",
			  mt76_rr(dev, MT_SWDEF_LAMC_WISR7_BN1_STATS));

	desc += scnprintf(buff + desc, bufsz - desc,
			  "\nWF RESET STATUS: EN %d, WM %d, WA %d\n",
			  dev->ser.reset_enable,
			  dev->ser.wf_reset_wm_count,
			  dev->ser.wf_reset_wa_count);

	ret = simple_read_from_buffer(user_buf, count, ppos, buff, desc);
	kfree(buff);
	return ret;
}

static const struct file_operations mt7915_fw_ser_ops = {
	.write = mt7915_fw_ser_set,
	.read = mt7915_fw_ser_get,
	.open = simple_open,
	.llseek = default_llseek,
};

static int
mt7915_radar_trigger(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	if (val > MT_RX_SEL2)
		return -EINVAL;

	return mt76_connac_mcu_rdd_cmd(&dev->mt76, RDD_RADAR_EMULATE,
				       val, 0, 0);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_radar_trigger, NULL,
			 mt7915_radar_trigger, "%lld\n");

static int
mt7915_muru_debug_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	dev->muru_debug = val;
	mt7915_mcu_muru_debug_set(dev, dev->muru_debug);

	return 0;
}

static int
mt7915_muru_debug_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;

	*val = dev->muru_debug;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_muru_debug, mt7915_muru_debug_get,
			 mt7915_muru_debug_set, "%lld\n");

static int mt7915_muru_stats_show(struct seq_file *file, void *data)
{
	struct mt7915_phy *phy = file->private;
	struct mt7915_dev *dev = phy->dev;
	static const char * const dl_non_he_type[] = {
		"CCK", "OFDM", "HT MIX", "HT GF",
		"VHT SU", "VHT 2MU", "VHT 3MU", "VHT 4MU"
	};
	static const char * const dl_he_type[] = {
		"HE SU", "HE EXT", "HE 2MU", "HE 3MU", "HE 4MU",
		"HE 2RU", "HE 3RU", "HE 4RU", "HE 5-8RU", "HE 9-16RU",
		"HE >16RU"
	};
	static const char * const ul_he_type[] = {
		"HE 2MU", "HE 3MU", "HE 4MU", "HE SU", "HE 2RU",
		"HE 3RU", "HE 4RU", "HE 5-8RU", "HE 9-16RU", "HE >16RU"
	};
	int ret, i;
	u64 total_ppdu_cnt, sub_total_cnt;

	if (!dev->muru_debug) {
		seq_puts(file, "Please enable muru_debug first.\n");
		return 0;
	}

	mutex_lock(&dev->mt76.mutex);

	ret = mt7915_mcu_muru_debug_get(phy);
	if (ret)
		goto exit;

	/* Non-HE Downlink*/
	seq_puts(file, "[Non-HE]\nDownlink\nData Type:  ");

	for (i = 0; i < 5; i++)
		seq_printf(file, "%8s | ", dl_non_he_type[i]);

#define __dl_u32(s)     phy->mib.rx_##s
	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | %8u | %8u | %8u | ",
		   __dl_u32(cck_cnt),
		   __dl_u32(ofdm_cnt),
		   __dl_u32(htmix_cnt),
		   __dl_u32(htgf_cnt),
		   __dl_u32(vht_su_cnt));

	seq_puts(file, "\nDownlink MU-MIMO\nData Type:  ");

	for (i = 5; i < 8; i++)
		seq_printf(file, "%8s | ", dl_non_he_type[i]);

	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | %8u | ",
		   __dl_u32(vht_2mu_cnt),
		   __dl_u32(vht_3mu_cnt),
		   __dl_u32(vht_4mu_cnt));

	sub_total_cnt = __dl_u32(vht_2mu_cnt) +
		__dl_u32(vht_3mu_cnt) +
		__dl_u32(vht_4mu_cnt);

	seq_printf(file, "\nTotal non-HE MU-MIMO DL PPDU count: %lld",
		   sub_total_cnt);

	total_ppdu_cnt = sub_total_cnt +
		__dl_u32(cck_cnt) +
		__dl_u32(ofdm_cnt) +
		__dl_u32(htmix_cnt) +
		__dl_u32(htgf_cnt) +
		__dl_u32(vht_su_cnt);

	seq_printf(file, "\nAll non-HE DL PPDU count: %lld", total_ppdu_cnt);

	/* HE Downlink */
	seq_puts(file, "\n\n[HE]\nDownlink\nData Type:  ");

	for (i = 0; i < 2; i++)
		seq_printf(file, "%8s | ", dl_he_type[i]);

	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | ",
		   __dl_u32(he_su_cnt),
		   __dl_u32(he_ext_su_cnt));

	seq_puts(file, "\nDownlink MU-MIMO\nData Type:  ");

	for (i = 2; i < 5; i++)
		seq_printf(file, "%8s | ", dl_he_type[i]);

	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | %8u | ",
		   __dl_u32(he_2mu_cnt),
		   __dl_u32(he_3mu_cnt),
		   __dl_u32(he_4mu_cnt));

	seq_puts(file, "\nDownlink OFDMA\nData Type:  ");

	for (i = 5; i < 11; i++)
		seq_printf(file, "%8s | ", dl_he_type[i]);

	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | %8u | %8u | %9u | %8u | ",
		   __dl_u32(he_2ru_cnt),
		   __dl_u32(he_3ru_cnt),
		   __dl_u32(he_4ru_cnt),
		   __dl_u32(he_5to8ru_cnt),
		   __dl_u32(he_9to16ru_cnt),
		   __dl_u32(he_gtr16ru_cnt));

	sub_total_cnt = __dl_u32(he_2mu_cnt) +
		__dl_u32(he_3mu_cnt) +
		__dl_u32(he_4mu_cnt);
	total_ppdu_cnt = sub_total_cnt;

	seq_printf(file, "\nTotal HE MU-MIMO DL PPDU count: %lld",
		   sub_total_cnt);

	sub_total_cnt = __dl_u32(he_2ru_cnt) +
		__dl_u32(he_3ru_cnt) +
		__dl_u32(he_4ru_cnt) +
		__dl_u32(he_5to8ru_cnt) +
		__dl_u32(he_9to16ru_cnt) +
		__dl_u32(he_gtr16ru_cnt);
	total_ppdu_cnt += sub_total_cnt;

	seq_printf(file, "\nTotal HE OFDMA DL PPDU count: %lld",
		   sub_total_cnt);

	total_ppdu_cnt += __dl_u32(he_su_cnt) +
		__dl_u32(he_ext_su_cnt);

	seq_printf(file, "\nAll HE DL PPDU count: %lld", total_ppdu_cnt);
#undef __dl_u32

	/* HE Uplink */
	seq_puts(file, "\n\nUplink");
	seq_puts(file, "\nTrigger-based Uplink MU-MIMO\nData Type:  ");

	for (i = 0; i < 3; i++)
		seq_printf(file, "%8s | ", ul_he_type[i]);

#define __ul_u32(s)     phy->mib.tx_##s
	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | %8u | ",
		   __ul_u32(hetrig_2mu_cnt),
		   __ul_u32(hetrig_3mu_cnt),
		   __ul_u32(hetrig_4mu_cnt));

	seq_puts(file, "\nTrigger-based Uplink OFDMA\nData Type:  ");

	for (i = 3; i < 10; i++)
		seq_printf(file, "%8s | ", ul_he_type[i]);

	seq_puts(file, "\nTotal Count:");
	seq_printf(file, "%8u | %8u | %8u | %8u | %8u | %9u |  %7u | ",
		   __ul_u32(hetrig_su_cnt),
		   __ul_u32(hetrig_2ru_cnt),
		   __ul_u32(hetrig_3ru_cnt),
		   __ul_u32(hetrig_4ru_cnt),
		   __ul_u32(hetrig_5to8ru_cnt),
		   __ul_u32(hetrig_9to16ru_cnt),
		   __ul_u32(hetrig_gtr16ru_cnt));

	sub_total_cnt = __ul_u32(hetrig_2mu_cnt) +
		__ul_u32(hetrig_3mu_cnt) +
		__ul_u32(hetrig_4mu_cnt);
	total_ppdu_cnt = sub_total_cnt;

	seq_printf(file, "\nTotal HE MU-MIMO UL TB PPDU count: %lld",
		   sub_total_cnt);

	sub_total_cnt = __ul_u32(hetrig_2ru_cnt) +
		__ul_u32(hetrig_3ru_cnt) +
		__ul_u32(hetrig_4ru_cnt) +
		__ul_u32(hetrig_5to8ru_cnt) +
		__ul_u32(hetrig_9to16ru_cnt) +
		__ul_u32(hetrig_gtr16ru_cnt);
	total_ppdu_cnt += sub_total_cnt;

	seq_printf(file, "\nTotal HE OFDMA UL TB PPDU count: %lld",
		   sub_total_cnt);

	total_ppdu_cnt += __ul_u32(hetrig_su_cnt);

	seq_printf(file, "\nAll HE UL TB PPDU count: %lld\n", total_ppdu_cnt);
#undef __ul_u32

exit:
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}
DEFINE_SHOW_ATTRIBUTE(mt7915_muru_stats);

static int
mt7915_rdd_monitor(struct seq_file *s, void *data)
{
	struct mt7915_dev *dev = dev_get_drvdata(s->private);
	struct cfg80211_chan_def *chandef = &dev->rdd2_chandef;
	const char *bw;
	int ret = 0;

	mutex_lock(&dev->mt76.mutex);

	if (!cfg80211_chandef_valid(chandef)) {
		ret = -EINVAL;
		goto out;
	}

	if (!dev->rdd2_phy) {
		seq_puts(s, "not running\n");
		goto out;
	}

	switch (chandef->width) {
	case NL80211_CHAN_WIDTH_40:
		bw = "40";
		break;
	case NL80211_CHAN_WIDTH_80:
		bw = "80";
		break;
	case NL80211_CHAN_WIDTH_160:
		bw = "160";
		break;
	case NL80211_CHAN_WIDTH_80P80:
		bw = "80P80";
		break;
	default:
		bw = "20";
		break;
	}

	seq_printf(s, "channel %d (%d MHz) width %s MHz center1: %d MHz\n",
		   chandef->chan->hw_value, chandef->chan->center_freq,
		   bw, chandef->center_freq1);
out:
	mutex_unlock(&dev->mt76.mutex);

	return ret;
}

static int
_mt7915_fw_debug_wm_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	return mt7915_fw_debug_wm_set(dev, val);
}

static int
mt7915_fw_debug_wm_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;

#ifdef MTK_DEBUG
	*val = dev->fw.debug_wm & ~BIT(7);
#else
	val = dev->fw.debug_wm;
#endif

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug_wm, mt7915_fw_debug_wm_get,
			 _mt7915_fw_debug_wm_set, "%lld\n");

static int
_mt7915_fw_debug_wa_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	return mt7915_fw_debug_wa_set(dev, val);
}

static int
mt7915_fw_debug_wa_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;

	*val = dev->fw.debug_wa;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug_wa, mt7915_fw_debug_wa_get,
			 _mt7915_fw_debug_wa_set, "%lld\n");

static int
mt7915_fw_debug_bin_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	if (!mt7915_check_add_fwlog(dev))
		return -ENOMEM;

	dev->fw.debug_bin = val;

	relay_reset(dev->relay_fwlog);

#ifdef MTK_DEBUG
	dev->dbg.dump_mcu_pkt = val & BIT(4) ? true : false;
	dev->dbg.dump_txd = val & BIT(5) ? true : false;
	dev->dbg.dump_tx_pkt = val & BIT(6) ? true : false;
	dev->dbg.dump_rx_pkt = val & BIT(7) ? true : false;
	dev->dbg.dump_rx_raw = val & BIT(8) ? true : false;
	if (!(val & GENMASK(3, 0)))
		return 0;
#endif


	return mt7915_fw_debug_wm_set(dev, dev->fw.debug_wm);
}

static int
mt7915_fw_debug_bin_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;

	*val = dev->fw.debug_bin;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_fw_debug_bin, mt7915_fw_debug_bin_get,
			 mt7915_fw_debug_bin_set, "%lld\n");

static int
mt7915_fw_util_wm_show(struct seq_file *file, void *data)
{
	struct mt7915_dev *dev = file->private;

	seq_printf(file, "Program counter: 0x%x\n", mt76_rr(dev, MT_WM_MCU_PC));
	seq_printf(file, "Exception state: 0x%x\n",
		   is_mt7915(&dev->mt76) ?
		   (u32)mt76_get_field(dev, MT_FW_EXCEPTION, GENMASK(15, 8)) :
		   (u32)mt76_get_field(dev, MT_FW_EXCEPTION, GENMASK(7, 0)));

	if (dev->fw.debug_wm) {
		seq_printf(file, "Busy: %u%%  Peak busy: %u%%\n",
			   mt76_rr(dev, MT_CPU_UTIL_BUSY_PCT),
			   mt76_rr(dev, MT_CPU_UTIL_PEAK_BUSY_PCT));
		seq_printf(file, "Idle count: %u  Peak idle count: %u\n",
			   mt76_rr(dev, MT_CPU_UTIL_IDLE_CNT),
			   mt76_rr(dev, MT_CPU_UTIL_PEAK_IDLE_CNT));
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_fw_util_wm);

static int
mt7915_fw_util_wa_show(struct seq_file *file, void *data)
{
	struct mt7915_dev *dev = file->private;

	seq_printf(file, "Program counter: 0x%x\n", mt76_rr(dev, MT_WA_MCU_PC));

	if (dev->fw.debug_wa)
		return mt7915_mcu_wa_cmd(dev, MCU_WA_PARAM_CMD(QUERY),
					 MCU_WA_PARAM_CPU_UTIL, 0, 0);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_fw_util_wa);

struct mt7915_txo_worker_info {
	char *buf;
	int sofar;
	int size;
};

static void mt7915_txo_worker(void *wi_data, struct ieee80211_sta *sta)
{
	struct mt7915_txo_worker_info *wi = wi_data;
	struct mt7915_sta *msta = (struct mt7915_sta *)sta->drv_priv;
	struct mt76_testmode_data *td = &msta->test;
	struct ieee80211_vif *vif;
	struct wireless_dev *wdev;

	if (wi->sofar >= wi->size)
		return; /* buffer is full */

	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
	wdev = ieee80211_vif_to_wdev(vif);

	wi->sofar += scnprintf(wi->buf + wi->sofar, wi->size - wi->sofar,
			       "vdev (%s) active=%d tpc=%d sgi=%d mcs=%d nss=%d"
			       " pream=%d retries=%d dynbw=%d bw=%d\n",
			       wdev->netdev->name,
			       td->txo_active, td->tx_power[0],
			       td->tx_rate_sgi, td->tx_rate_idx,
			       td->tx_rate_nss, td->tx_rate_mode,
			       td->tx_xmit_count, td->tx_dynbw,
			       td->txbw);
}

static ssize_t mt7915_read_set_rate_override(struct file *file,
					     char __user *user_buf,
					     size_t count, loff_t *ppos)
{
	struct mt7915_dev *dev = file->private_data;
	struct ieee80211_hw *hw = dev->mphy.hw;
	char *buf2;
	int size = 8000;
	int rv, sofar;
	struct mt7915_txo_worker_info wi;
	const char buf[] =
		"This allows specify specif tx rate parameters for all DATA"
		" frames on a vdev\n"
		"To set a value, you specify the dev-name and key-value pairs:\n"
		"tpc=10 sgi=1 mcs=x nss=x pream=x retries=x dynbw=0|1 bw=x enable=0|1\n"
		"pream: 0=cck, 1=ofdm, 2=HT, 3=VHT, 4=HE_SU\n"
		"cck-mcs: 0=1Mbps, 1=2Mbps, 3=5.5Mbps, 3=11Mbps\n"
		"ofdm-mcs: 0=6Mbps, 1=9Mbps, 2=12Mbps, 3=18Mbps, 4=24Mbps, 5=36Mbps,"
		" 6=48Mbps, 7=54Mbps\n"
		"tpc: adjust power from defaults, in 1/2 db units 0 - 31, 16 is default\n"
		"bw is 0-3 for 20-160\n"
		" For example, wlan0:\n"
		"echo \"wlan0 tpc=255 sgi=1 mcs=0 nss=1 pream=3 retries=1 dynbw=0 bw=0"
		" active=1\" > ...mt76/set_rate_override\n";

	buf2 = kzalloc(size, GFP_KERNEL);
	if (!buf2)
		return -ENOMEM;
	strcpy(buf2, buf);
	sofar = strlen(buf2);

	wi.sofar = sofar;
	wi.buf = buf2;
	wi.size = size;

	ieee80211_iterate_stations_atomic(hw, mt7915_txo_worker, &wi);

	rv = simple_read_from_buffer(user_buf, count, ppos, buf2, wi.sofar);
	kfree(buf2);
	return rv;
}

/* Set the rates for specific types of traffic.
 */
static ssize_t mt7915_write_set_rate_override(struct file *file,
					      const char __user *user_buf,
					      size_t count, loff_t *ppos)
{
	struct mt7915_dev *dev = file->private_data;
	struct mt7915_sta *msta;
	struct ieee80211_vif *vif;
	struct mt76_testmode_data *td = NULL;
	struct wireless_dev *wdev;
	struct mt76_wcid *wcid;
	struct mt76_phy *mphy = &dev->mt76.phy;
	char buf[180];
	char tmp[20];
	char *tok;
	int ret, i, j;
	unsigned int vdev_id = 0xFFFF;
	char *bufptr = buf;
	long rc;
	char dev_name_match[IFNAMSIZ + 2];

	memset(buf, 0, sizeof(buf));

	simple_write_to_buffer(buf, sizeof(buf) - 1, ppos, user_buf, count);

	/* make sure that buf is null terminated */
	buf[sizeof(buf) - 1] = 0;

	/* drop the possible '\n' from the end */
	if (buf[count - 1] == '\n')
		buf[count - 1] = 0;

	mutex_lock(&mphy->dev->mutex);

	/* Ignore empty lines, 'echo' appends them sometimes at least. */
	if (buf[0] == 0) {
		ret = count;
		goto exit;
	}

	/* String starts with vdev name, ie 'wlan0'  Find the proper vif that
	 * matches the name.
	 */
	for (i = 0; i < ARRAY_SIZE(dev->mt76.wcid_mask); i++) {
		u32 mask = dev->mt76.wcid_mask[i];
		u32 phy_mask = dev->mt76.wcid_phy_mask[i];

		if (!mask)
			continue;

		for (j = i * 32; mask; j++, mask >>= 1, phy_mask >>= 1) {
			if (!(mask & 1))
				continue;

			wcid = rcu_dereference(dev->mt76.wcid[j]);
			if (!wcid)
				continue;

			msta = container_of(wcid, struct mt7915_sta, wcid);

			vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);

			wdev = ieee80211_vif_to_wdev(vif);

			if (!wdev)
				continue;

			snprintf(dev_name_match, sizeof(dev_name_match) - 1, "%s ",
				 wdev->netdev->name);

			if (strncmp(dev_name_match, buf, strlen(dev_name_match)) == 0) {
				vdev_id = j;
				td = &msta->test;
				bufptr = buf + strlen(dev_name_match) - 1;
				break;
			}
		}
	}

	if (vdev_id == 0xFFFF) {
		if (strstr(buf, "active=0")) {
			/* Ignore, we are disabling it anyway */
			ret = count;
			goto exit;
		} else {
			dev_info(dev->mt76.dev,
				 "mt7915: set-rate-override, unknown netdev name: %s\n", buf);
		}
		ret = -EINVAL;
		goto exit;
	}

#define MT7915_PARSE_LTOK(a, b)					\
	do {								\
		tok = strstr(bufptr, " " #a "=");			\
		if (tok) {						\
			char *tspace;					\
			tok += 1; /* move past initial space */		\
			strncpy(tmp, tok + strlen(#a "="), sizeof(tmp) - 1); \
			tmp[sizeof(tmp) - 1] = 0;			\
			tspace = strstr(tmp, " ");			\
			if (tspace)					\
				*tspace = 0;				\
			if (kstrtol(tmp, 0, &rc) != 0)			\
				dev_info(dev->mt76.dev,			\
					 "mt7915: set-rate-override: " #a \
					 "= could not be parsed, tmp: %s\n", \
					 tmp);				\
			else						\
				td->b = rc;				\
		}							\
	} while (0)

	/* TODO:  Allow configuring LTF? */
	td->tx_ltf = 1; /* 0: HTLTF 3.2us, 1: HELTF, 6.4us, 2 HELTF 12,8us */

	MT7915_PARSE_LTOK(tpc, tx_power[0]);
	MT7915_PARSE_LTOK(sgi, tx_rate_sgi);
	MT7915_PARSE_LTOK(mcs, tx_rate_idx);
	MT7915_PARSE_LTOK(nss, tx_rate_nss);
	MT7915_PARSE_LTOK(pream, tx_rate_mode);
	MT7915_PARSE_LTOK(retries, tx_xmit_count);
	MT7915_PARSE_LTOK(dynbw, tx_dynbw);
	MT7915_PARSE_LTOK(bw, txbw);
	MT7915_PARSE_LTOK(active, txo_active);

	dev_info(dev->mt76.dev,
		 "mt7915: set-rate-overrides, vdev %i(%s) active=%d tpc=%d sgi=%d mcs=%d"
		 " nss=%d pream=%d retries=%d dynbw=%d bw=%d\n",
		 vdev_id, dev_name_match,
		 td->txo_active, td->tx_power[0], td->tx_rate_sgi, td->tx_rate_idx,
		 td->tx_rate_nss, td->tx_rate_mode, td->tx_xmit_count, td->tx_dynbw,
		 td->txbw);

	ret = count;

exit:
	mutex_unlock(&mphy->dev->mutex);
	return ret;
}

static const struct file_operations fops_set_rate_override = {
	.read = mt7915_read_set_rate_override,
	.write = mt7915_write_set_rate_override,
	.open = simple_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static int
mt7915_rx_group_5_enable_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;

	mutex_lock(&dev->mt76.mutex);

	dev->rx_group_5_enable = !!val;

	/* Enabled if we requested enabled OR if monitor mode is enabled. */
	mt76_rmw_field(dev, MT_DMA_DCR0(0), MT_DMA_DCR0_RXD_G5_EN,
		       dev->rx_group_5_enable);
	mt76_testmode_reset(dev->phy.mt76, true);

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

static int
mt7915_rx_group_5_enable_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;

	*val = dev->rx_group_5_enable;

	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_rx_group_5_enable, mt7915_rx_group_5_enable_get,
			 mt7915_rx_group_5_enable_set, "%lld\n");

static void
mt7915_ampdu_stat_read_phy(struct mt7915_phy *phy,
			   struct seq_file *file)
{
	struct mt7915_dev *dev = phy->dev;
	bool ext_phy = phy != &dev->phy;
	int bound[15], range[4], i, n;

	/* Tx ampdu stat */
	for (i = 0; i < ARRAY_SIZE(range); i++)
		range[i] = mt76_rr(dev, MT_MIB_ARNG(phy->band_idx, i));

	for (i = 0; i < ARRAY_SIZE(bound); i++)
		bound[i] = MT_MIB_ARNCR_RANGE(range[i / 4], i % 4) + 1;

	seq_printf(file, "\nPhy %d, Phy band %d\n", ext_phy, phy->band_idx);

	seq_printf(file, "Length: %8d | ", bound[0]);
	for (i = 0; i < ARRAY_SIZE(bound) - 1; i++)
		seq_printf(file, "%3d -%3d | ",
			   bound[i] + 1, bound[i + 1]);

	seq_puts(file, "\nCount:  ");
	n = phy->band_idx ? ARRAY_SIZE(dev->mt76.aggr_stats) / 2 : 0;
	for (i = 0; i < ARRAY_SIZE(bound); i++)
		seq_printf(file, "%8d | ", dev->mt76.aggr_stats[i + n]);
	seq_puts(file, "\n");

	seq_printf(file, "BA miss count: %d\n", phy->mib.ba_miss_cnt);
}

static void
mt7915_txbf_stat_read_phy(struct mt7915_phy *phy, struct seq_file *s)
{
	static const char * const bw[] = {
		"BW20", "BW40", "BW80", "BW160"
	};
	struct mib_stats *mib = &phy->mib;

	/* Tx Beamformer monitor */
	seq_puts(s, "\nTx Beamformer applied PPDU counts: ");

	seq_printf(s, "iBF: %d, eBF: %d\n",
		   mib->tx_bf_ibf_ppdu_cnt,
		   mib->tx_bf_ebf_ppdu_cnt);

	/* Tx Beamformer Rx feedback monitor */
	seq_puts(s, "Tx Beamformer Rx feedback statistics: ");

	seq_printf(s, "All: %d, HE: %d, VHT: %d, HT: %d, ",
		   mib->tx_bf_rx_fb_all_cnt,
		   mib->tx_bf_rx_fb_he_cnt,
		   mib->tx_bf_rx_fb_vht_cnt,
		   mib->tx_bf_rx_fb_ht_cnt);

	seq_printf(s, "%s, NC: %d, NR: %d\n",
		   bw[mib->tx_bf_rx_fb_bw],
		   mib->tx_bf_rx_fb_nc_cnt,
		   mib->tx_bf_rx_fb_nr_cnt);

	/* Tx Beamformee Rx NDPA & Tx feedback report */
	seq_printf(s, "Tx Beamformee successful feedback frames: %d\n",
		   mib->tx_bf_fb_cpl_cnt);
	seq_printf(s, "Tx Beamformee feedback triggered counts: %d\n",
		   mib->tx_bf_fb_trig_cnt);

	/* Tx SU & MU counters */
	seq_printf(s, "Tx multi-user Beamforming counts: %d\n",
		   mib->tx_bf_cnt);
	seq_printf(s, "Tx multi-user MPDU counts: %d\n", mib->tx_mu_mpdu_cnt);
	seq_printf(s, "Tx multi-user successful MPDU counts: %d\n",
		   mib->tx_mu_acked_mpdu_cnt);
	seq_printf(s, "Tx single-user successful MPDU counts: %d\n",
		   mib->tx_su_acked_mpdu_cnt);

	seq_puts(s, "\n");
}

static int
mt7915_tx_stats_show(struct seq_file *file, void *data)
{
	struct mt7915_phy *phy = file->private;
	struct mt7915_dev *dev = phy->dev;
	struct mib_stats *mib = &phy->mib;
	int i;

	mutex_lock(&dev->mt76.mutex);

	mt7915_ampdu_stat_read_phy(phy, file);
	mt7915_mac_update_stats(phy);
	mt7915_txbf_stat_read_phy(phy, file);

	/* Tx amsdu info */
	seq_puts(file, "Tx MSDU statistics:\n");
	for (i = 0; i < ARRAY_SIZE(mib->tx_amsdu); i++) {
		seq_printf(file, "AMSDU pack count of %d MSDU in TXD: %8d ",
			   i + 1, mib->tx_amsdu[i]);
		if (mib->tx_amsdu_cnt)
			seq_printf(file, "(%3d%%)\n",
				   mib->tx_amsdu[i] * 100 / mib->tx_amsdu_cnt);
		else
			seq_puts(file, "\n");
	}

	mutex_unlock(&dev->mt76.mutex);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_tx_stats);

static void
mt7915_hw_queue_read(struct seq_file *s, u32 size,
		     const struct hw_queue_map *map)
{
	struct mt7915_phy *phy = s->private;
	struct mt7915_dev *dev = phy->dev;
	u32 i, val;

	val = mt76_rr(dev, MT_FL_Q_EMPTY);
	for (i = 0; i < size; i++) {
		u32 ctrl, head, tail, queued;

		if (val & BIT(map[i].index))
			continue;

		ctrl = BIT(31) | (map[i].pid << 10) | (map[i].qid << 24);
		mt76_wr(dev, MT_FL_Q0_CTRL, ctrl);

		head = mt76_get_field(dev, MT_FL_Q2_CTRL,
				      GENMASK(11, 0));
		tail = mt76_get_field(dev, MT_FL_Q2_CTRL,
				      GENMASK(27, 16));
		queued = mt76_get_field(dev, MT_FL_Q3_CTRL,
					GENMASK(11, 0));

		seq_printf(s, "\t%s: ", map[i].name);
		seq_printf(s, "queued:0x%03x head:0x%03x tail:0x%03x\n",
			   queued, head, tail);
	}
}

static void
mt7915_sta_hw_queue_read(void *data, struct ieee80211_sta *sta)
{
	struct mt7915_sta *msta = (struct mt7915_sta *)sta->drv_priv;
	struct mt7915_dev *dev = msta->vif->phy->dev;
	struct seq_file *s = data;
	u8 ac;

	for (ac = 0; ac < 4; ac++) {
		u32 qlen, ctrl, val;
		u32 idx = msta->wcid.idx >> 5;
		u8 offs = msta->wcid.idx & GENMASK(4, 0);

		ctrl = BIT(31) | BIT(11) | (ac << 24);
		val = mt76_rr(dev, MT_PLE_AC_QEMPTY(ac, idx));

		if (val & BIT(offs))
			continue;

		mt76_wr(dev, MT_FL_Q0_CTRL, ctrl | msta->wcid.idx);
		qlen = mt76_get_field(dev, MT_FL_Q3_CTRL,
				      GENMASK(11, 0));
		seq_printf(s, "\tSTA %pM wcid %d: AC%d%d queued:%d\n",
			   sta->addr, msta->wcid.idx,
			   msta->vif->mt76.wmm_idx, ac, qlen);
	}
}

static int
mt7915_hw_queues_show(struct seq_file *file, void *data)
{
	struct mt7915_phy *phy = file->private;
	struct mt7915_dev *dev = phy->dev;
	static const struct hw_queue_map ple_queue_map[] = {
		{ "CPU_Q0",  0,  1, MT_CTX0	      },
		{ "CPU_Q1",  1,  1, MT_CTX0 + 1	      },
		{ "CPU_Q2",  2,  1, MT_CTX0 + 2	      },
		{ "CPU_Q3",  3,  1, MT_CTX0 + 3	      },
		{ "ALTX_Q0", 8,  2, MT_LMAC_ALTX0     },
		{ "BMC_Q0",  9,  2, MT_LMAC_BMC0      },
		{ "BCN_Q0",  10, 2, MT_LMAC_BCN0      },
		{ "PSMP_Q0", 11, 2, MT_LMAC_PSMP0     },
		{ "ALTX_Q1", 12, 2, MT_LMAC_ALTX0 + 4 },
		{ "BMC_Q1",  13, 2, MT_LMAC_BMC0  + 4 },
		{ "BCN_Q1",  14, 2, MT_LMAC_BCN0  + 4 },
		{ "PSMP_Q1", 15, 2, MT_LMAC_PSMP0 + 4 },
	};
	static const struct hw_queue_map pse_queue_map[] = {
		{ "CPU Q0",  0,  1, MT_CTX0	      },
		{ "CPU Q1",  1,  1, MT_CTX0 + 1	      },
		{ "CPU Q2",  2,  1, MT_CTX0 + 2	      },
		{ "CPU Q3",  3,  1, MT_CTX0 + 3	      },
		{ "HIF_Q0",  8,  0, MT_HIF0	      },
		{ "HIF_Q1",  9,  0, MT_HIF0 + 1	      },
		{ "HIF_Q2",  10, 0, MT_HIF0 + 2	      },
		{ "HIF_Q3",  11, 0, MT_HIF0 + 3	      },
		{ "HIF_Q4",  12, 0, MT_HIF0 + 4	      },
		{ "HIF_Q5",  13, 0, MT_HIF0 + 5	      },
		{ "LMAC_Q",  16, 2, 0		      },
		{ "MDP_TXQ", 17, 2, 1		      },
		{ "MDP_RXQ", 18, 2, 2		      },
		{ "SEC_TXQ", 19, 2, 3		      },
		{ "SEC_RXQ", 20, 2, 4		      },
	};
	u32 val, head, tail;

	/* ple queue */
	val = mt76_rr(dev, MT_PLE_FREEPG_CNT);
	head = mt76_get_field(dev, MT_PLE_FREEPG_HEAD_TAIL, GENMASK(11, 0));
	tail = mt76_get_field(dev, MT_PLE_FREEPG_HEAD_TAIL, GENMASK(27, 16));
	seq_puts(file, "PLE page info:\n");
	seq_printf(file,
		   "\tTotal free page: 0x%08x head: 0x%03x tail: 0x%03x\n",
		   val, head, tail);

	val = mt76_rr(dev, MT_PLE_PG_HIF_GROUP);
	head = mt76_get_field(dev, MT_PLE_HIF_PG_INFO, GENMASK(11, 0));
	tail = mt76_get_field(dev, MT_PLE_HIF_PG_INFO, GENMASK(27, 16));
	seq_printf(file, "\tHIF free page: 0x%03x res: 0x%03x used: 0x%03x\n",
		   val, head, tail);

	seq_puts(file, "PLE non-empty queue info:\n");
	mt7915_hw_queue_read(file, ARRAY_SIZE(ple_queue_map),
			     &ple_queue_map[0]);

	/* iterate per-sta ple queue */
	ieee80211_iterate_stations_atomic(phy->mt76->hw,
					  mt7915_sta_hw_queue_read, file);
	/* pse queue */
	seq_puts(file, "PSE non-empty queue info:\n");
	mt7915_hw_queue_read(file, ARRAY_SIZE(pse_queue_map),
			     &pse_queue_map[0]);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_hw_queues);

static int
mt7915_xmit_queues_show(struct seq_file *file, void *data)
{
	struct mt7915_phy *phy = file->private;
	struct mt7915_dev *dev = phy->dev;
	struct {
		struct mt76_queue *q;
		char *queue;
	} queue_map[] = {
		{ phy->mt76->q_tx[MT_TXQ_BE],	 "   MAIN"  },
		{ dev->mt76.q_mcu[MT_MCUQ_WM],	 "  MCUWM"  },
		{ dev->mt76.q_mcu[MT_MCUQ_WA],	 "  MCUWA"  },
		{ dev->mt76.q_mcu[MT_MCUQ_FWDL], "MCUFWDL" },
	};
	int i;

	seq_puts(file, "     queue | hw-queued |      head |      tail |\n");
	for (i = 0; i < ARRAY_SIZE(queue_map); i++) {
		struct mt76_queue *q = queue_map[i].q;

		if (!q)
			continue;

		seq_printf(file, "   %s | %9d | %9d | %9d |\n",
			   queue_map[i].queue, q->queued, q->head,
			   q->tail);
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_xmit_queues);

static int
mt7915_rate_txpower_show(struct seq_file *file, void *data)
{
	static const char * const sku_group_name[] = {
		"CCK", "OFDM", "HT20", "HT40",
		"VHT20", "VHT40", "VHT80", "VHT160",
		"RU26", "RU52", "RU106", "RU242/SU20",
		"RU484/SU40", "RU996/SU80", "RU2x996/SU160"
	};
	struct mt7915_phy *phy = file->private;
	s8 txpower[MT7915_SKU_RATE_NUM], *buf;
	int i;

	seq_puts(file, "Per-chain txpower in 1/2 db units.\n");
	seq_printf(file, "\nPhy %d\n", phy != &phy->dev->phy);
	mt7915_mcu_get_txpower_sku(phy, txpower, sizeof(txpower));
	for (i = 0, buf = txpower; i < ARRAY_SIZE(mt7915_sku_group_len); i++) {
		u8 mcs_num = mt7915_sku_group_len[i];

		if (i >= SKU_VHT_BW20 && i <= SKU_VHT_BW160)
			mcs_num = 10;

		mt76_seq_puts_array(file, sku_group_name[i], buf, mcs_num);
		buf += mt7915_sku_group_len[i];
	}

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_rate_txpower);

static int
mt7915_twt_stats(struct seq_file *s, void *data)
{
	struct mt7915_dev *dev = dev_get_drvdata(s->private);
	struct mt7915_twt_flow *iter;

	rcu_read_lock();

	seq_puts(s, "     wcid |       id |    flags |      exp | mantissa");
	seq_puts(s, " | duration |            tsf |\n");
	list_for_each_entry_rcu(iter, &dev->twt_list, list)
		seq_printf(s,
			"%9d | %8d | %5c%c%c%c | %8d | %8d | %8d | %14lld |\n",
			iter->wcid, iter->id,
			iter->sched ? 's' : 'u',
			iter->protection ? 'p' : '-',
			iter->trigger ? 't' : '-',
			iter->flowtype ? '-' : 'a',
			iter->exp, iter->mantissa,
			iter->duration, iter->tsf);

	rcu_read_unlock();

	return 0;
}

/* The index of RF registers use the generic regidx, combined with two parts:
 * WF selection [31:28] and offset [27:0].
 */
static int
mt7915_rf_regval_get(void *data, u64 *val)
{
	struct mt7915_dev *dev = data;
	u32 regval;
	int ret;

	ret = mt7915_mcu_rf_regval(dev, dev->mt76.debugfs_reg, &regval, false);
	if (ret)
		return ret;

	*val = regval;

	return 0;
}

static int
mt7915_rf_regval_set(void *data, u64 val)
{
	struct mt7915_dev *dev = data;
	u32 val32 = val;

	return mt7915_mcu_rf_regval(dev, dev->mt76.debugfs_reg, &val32, true);
}

DEFINE_DEBUGFS_ATTRIBUTE(fops_rf_regval, mt7915_rf_regval_get,
			 mt7915_rf_regval_set, "0x%08llx\n");

int mt7915_init_debugfs(struct mt7915_phy *phy)
{
	struct mt7915_dev *dev = phy->dev;
	bool ext_phy = phy != &dev->phy;
	struct dentry *dir;

	dir = mt76_register_debugfs_fops(phy->mt76, NULL);
	if (!dir)
		return -ENOMEM;
	debugfs_create_file("muru_debug", 0600, dir, dev, &fops_muru_debug);
	debugfs_create_file("muru_stats", 0400, dir, phy,
			    &mt7915_muru_stats_fops);
	debugfs_create_file("hw-queues", 0400, dir, phy,
			    &mt7915_hw_queues_fops);
	debugfs_create_file("xmit-queues", 0400, dir, phy,
			    &mt7915_xmit_queues_fops);
	debugfs_create_file("tx_stats", 0400, dir, phy, &mt7915_tx_stats_fops);
	debugfs_create_file("fw_ser", 0600, dir, phy, &mt7915_fw_ser_ops);
	debugfs_create_file("fw_debug_wm", 0600, dir, dev, &fops_fw_debug_wm);
	debugfs_create_file("fw_debug_wa", 0600, dir, dev, &fops_fw_debug_wa);
	debugfs_create_file("fw_debug_bin", 0600, dir, dev, &fops_fw_debug_bin);
	debugfs_create_file("fw_util_wm", 0400, dir, dev,
			    &mt7915_fw_util_wm_fops);
	debugfs_create_file("fw_util_wa", 0400, dir, dev,
			    &mt7915_fw_util_wa_fops);
	debugfs_create_file("rx_group_5_enable", 0600, dir, dev, &fops_rx_group_5_enable);
	debugfs_create_file("implicit_txbf", 0600, dir, dev,
			    &fops_implicit_txbf);
	debugfs_create_file("txpower_sku", 0400, dir, phy,
			    &mt7915_rate_txpower_fops);
	debugfs_create_devm_seqfile(dev->mt76.dev, "twt_stats", dir,
				    mt7915_twt_stats);
	debugfs_create_file("rf_regval", 0600, dir, dev, &fops_rf_regval);

	if (!dev->dbdc_support || phy->band_idx) {
		debugfs_create_u32("dfs_hw_pattern", 0400, dir,
				   &dev->hw_pattern);
		debugfs_create_file("radar_trigger", 0200, dir, dev,
				    &fops_radar_trigger);
		debugfs_create_devm_seqfile(dev->mt76.dev, "rdd_monitor", dir,
					    mt7915_rdd_monitor);
	}
	debugfs_create_file("set_rate_override", 0600, dir,
			    dev, &fops_set_rate_override);

	if (!ext_phy)
		dev->debugfs_dir = dir;

#ifdef MTK_DEBUG
	debugfs_create_u16("wlan_idx", 0600, dir, &dev->wlan_idx);
	mt7915_mtk_init_debugfs(phy, dir);
#endif

	return 0;
}

static void
mt7915_debugfs_write_fwlog(struct mt7915_dev *dev, const void *hdr, int hdrlen,
			 const void *data, int len)
{
	static DEFINE_SPINLOCK(lock);
	unsigned long flags;
	void *dest;

	spin_lock_irqsave(&lock, flags);
	dest = relay_reserve(dev->relay_fwlog, hdrlen + len + 4);
	if (dest) {
		*(u32 *)dest = hdrlen + len;
		dest += 4;

		if (hdrlen) {
			memcpy(dest, hdr, hdrlen);
			dest += hdrlen;
		}

		memcpy(dest, data, len);
		relay_flush(dev->relay_fwlog);
	}
	spin_unlock_irqrestore(&lock, flags);
}

void mt7915_debugfs_rx_fw_monitor(struct mt7915_dev *dev, const void *data, int len)
{
	struct {
		__le32 magic;
		__le32 timestamp;
		__le16 msg_type;
		__le16 len;
	} hdr = {
		.magic = cpu_to_le32(FW_BIN_LOG_MAGIC),
		.msg_type = cpu_to_le16(PKT_TYPE_RX_FW_MONITOR),
	};

#ifdef MTK_DEBUG
	struct {
		__le32 magic;
		u8 version;
		u8 _rsv;
		__le16 serial_id;
		__le32 timestamp;
		__le16 msg_type;
		__le16 len;
	} hdr2 = {
		.version = 0x1,
		.magic = cpu_to_le32(FW_BIN_LOG_MAGIC_V2),
		.msg_type = PKT_TYPE_RX_FW_MONITOR,
	};
#endif

	if (!dev->relay_fwlog)
		return;

#ifdef MTK_DEBUG
	/* old magic num */
	if (!(dev->fw.debug_wm & BIT(7))) {
		hdr.timestamp = mt76_rr(dev, MT_LPON_FRCR(0));
		hdr.len = *(__le16 *)data;
		mt7915_debugfs_write_fwlog(dev, &hdr, sizeof(hdr), data, len);
	} else {
		hdr2.serial_id = dev->dbg.fwlog_seq++;
		hdr2.timestamp = mt76_rr(dev, MT_LPON_FRCR(0));
		hdr2.len = *(__le16 *)data;
		mt7915_debugfs_write_fwlog(dev, &hdr2, sizeof(hdr2), data, len);
	}
#else
	hdr.timestamp = cpu_to_le32(mt76_rr(dev, MT_LPON_FRCR(0)));
	hdr.len = *(__le16 *)data;
	mt7915_debugfs_write_fwlog(dev, &hdr, sizeof(hdr), data, len);
#endif
}

bool mt7915_debugfs_rx_log(struct mt7915_dev *dev, const void *data, int len)
{
#ifdef MTK_DEBUG
	if (get_unaligned_le32(data) != FW_BIN_LOG_MAGIC &&
	    get_unaligned_le32(data) != FW_BIN_LOG_MAGIC_V2 &&
	    get_unaligned_le32(data) != PKT_BIN_DEBUG_MAGIC)
#else
	if (get_unaligned_le32(data) != FW_BIN_LOG_MAGIC)
#endif
		return false;

	if (dev->relay_fwlog)
		mt7915_debugfs_write_fwlog(dev, NULL, 0, data, len);
	else
		return false;

	return true;
}

#ifdef CONFIG_MAC80211_DEBUGFS
/** per-station debugfs **/

static ssize_t mt7915_sta_fixed_rate_set(struct file *file,
					 const char __user *user_buf,
					 size_t count, loff_t *ppos)
{
	struct ieee80211_sta *sta = file->private_data;
	struct mt7915_sta *msta = (struct mt7915_sta *)sta->drv_priv;
	struct mt7915_dev *dev = msta->vif->phy->dev;
	struct ieee80211_vif *vif;
	struct sta_phy phy = {};
	char buf[100];
	int ret;
	u32 field;
	u8 i, gi, he_ltf;

	if (count >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(buf, user_buf, count))
		return -EFAULT;

	if (count && buf[count - 1] == '\n')
		buf[count - 1] = '\0';
	else
		buf[count] = '\0';

	/* mode - cck: 0, ofdm: 1, ht: 2, gf: 3, vht: 4, he_su: 8, he_er: 9
	 * bw - bw20: 0, bw40: 1, bw80: 2, bw160: 3
	 * nss - vht: 1~4, he: 1~4, others: ignore
	 * mcs - cck: 0~4, ofdm: 0~7, ht: 0~32, vht: 0~9, he_su: 0~11, he_er: 0~2
	 * gi - (ht/vht) lgi: 0, sgi: 1; (he) 0.8us: 0, 1.6us: 1, 3.2us: 2
	 * ldpc - off: 0, on: 1
	 * stbc - off: 0, on: 1
	 * he_ltf - 1xltf: 0, 2xltf: 1, 4xltf: 2
	 */
	if (sscanf(buf, "%hhu %hhu %hhu %hhu %hhu %hhu %hhu %hhu",
		   &phy.type, &phy.bw, &phy.nss, &phy.mcs, &gi,
		   &phy.ldpc, &phy.stbc, &he_ltf) != 8) {
		dev_warn(dev->mt76.dev,
			 "format: Mode BW NSS MCS (HE)GI LDPC STBC HE_LTF\n");
		field = RATE_PARAM_AUTO;
		goto out;
	}

	phy.ldpc = (phy.bw || phy.ldpc) * GENMASK(2, 0);
	for (i = 0; i <= phy.bw; i++) {
		phy.sgi |= gi << (i << sta->deflink.he_cap.has_he);
		phy.he_ltf |= he_ltf << (i << sta->deflink.he_cap.has_he);
	}
	field = RATE_PARAM_FIXED;

out:
	vif = container_of((void *)msta->vif, struct ieee80211_vif, drv_priv);
	ret = mt7915_mcu_set_fixed_rate_ctrl(dev, vif, sta, &phy, field);
	if (ret)
		return -EFAULT;

	return count;
}

static const struct file_operations fops_fixed_rate = {
	.write = mt7915_sta_fixed_rate_set,
	.open = simple_open,
	.owner = THIS_MODULE,
	.llseek = default_llseek,
};

static int
mt7915_queues_show(struct seq_file *s, void *data)
{
	struct ieee80211_sta *sta = s->private;

	mt7915_sta_hw_queue_read(s, sta);

	return 0;
}

DEFINE_SHOW_ATTRIBUTE(mt7915_queues);

void mt7915_sta_add_debugfs(struct ieee80211_hw *hw, struct ieee80211_vif *vif,
			    struct ieee80211_sta *sta, struct dentry *dir)
{
	debugfs_create_file("fixed_rate", 0600, dir, sta, &fops_fixed_rate);
	debugfs_create_file("hw-queues", 0400, dir, sta, &mt7915_queues_fops);
}

#endif
