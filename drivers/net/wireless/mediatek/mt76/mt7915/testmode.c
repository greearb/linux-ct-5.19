// SPDX-License-Identifier: ISC
/* Copyright (C) 2020 MediaTek Inc. */

#include "mt7915.h"
#include "mac.h"
#include "mcu.h"
#include "testmode.h"
#include "eeprom.h"

enum {
	TM_CHANGED_TXPOWER,
	TM_CHANGED_FREQ_OFFSET,
	TM_CHANGED_AID,
	TM_CHANGED_CFG,
	TM_CHANGED_TXBF_ACT,

	/* must be last */
	NUM_TM_CHANGED
};

static const u8 tm_change_map[] = {
	[TM_CHANGED_TXPOWER] = MT76_TM_ATTR_TX_POWER,
	[TM_CHANGED_FREQ_OFFSET] = MT76_TM_ATTR_FREQ_OFFSET,
	[TM_CHANGED_AID] = MT76_TM_ATTR_AID,
	[TM_CHANGED_CFG] = MT76_TM_ATTR_CFG,
	[TM_CHANGED_TXBF_ACT] = MT76_TM_ATTR_TXBF_ACT,
};

struct reg_band {
	u32 band[2];
};

#define REG_BAND(_list, _reg) \
		{ _list.band[0] = MT_##_reg(0);	\
		  _list.band[1] = MT_##_reg(1); }
#define REG_BAND_IDX(_list, _reg, _idx) \
		{ _list.band[0] = MT_##_reg(0, _idx);	\
		  _list.band[1] = MT_##_reg(1, _idx); }

#define TM_REG_MAX_ID	20
static struct reg_band reg_backup_list[TM_REG_MAX_ID];

static void mt7915_tm_update_entry(struct mt7915_phy *phy);

static u8 mt7915_tm_chan_bw(enum nl80211_chan_width width)
{
	static const u8 width_to_bw[] = {
		[NL80211_CHAN_WIDTH_40] = TM_CBW_40MHZ,
		[NL80211_CHAN_WIDTH_80] = TM_CBW_80MHZ,
		[NL80211_CHAN_WIDTH_80P80] = TM_CBW_8080MHZ,
		[NL80211_CHAN_WIDTH_160] = TM_CBW_160MHZ,
		[NL80211_CHAN_WIDTH_5] = TM_CBW_5MHZ,
		[NL80211_CHAN_WIDTH_10] = TM_CBW_10MHZ,
		[NL80211_CHAN_WIDTH_20] = TM_CBW_20MHZ,
		[NL80211_CHAN_WIDTH_20_NOHT] = TM_CBW_20MHZ,
	};

	if (width >= ARRAY_SIZE(width_to_bw))
		return 0;

	return width_to_bw[width];
}

static void
mt7915_tm_update_channel(struct mt7915_phy *phy)
{
	mutex_unlock(&phy->dev->mt76.mutex);
	mt7915_set_channel(phy);
	mutex_lock(&phy->dev->mt76.mutex);

	mt7915_mcu_set_chan_info(phy, MCU_EXT_CMD(SET_RX_PATH));

	mt7915_tm_update_entry(phy);
}

static int
mt7915_tm_set_tx_power(struct mt7915_phy *phy)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt76_phy *mphy = phy->mt76;
	struct cfg80211_chan_def *chandef = &mphy->chandef;
	int freq = chandef->center_freq1;
	int ret;
	struct {
		u8 format_id;
		u8 dbdc_idx;
		s8 tx_power;
		u8 ant_idx;	/* Only 0 is valid */
		u8 center_chan;
		u8 rsv[3];
	} __packed req = {
		.format_id = 0xf,
		.dbdc_idx = phy != &dev->phy,
		.center_chan = ieee80211_frequency_to_channel(freq),
	};
	u8 *tx_power = NULL;

	if (phy->mt76->test.state != MT76_TM_STATE_OFF)
		tx_power = phy->mt76->test.tx_power;

	/* Tx power of the other antennas are the same as antenna 0 */
	if (tx_power && tx_power[0])
		req.tx_power = tx_power[0];

	ret = mt76_mcu_send_msg(&dev->mt76,
				MCU_EXT_CMD(TX_POWER_FEATURE_CTRL),
				&req, sizeof(req), false);

	return ret;
}

static int
mt7915_tm_set_freq_offset(struct mt7915_phy *phy, bool en, u32 val)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = en,
		.param_idx = MCU_ATE_SET_FREQ_OFFSET,
		.param.freq.band = phy != &dev->phy,
		.param.freq.freq_offset = cpu_to_le32(val),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_mode_ctrl(struct mt7915_dev *dev, bool enable)
{
	struct {
		u8 format_id;
		bool enable;
		u8 rsv[2];
	} __packed req = {
		.format_id = 0x6,
		.enable = enable,
	};

	return mt76_mcu_send_msg(&dev->mt76,
				 MCU_EXT_CMD(TX_POWER_FEATURE_CTRL),
				 &req, sizeof(req), false);
}

static int
mt7915_tm_set_trx(struct mt7915_phy *phy, int type, bool en)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = 1,
		.param_idx = MCU_ATE_SET_TRX,
		.param.trx.type = type,
		.param.trx.enable = en,
		.param.trx.band = phy != &dev->phy,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_clean_hwq(struct mt7915_phy *phy)
{
	struct mt76_testmode_entry_data *ed;
	struct mt76_wcid *wcid;
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = 1,
		.param_idx = MCU_ATE_CLEAN_TXQUEUE,
		.param.clean.band = phy != &dev->phy,
	};

	mt76_tm_for_each_entry(phy->mt76, wcid, ed) {
		int ret;

		req.param.clean.wcid = wcid->idx;
		ret = mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL),
					&req, sizeof(req), false);
		if (ret)
			return ret;
	}

	return 0;
}

static int
mt7915_tm_set_phy_count(struct mt7915_phy *phy, u8 control)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = 1,
		.param_idx = MCU_ATE_SET_PHY_COUNT,
		.param.cfg.enable = control,
		.param.cfg.band = phy != &dev->phy,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_set_slot_time(struct mt7915_phy *phy, u8 slot_time, u8 sifs)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = !(phy->mt76->test.state == MT76_TM_STATE_OFF),
		.param_idx = MCU_ATE_SET_SLOT_TIME,
		.param.slot.slot_time = slot_time,
		.param.slot.sifs = sifs,
		.param.slot.rifs = 2,
		.param.slot.eifs = cpu_to_le16(60),
		.param.slot.band = phy != &dev->phy,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_set_tam_arb(struct mt7915_phy *phy, bool enable, bool mu)
{
	struct mt7915_dev *dev = phy->dev;
	u32 op_mode;

	if (!enable)
		op_mode = TAM_ARB_OP_MODE_NORMAL;
	else if (mu)
		op_mode = TAM_ARB_OP_MODE_TEST;
	else
		op_mode = TAM_ARB_OP_MODE_FORCE_SU;

	return mt7915_mcu_set_muru_ctrl(dev, MURU_SET_ARB_OP_MODE, op_mode);
}

static int
mt7915_tm_set_cfg(struct mt7915_phy *phy)
{
	static const u8 cfg_cmd[] = {
		[MT76_TM_CFG_TSSI] = MCU_ATE_SET_TSSI,
		[MT76_TM_CFG_DPD] = MCU_ATE_SET_DPD,
		[MT76_TM_CFG_RATE_POWER_OFFSET] = MCU_ATE_SET_RATE_POWER_OFFSET,
		[MT76_TM_CFG_THERMAL_COMP] = MCU_ATE_SET_THERMAL_COMP,
	};
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = !(phy->mt76->test.state == MT76_TM_STATE_OFF),
		.param_idx = cfg_cmd[td->cfg.type],
		.param.cfg.enable = td->cfg.enable,
		.param.cfg.band = phy->band_idx,
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_add_txbf(struct mt7915_phy *phy, struct ieee80211_vif *vif,
		   struct ieee80211_sta *sta, u8 pfmu_idx, u8 nr,
		   u8 nc, bool ebf)
{
	struct mt7915_vif *mvif = (struct mt7915_vif *)vif->drv_priv;
	struct mt7915_sta *msta = (struct mt7915_sta *)sta->drv_priv;
	struct mt7915_dev *dev = phy->dev;
	struct sk_buff *skb;
	struct sta_rec_bf *bf;
	struct tlv *tlv;
	u8 ndp_rate;

	if (nr == 1)
		ndp_rate = 8;
	else if (nr == 2)
		ndp_rate = 16;
	else
		ndp_rate = 24;

	skb = mt76_connac_mcu_alloc_sta_req(&dev->mt76, &mvif->mt76,
					    &msta->wcid);
	if (IS_ERR(skb))
		return PTR_ERR(skb);

	tlv = mt76_connac_mcu_add_tlv(skb, STA_REC_BF, sizeof(*bf));
	bf = (struct sta_rec_bf *)tlv;

	bf->pfmu = cpu_to_le16(pfmu_idx);
	bf->sounding_phy = 1;
	bf->bf_cap = ebf;
	bf->ncol = nc;
	bf->nrow = nr;
	bf->ndp_rate = ndp_rate;
	bf->ibf_timeout = 0xff;
	bf->tx_mode = MT_PHY_TYPE_HT;

	if (ebf) {
		bf->mem[0].row = 0;
		bf->mem[1].row = 1;
		bf->mem[2].row = 2;
		bf->mem[3].row = 3;
	} else {
		bf->mem[0].row = 4;
		bf->mem[1].row = 5;
		bf->mem[2].row = 6;
		bf->mem[3].row = 7;
	}

	return mt76_mcu_skb_send_msg(&dev->mt76, skb,
				     MCU_EXT_CMD(STA_REC_UPDATE), true);
}

static int
mt7915_tm_entry_add(struct mt7915_phy *phy, u8 aid)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_testmode_entry_data *ed;
	struct ieee80211_sband_iftype_data *sdata;
	struct ieee80211_supported_band *sband;
	struct ieee80211_sta *sta;
	struct mt7915_sta *msta;
	int tid, ret;

	if (td->entry_num >= MT76_TM_MAX_ENTRY_NUM)
		return -EINVAL;

	sta = kzalloc(sizeof(*sta) + phy->mt76->hw->sta_data_size +
		      sizeof(*ed), GFP_KERNEL);
	if (!sta)
		return -ENOMEM;

	msta = (struct mt7915_sta *)sta->drv_priv;
	ed = mt76_testmode_entry_data(phy->mt76, &msta->wcid);
	memcpy(ed, &td->ed, sizeof(*ed));

	if (phy->mt76->chandef.chan->band == NL80211_BAND_5GHZ) {
		sband = &phy->mt76->sband_5g.sband;
		sdata = phy->iftype[NL80211_BAND_5GHZ];
	} else if (phy->mt76->chandef.chan->band == NL80211_BAND_6GHZ) {
		sband = &phy->mt76->sband_6g.sband;
		sdata = phy->iftype[NL80211_BAND_6GHZ];
	} else {
		sband = &phy->mt76->sband_2g.sband;
		sdata = phy->iftype[NL80211_BAND_2GHZ];
	}

	memcpy(sta->addr, ed->addr[0], ETH_ALEN);
	if (phy->test.bf_en) {
		u8 addr[ETH_ALEN] = {0x00, 0x11, 0x11, 0x11, 0x11, 0x11};

		memcpy(sta->addr, addr, ETH_ALEN);
	}

	if (td->tx_rate_mode >= MT76_TM_TX_MODE_HT)
		memcpy(&sta->ht_cap, &sband->ht_cap, sizeof(sta->ht_cap));
	if (td->tx_rate_mode >= MT76_TM_TX_MODE_VHT)
		memcpy(&sta->vht_cap, &sband->vht_cap, sizeof(sta->vht_cap));
	if (td->tx_rate_mode >= MT76_TM_TX_MODE_HE_SU)
		memcpy(&sta->he_cap, &sdata[NL80211_IFTYPE_STATION].he_cap,
		       sizeof(sta->he_cap));
	sta->aid = aid;
	sta->wme = 1;

	ret = mt7915_mac_sta_add(&phy->dev->mt76, phy->monitor_vif, sta);
	if (ret) {
		kfree(sta);
		return ret;
	}

	/* prevent from starting tx ba session */
	for (tid = 0; tid < 8; tid++)
		set_bit(tid, &msta->ampdu_state);

	list_add_tail(&msta->wcid.list, &td->tm_entry_list);
	td->entry_num++;

	return 0;
}

static void
mt7915_tm_entry_remove(struct mt7915_phy *phy, u8 aid)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_wcid *wcid, *tmp;

	if (list_empty(&td->tm_entry_list))
		return;

	list_for_each_entry_safe(wcid, tmp, &td->tm_entry_list, list) {
		struct mt76_testmode_entry_data *ed;
		struct mt7915_dev *dev = phy->dev;
		struct ieee80211_sta *sta;

		ed = mt76_testmode_entry_data(phy->mt76, wcid);
		if (aid && ed->aid != aid)
			continue;

		sta = wcid_to_sta(wcid);
		mt7915_mac_sta_remove(&dev->mt76, phy->monitor_vif, sta);
		mt76_wcid_mask_clear(dev->mt76.wcid_mask, wcid->idx);

		list_del_init(&wcid->list);
		kfree(sta);
		phy->mt76->test.entry_num--;
	}
}

static int
mt7915_tm_set_entry(struct mt7915_phy *phy)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_testmode_entry_data *ed;
	struct mt76_wcid *wcid;

	if (!td->aid) {
		if (td->state > MT76_TM_STATE_IDLE)
			mt76_testmode_set_state(phy->mt76, MT76_TM_STATE_IDLE);
		mt7915_tm_entry_remove(phy, td->aid);
		return 0;
	}

	mt76_tm_for_each_entry(phy->mt76, wcid, ed) {
		if (ed->aid == td->aid) {
			struct sk_buff *skb;

			local_bh_disable();
			skb = ed->tx_skb;
			memcpy(ed, &td->ed, sizeof(*ed));
			ed->tx_skb = skb;
			local_bh_enable();

			return 0;
		}
	}

	return mt7915_tm_entry_add(phy, td->aid);
}

static void
mt7915_tm_update_entry(struct mt7915_phy *phy)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_testmode_entry_data *ed, tmp;
	struct mt76_wcid *wcid, *last;

	if (!td->aid || phy->test.bf_en)
		return;

	memcpy(&tmp, &td->ed, sizeof(tmp));
	last = list_last_entry(&td->tm_entry_list,
			       struct mt76_wcid, list);

	mt76_tm_for_each_entry(phy->mt76, wcid, ed) {
		memcpy(&td->ed, ed, sizeof(td->ed));
		mt7915_tm_entry_remove(phy, td->aid);
		mt7915_tm_entry_add(phy, td->aid);
		if (wcid == last)
			break;
	}

	memcpy(&td->ed, &tmp, sizeof(td->ed));
}

static int
mt7915_tm_txbf_init(struct mt7915_phy *phy, u16 *val)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt7915_dev *dev = phy->dev;
	bool enable = val[0];
	void *phase_cal, *pfmu_data, *pfmu_tag;
	u8 addr[ETH_ALEN] = {0x00, 0x22, 0x22, 0x22, 0x22, 0x22};

	if (!enable) {
		phy->test.bf_en = 0;
		return 0;
	}

	if (!dev->test.txbf_phase_cal) {
		phase_cal = devm_kzalloc(dev->mt76.dev,
					 sizeof(struct mt7915_tm_txbf_phase) *
					 MAX_PHASE_GROUP_NUM,
					 GFP_KERNEL);
		if (!phase_cal)
			return -ENOMEM;

		dev->test.txbf_phase_cal = phase_cal;
	}

	if (!dev->test.txbf_pfmu_data) {
		pfmu_data = devm_kzalloc(dev->mt76.dev, 512, GFP_KERNEL);
		if (!pfmu_data)
			return -ENOMEM;

		dev->test.txbf_pfmu_data = pfmu_data;
	}

	if (!dev->test.txbf_pfmu_tag) {
		pfmu_tag = devm_kzalloc(dev->mt76.dev,
					sizeof(struct mt7915_tm_pfmu_tag), GFP_KERNEL);
		if (!pfmu_tag)
			return -ENOMEM;

		dev->test.txbf_pfmu_tag = pfmu_tag;
	}

	memcpy(phy->monitor_vif->addr, addr, ETH_ALEN);
	mt7915_mcu_add_dev_info(phy, phy->monitor_vif, true);

	td->tx_rate_mode = MT76_TM_TX_MODE_HT;
	td->tx_mpdu_len = 1024;
	td->tx_rate_sgi = 0;
	td->tx_ipg = 100;
	phy->test.bf_en = 1;

	return mt7915_tm_set_trx(phy, TM_MAC_TX, true);
}

static int
mt7915_tm_txbf_phase_comp(struct mt7915_phy *phy, u16 *val)
{
	struct mt7915_dev *dev = phy->dev;
	struct {
		u8 category;
		u8 wlan_idx_lo;
		u8 bw;
		u8 jp_band;
		u8 dbdc_idx;
		bool read_from_e2p;
		bool disable;
		u8 wlan_idx_hi;
		u8 buf[40];
	} __packed req = {
		.category = MT_BF_IBF_PHASE_COMP,
		.bw = val[0],
		.jp_band = (val[2] == 1) ? 1 : 0,
		.dbdc_idx = phy->band_idx,
		.read_from_e2p = val[3],
		.disable = val[4],
	};
	struct mt7915_tm_txbf_phase *phase =
		(struct mt7915_tm_txbf_phase *)dev->test.txbf_phase_cal;

	wait_event_timeout(dev->mt76.tx_wait, phase[val[2]].status != 0, HZ);
	memcpy(req.buf, &phase[val[2]].phase, sizeof(req.buf));

	pr_info("ibf cal process: phase comp info\n");
	print_hex_dump(KERN_INFO, "", DUMP_PREFIX_NONE, 16, 1,
		       &req, sizeof(req), 0);

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(TXBF_ACTION), &req,
				 sizeof(req), true);
}

static int
mt7915_tm_txbf_profile_tag_read(struct mt7915_phy *phy, u8 pfmu_idx)
{
	struct mt7915_dev *dev = phy->dev;
	struct {
		u8 format_id;
		u8 pfmu_idx;
		bool bfer;
		u8 dbdc_idx;
	} __packed req = {
		.format_id = MT_BF_PFMU_TAG_READ,
		.pfmu_idx = pfmu_idx,
		.bfer = 1,
		.dbdc_idx = phy != &dev->phy,
	};
	struct mt7915_tm_pfmu_tag *tag = phy->dev->test.txbf_pfmu_tag;

	tag->t1.pfmu_idx = 0;

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(TXBF_ACTION), &req,
				 sizeof(req), true);
}

static int
mt7915_tm_txbf_profile_tag_write(struct mt7915_phy *phy, u8 pfmu_idx,
				 struct mt7915_tm_pfmu_tag *tag)
{
	struct mt7915_dev *dev = phy->dev;
	struct {
		u8 format_id;
		u8 pfmu_idx;
		bool bfer;
		u8 dbdc_idx;
		u8 buf[64];
	} __packed req = {
		.format_id = MT_BF_PFMU_TAG_WRITE,
		.pfmu_idx = pfmu_idx,
		.bfer = 1,
		.dbdc_idx = phy != &dev->phy,
	};

	memcpy(req.buf, tag, sizeof(*tag));
	wait_event_timeout(dev->mt76.tx_wait, tag->t1.pfmu_idx != 0, HZ);

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(TXBF_ACTION), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_txbf_apply_tx(struct mt7915_phy *phy, u16 wlan_idx, bool ebf,
			bool ibf, bool phase_cal)
{
#define to_wcid_lo(id)			FIELD_GET(GENMASK(7, 0), (u16)id)
#define to_wcid_hi(id)			FIELD_GET(GENMASK(9, 8), (u16)id)
	struct mt7915_dev *dev = phy->dev;
	struct {
		u8 category;
		u8 wlan_idx_lo;
		bool ebf;
		bool ibf;
		bool mu_txbf;
		bool phase_cal;
		u8 wlan_idx_hi;
		u8 _rsv;
	} __packed req = {
		.category = MT_BF_DATA_PACKET_APPLY,
		.wlan_idx_lo = to_wcid_lo(wlan_idx),
		.ebf = ebf,
		.ibf = ibf,
		.phase_cal = phase_cal,
		.wlan_idx_hi = to_wcid_hi(wlan_idx),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(TXBF_ACTION), &req,
				 sizeof(req), false);
}

static int mt7915_tm_txbf_set_rate(struct mt7915_phy *phy,
				   struct mt76_wcid *wcid)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt76_testmode_entry_data *ed = mt76_testmode_entry_data(phy->mt76, wcid);
	struct ieee80211_sta *sta = wcid_to_sta(wcid);
	struct sta_phy rate = {};

	if (!sta)
		return 0;

	rate.type = MT_PHY_TYPE_HT;
	rate.bw = mt7915_tm_chan_bw(phy->mt76->chandef.width);
	rate.nss = ed->tx_rate_nss;
	rate.mcs = ed->tx_rate_idx;
	rate.ldpc = (rate.bw || ed->tx_rate_ldpc) * GENMASK(2, 0);

	return mt7915_mcu_set_fixed_rate_ctrl(dev, phy->monitor_vif, sta,
					      &rate, RATE_PARAM_FIXED);
}

static int
mt7915_tm_txbf_set_tx(struct mt7915_phy *phy, u16 *val)
{
	bool bf_on = val[0], update = val[3];
	/* u16 wlan_idx = val[2]; */
	struct mt7915_tm_pfmu_tag *tag = phy->dev->test.txbf_pfmu_tag;
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_wcid *wcid;

	if (bf_on) {
		mt7915_tm_set_trx(phy, TM_MAC_RX_RXV, false);
		mt7915_tm_txbf_profile_tag_read(phy, 2);
		tag->t1.invalid_prof = false;
		mt7915_tm_txbf_profile_tag_write(phy, 2, tag);

		phy->test.bf_ever_en = true;

		if (update)
			mt7915_tm_txbf_apply_tx(phy, 1, 0, 1, 1);
	} else {
		if (!phy->test.bf_ever_en) {
			if (update)
				mt7915_tm_txbf_apply_tx(phy, 1, 0, 0, 0);
		} else {
			phy->test.bf_ever_en = false;

			mt7915_tm_txbf_profile_tag_read(phy, 2);
			tag->t1.invalid_prof = true;
			mt7915_tm_txbf_profile_tag_write(phy, 2, tag);
		}
	}

	wcid = list_first_entry(&td->tm_entry_list, struct mt76_wcid, list);
	mt7915_tm_txbf_set_rate(phy, wcid);

	return 0;
}

static int
mt7915_tm_txbf_profile_update(struct mt7915_phy *phy, u16 *val, bool ebf)
{
	static const u8 mode_to_lm[] = {
		[MT76_TM_TX_MODE_CCK] = 0,
		[MT76_TM_TX_MODE_OFDM] = 0,
		[MT76_TM_TX_MODE_HT] = 1,
		[MT76_TM_TX_MODE_VHT] = 2,
		[MT76_TM_TX_MODE_HE_SU] = 3,
		[MT76_TM_TX_MODE_HE_EXT_SU] = 3,
		[MT76_TM_TX_MODE_HE_TB] = 3,
		[MT76_TM_TX_MODE_HE_MU] = 3,
	};
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_wcid *wcid;
	struct ieee80211_vif *vif = phy->monitor_vif;
	struct mt7915_tm_pfmu_tag *tag = phy->dev->test.txbf_pfmu_tag;
	u8 pfmu_idx = val[0], nc = val[2], nr;
	bool is_atenl = val[6];
	int ret;

	if (td->tx_antenna_mask == 3)
		nr = 1;
	else if (td->tx_antenna_mask == 7)
		nr = 2;
	else
		nr = 3;

	memset(tag, 0, sizeof(*tag));
	tag->t1.pfmu_idx = pfmu_idx;
	tag->t1.ebf = ebf;
	tag->t1.nr = nr;
	tag->t1.nc = nc;
	tag->t1.invalid_prof = true;

	tag->t1.snr_sts4 = 0xc0;
	tag->t1.snr_sts5 = 0xff;
	tag->t1.snr_sts6 = 0xff;
	tag->t1.snr_sts7 = 0xff;

	if (ebf) {
		tag->t1.row_id1 = 0;
		tag->t1.row_id2 = 1;
		tag->t1.row_id3 = 2;
		tag->t1.row_id4 = 3;
		tag->t1.lm = mode_to_lm[MT76_TM_TX_MODE_HT];
	} else {
		tag->t1.row_id1 = 4;
		tag->t1.row_id2 = 5;
		tag->t1.row_id3 = 6;
		tag->t1.row_id4 = 7;
		tag->t1.lm = mode_to_lm[MT76_TM_TX_MODE_OFDM];

		tag->t2.ibf_timeout = 0xff;
		tag->t2.ibf_nr = nr;
	}

	ret = mt7915_tm_txbf_profile_tag_write(phy, pfmu_idx, tag);
	if (ret)
		return ret;

	wcid = list_first_entry(&td->tm_entry_list, struct mt76_wcid, list);
	ret = mt7915_tm_add_txbf(phy, vif, wcid_to_sta(wcid), pfmu_idx, nr, nc, ebf);
	if (ret)
		return ret;

	if (!ebf && is_atenl)
		return mt7915_tm_txbf_apply_tx(phy, 1, false, true, true);

	return 0;
}

static int
mt7915_tm_txbf_phase_cal(struct mt7915_phy *phy, u16 *val)
{
#define GROUP_L		0
#define GROUP_M		1
#define GROUP_H		2
	struct mt7915_dev *dev = phy->dev;
	struct {
		u8 category;
		u8 group_l_m_n;
		u8 group;
		bool sx2;
		u8 cal_type;
		u8 lna_gain_level;
		u8 _rsv[2];
	} __packed req = {
		.category = MT_BF_PHASE_CAL,
		.group = val[0],
		.group_l_m_n = val[1],
		.sx2 = val[2],
		.cal_type = val[3],
		.lna_gain_level = val[4],
	};
	struct mt7915_tm_txbf_phase *phase =
		(struct mt7915_tm_txbf_phase *)dev->test.txbf_phase_cal;

	phase[req.group].status = 0;

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(TXBF_ACTION), &req,
				 sizeof(req), true);
}

int mt7915_tm_txbf_status_read(struct mt7915_dev *dev, struct sk_buff *skb)
{
#define BF_PFMU_TAG	16
#define BF_CAL_PHASE	21
	u8 format_id;

	skb_pull(skb, sizeof(struct mt76_connac2_mcu_rxd));
	format_id = *(u8 *)skb->data;

	if (format_id == BF_PFMU_TAG) {
		struct mt7915_tm_pfmu_tag *tag = dev->test.txbf_pfmu_tag;

		skb_pull(skb, 8);
		memcpy(tag, skb->data, sizeof(struct mt7915_tm_pfmu_tag));
	} else if (format_id == BF_CAL_PHASE) {
		struct mt7915_tm_ibf_cal_info *cal;
		struct mt7915_tm_txbf_phase *phase =
			(struct mt7915_tm_txbf_phase *)dev->test.txbf_phase_cal;

		cal = (struct mt7915_tm_ibf_cal_info *)skb->data;
		switch (cal->cal_type) {
		case IBF_PHASE_CAL_NORMAL:
		case IBF_PHASE_CAL_NORMAL_INSTRUMENT:
			if (cal->group_l_m_n != GROUP_M)
				break;
			phase = &phase[cal->group];
			memcpy(&phase->phase, cal->buf + 16, sizeof(phase->phase));
			phase->status = cal->status;
			/* for passing iTest script */
			dev_info(dev->mt76.dev, "Calibrated result = %d\n", phase->status);
			break;
		case IBF_PHASE_CAL_VERIFY:
		case IBF_PHASE_CAL_VERIFY_INSTRUMENT:
			break;
		default:
			break;
		}
	}

	wake_up(&dev->mt76.tx_wait);

	return 0;
}

static int
mt7915_tm_txbf_profile_update_all(struct mt7915_phy *phy, u16 *val)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	u16 pfmu_idx = val[0];
	u16 subc_id = val[1];
	u16 angle11 = val[2];
	u16 angle21 = val[3];
	u16 angle31 = val[4];
	u16 angle41 = val[5];
	s16 phi11 = 0, phi21 = 0, phi31 = 0;
	struct mt7915_tm_pfmu_data *pfmu_data;

	if (subc_id > 63)
		return -EINVAL;

	if (td->tx_antenna_mask == 2) {
		phi11 = (s16)(angle21 - angle11);
	} else if (td->tx_antenna_mask == 3) {
		phi11 = (s16)(angle31 - angle11);
		phi21 = (s16)(angle31 - angle21);
	} else {
		phi11 = (s16)(angle41 - angle11);
		phi21 = (s16)(angle41 - angle21);
		phi31 = (s16)(angle41 - angle31);
	}

	pfmu_data = (struct mt7915_tm_pfmu_data *)phy->dev->test.txbf_pfmu_data;
	pfmu_data = &pfmu_data[subc_id];

	if (subc_id < 32)
		pfmu_data->subc_idx = cpu_to_le16(subc_id + 224);
	else
		pfmu_data->subc_idx = cpu_to_le16(subc_id - 32);
	pfmu_data->phi11 = cpu_to_le16(phi11);
	pfmu_data->phi21 = cpu_to_le16(phi21);
	pfmu_data->phi31 = cpu_to_le16(phi31);
	if (subc_id == 63) {
		struct mt7915_dev *dev = phy->dev;
		struct {
			u8 format_id;
			u8 pfmu_idx;
			u8 dbdc_idx;
			u8 _rsv;
			u8 buf[512];
		} __packed req = {
			.format_id = MT_BF_PROFILE_WRITE_ALL,
			.pfmu_idx = pfmu_idx,
			.dbdc_idx = phy != &dev->phy,
		};

		memcpy(req.buf, dev->test.txbf_pfmu_data, 512);

		return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(TXBF_ACTION),
					 &req, sizeof(req), true);
	}

	return 0;
}

static int
mt7915_tm_txbf_e2p_update(struct mt7915_phy *phy)
{
	struct mt7915_tm_txbf_phase *phase, *p;
	struct mt7915_dev *dev = phy->dev;
	u8 *eeprom = dev->mt76.eeprom.data;
	u16 offset;
	bool is_7976;
	int i;

	is_7976 = mt7915_check_adie(dev, false) || is_mt7916(&dev->mt76);
	offset = is_7976 ? 0x60a : 0x651;

	phase = (struct mt7915_tm_txbf_phase *)dev->test.txbf_phase_cal;
	for (i = 0; i < MAX_PHASE_GROUP_NUM; i++) {
		p = &phase[i];

		if (!p->status)
			continue;

		/* copy phase cal data to eeprom */
		memcpy(eeprom + offset + i * sizeof(p->phase), &p->phase,
		       sizeof(p->phase));
	}

	return 0;
}

static int
mt7915_tm_set_txbf(struct mt7915_phy *phy)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	u16 *val = td->txbf_param;

	pr_info("ibf cal process: act = %u, val = %u, %u, %u, %u, %u, %u\n",
		td->txbf_act, val[0], val[1], val[2], val[3], val[4], val[5]);

	switch (td->txbf_act) {
	case MT76_TM_TXBF_ACT_INIT:
		return mt7915_tm_txbf_init(phy, val);
	case MT76_TM_TXBF_ACT_UPDATE_CH:
		mt7915_tm_update_channel(phy);
		break;
	case MT76_TM_TXBF_ACT_PHASE_COMP:
		return mt7915_tm_txbf_phase_comp(phy, val);
	case MT76_TM_TXBF_ACT_TX_PREP:
		return mt7915_tm_txbf_set_tx(phy, val);
	case MT76_TM_TXBF_ACT_IBF_PROF_UPDATE:
		return mt7915_tm_txbf_profile_update(phy, val, false);
	case MT76_TM_TXBF_ACT_EBF_PROF_UPDATE:
		return mt7915_tm_txbf_profile_update(phy, val, true);
	case MT76_TM_TXBF_ACT_PHASE_CAL:
		return mt7915_tm_txbf_phase_cal(phy, val);
	case MT76_TM_TXBF_ACT_PROF_UPDATE_ALL_CMD:
	case MT76_TM_TXBF_ACT_PROF_UPDATE_ALL:
		return mt7915_tm_txbf_profile_update_all(phy, val);
	case MT76_TM_TXBF_ACT_E2P_UPDATE:
		return mt7915_tm_txbf_e2p_update(phy);
	case MT76_TM_TXBF_ACT_APPLY_TX: {
		u16 wlan_idx = val[0];
		bool ebf = !!val[1], ibf = !!val[2], phase_cal = !!val[4];

		return mt7915_tm_txbf_apply_tx(phy, wlan_idx, ebf, ibf, phase_cal);
	}
	default:
		break;
	};

	return 0;
}

static int
mt7915_tm_set_wmm_qid(struct mt7915_phy *phy, u8 qid, u8 aifs, u8 cw_min,
		      u16 cw_max, u16 txop, u8 tx_cmd)
{
	struct mt7915_vif *mvif = (struct mt7915_vif *)phy->monitor_vif->drv_priv;
	struct mt7915_mcu_tx req = {
		.valid = true,
		.mode = tx_cmd,
		.total = 1,
	};
	struct edca *e = &req.edca[0];

	e->queue = qid + mvif->mt76.wmm_idx * MT76_CONNAC_MAX_WMM_SETS;
	e->set = WMM_PARAM_SET;

	e->aifs = aifs;
	e->cw_min = cw_min;
	e->cw_max = cpu_to_le16(cw_max);
	e->txop = cpu_to_le16(txop);

	return mt7915_mcu_update_edca(phy->dev, &req);
}

static int
mt7915_tm_set_ipg_params(struct mt7915_phy *phy, u32 ipg, u8 mode)
{
#define TM_DEFAULT_SIFS	10
#define TM_MAX_SIFS	127
#define TM_MAX_AIFSN	0xf
#define TM_MIN_AIFSN	0x1
#define BBP_PROC_TIME	1500
	struct mt7915_dev *dev = phy->dev;
	u8 sig_ext = (mode == MT76_TM_TX_MODE_CCK) ? 0 : 6;
	u8 slot_time = 9, sifs = TM_DEFAULT_SIFS;
	u8 aifsn = TM_MIN_AIFSN;
	u32 i2t_time, tr2t_time, txv_time;
	u16 cw = 0;

	if (ipg < sig_ext + slot_time + sifs)
		ipg = 0;

	if (!ipg)
		goto done;

	ipg -= sig_ext;

	if (ipg <= (TM_MAX_SIFS + slot_time)) {
		sifs = ipg - slot_time;
	} else {
		u32 val = (ipg + slot_time) / slot_time;

		while (val >>= 1)
			cw++;

		if (cw > 16)
			cw = 16;

		ipg -= ((1 << cw) - 1) * slot_time;

		aifsn = ipg / slot_time;
		if (aifsn > TM_MAX_AIFSN)
			aifsn = TM_MAX_AIFSN;

		ipg -= aifsn * slot_time;

		if (ipg > TM_DEFAULT_SIFS)
			sifs = min_t(u32, ipg, TM_MAX_SIFS);
	}
done:
	txv_time = mt76_get_field(dev, MT_TMAC_ATCR(phy->band_idx),
				  MT_TMAC_ATCR_TXV_TOUT);
	txv_time *= 50;	/* normal clock time */

	i2t_time = (slot_time * 1000 - txv_time - BBP_PROC_TIME) / 50;
	tr2t_time = (sifs * 1000 - txv_time - BBP_PROC_TIME) / 50;

	mt76_set(dev, MT_TMAC_TRCR0(phy->band_idx),
		 FIELD_PREP(MT_TMAC_TRCR0_TR2T_CHK, tr2t_time) |
		 FIELD_PREP(MT_TMAC_TRCR0_I2T_CHK, i2t_time));

	mt7915_tm_set_slot_time(phy, slot_time, sifs);

	return mt7915_tm_set_wmm_qid(phy,
				     mt76_connac_lmac_mapping(IEEE80211_AC_BE),
				     aifsn, cw, cw, 0,
				     mode == MT76_TM_TX_MODE_HE_MU);
}

static int
mt7915_tm_set_tx_len(struct mt7915_phy *phy, u32 tx_time)
{
	struct mt76_phy *mphy = phy->mt76;
	struct mt76_testmode_data *td = &mphy->test;
	struct ieee80211_supported_band *sband;
	struct rate_info rate = {};
	u16 flags = 0, tx_len;
	u32 bitrate;
	int ret;

	if (!tx_time)
		return 0;

	rate.mcs = td->tx_rate_idx;
	rate.nss = td->tx_rate_nss;

	switch (td->tx_rate_mode) {
	case MT76_TM_TX_MODE_CCK:
	case MT76_TM_TX_MODE_OFDM:
		if (mphy->chandef.chan->band == NL80211_BAND_5GHZ)
			sband = &mphy->sband_5g.sband;
		else if (mphy->chandef.chan->band == NL80211_BAND_6GHZ)
			sband = &mphy->sband_6g.sband;
		else
			sband = &mphy->sband_2g.sband;

		rate.legacy = sband->bitrates[rate.mcs].bitrate;
		break;
	case MT76_TM_TX_MODE_HT:
		flags |= RATE_INFO_FLAGS_MCS;

		if (td->tx_rate_sgi)
			flags |= RATE_INFO_FLAGS_SHORT_GI;
		break;
	case MT76_TM_TX_MODE_VHT:
		flags |= RATE_INFO_FLAGS_VHT_MCS;

		if (td->tx_rate_sgi)
			flags |= RATE_INFO_FLAGS_SHORT_GI;
		break;
	case MT76_TM_TX_MODE_HE_SU:
	case MT76_TM_TX_MODE_HE_EXT_SU:
	case MT76_TM_TX_MODE_HE_TB:
	case MT76_TM_TX_MODE_HE_MU:
		rate.he_gi = td->tx_rate_sgi;
		flags |= RATE_INFO_FLAGS_HE_MCS;
		break;
	default:
		break;
	}
	rate.flags = flags;

	switch (mphy->chandef.width) {
	case NL80211_CHAN_WIDTH_160:
	case NL80211_CHAN_WIDTH_80P80:
		rate.bw = RATE_INFO_BW_160;
		break;
	case NL80211_CHAN_WIDTH_80:
		rate.bw = RATE_INFO_BW_80;
		break;
	case NL80211_CHAN_WIDTH_40:
		rate.bw = RATE_INFO_BW_40;
		break;
	default:
		rate.bw = RATE_INFO_BW_20;
		break;
	}

	bitrate = cfg80211_calculate_bitrate(&rate);
	tx_len = bitrate * tx_time / 10 / 8;

	ret = mt76_testmode_init_skb(phy->mt76, tx_len, &td->tx_skb, td->addr);
	if (ret)
		return ret;

	return 0;
}

static void
mt7915_tm_reg_backup_restore(struct mt7915_phy *phy)
{
	int n_regs = ARRAY_SIZE(reg_backup_list);
	struct mt7915_dev *dev = phy->dev;
	u32 *b = phy->test.reg_backup, val;
	int i;

	REG_BAND_IDX(reg_backup_list[0], AGG_PCR0, 0);
	REG_BAND_IDX(reg_backup_list[1], AGG_PCR0, 1);
	REG_BAND_IDX(reg_backup_list[2], AGG_AWSCR0, 0);
	REG_BAND_IDX(reg_backup_list[3], AGG_AWSCR0, 1);
	REG_BAND_IDX(reg_backup_list[4], AGG_AWSCR0, 2);
	REG_BAND_IDX(reg_backup_list[5], AGG_AWSCR0, 3);
	REG_BAND(reg_backup_list[6], AGG_MRCR);
	REG_BAND(reg_backup_list[7], TMAC_TFCR0);
	REG_BAND(reg_backup_list[8], TMAC_TCR0);
	REG_BAND(reg_backup_list[9], TMAC_TCR2);
	REG_BAND(reg_backup_list[10], AGG_ATCR1);
	REG_BAND(reg_backup_list[11], AGG_ATCR3);
	REG_BAND(reg_backup_list[12], TMAC_TRCR0);
	REG_BAND(reg_backup_list[13], TMAC_ICR0);
	REG_BAND_IDX(reg_backup_list[14], ARB_DRNGR0, 0);
	REG_BAND_IDX(reg_backup_list[15], ARB_DRNGR0, 1);
	REG_BAND(reg_backup_list[16], WF_RFCR);
	REG_BAND(reg_backup_list[17], WF_RFCR1);

	if (is_mt7916(&dev->mt76)) {
		reg_backup_list[18].band[phy->band_idx] = MT_MDP_TOP_DBG_WDT_CTRL;
		reg_backup_list[19].band[phy->band_idx] = MT_MDP_TOP_DBG_CTRL;
	}

	if (phy->mt76->test.state == MT76_TM_STATE_OFF) {
		for (i = 0; i < n_regs; i++) {
			u8 reg = reg_backup_list[i].band[phy->band_idx];

			if (reg)
				mt76_wr(dev, reg, b[i]);
		}
		return;
	}

	if (!b) {
		b = devm_kzalloc(dev->mt76.dev, 4 * n_regs, GFP_KERNEL);
		if (!b)
			return;

		phy->test.reg_backup = b;
		for (i = 0; i < n_regs; i++)
			b[i] = mt76_rr(dev, reg_backup_list[i].band[phy->band_idx]);
	}

	mt76_clear(dev, MT_AGG_PCR0(phy->band_idx, 0), MT_AGG_PCR0_MM_PROT |
		   MT_AGG_PCR0_GF_PROT | MT_AGG_PCR0_ERP_PROT |
		   MT_AGG_PCR0_VHT_PROT | MT_AGG_PCR0_BW20_PROT |
		   MT_AGG_PCR0_BW40_PROT | MT_AGG_PCR0_BW80_PROT);
	mt76_set(dev, MT_AGG_PCR0(phy->band_idx, 0), MT_AGG_PCR0_PTA_WIN_DIS);

	if (is_mt7915(&dev->mt76))
		val = MT_AGG_PCR1_RTS0_NUM_THRES | MT_AGG_PCR1_RTS0_LEN_THRES;
	else
		val = MT_AGG_PCR1_RTS0_NUM_THRES_MT7916 |
		      MT_AGG_PCR1_RTS0_LEN_THRES_MT7916;

	mt76_wr(dev, MT_AGG_PCR0(phy->band_idx, 1), val);

	mt76_clear(dev, MT_AGG_MRCR(phy->band_idx), MT_AGG_MRCR_BAR_CNT_LIMIT |
		   MT_AGG_MRCR_LAST_RTS_CTS_RN | MT_AGG_MRCR_RTS_FAIL_LIMIT |
		   MT_AGG_MRCR_TXCMD_RTS_FAIL_LIMIT);

	mt76_rmw(dev, MT_AGG_MRCR(phy->band_idx), MT_AGG_MRCR_RTS_FAIL_LIMIT |
		 MT_AGG_MRCR_TXCMD_RTS_FAIL_LIMIT,
		 FIELD_PREP(MT_AGG_MRCR_RTS_FAIL_LIMIT, 1) |
		 FIELD_PREP(MT_AGG_MRCR_TXCMD_RTS_FAIL_LIMIT, 1));

	mt76_wr(dev, MT_TMAC_TFCR0(phy->band_idx), 0);
	mt76_clear(dev, MT_TMAC_TCR0(phy->band_idx), MT_TMAC_TCR0_TBTT_STOP_CTRL);
	mt76_set(dev, MT_TMAC_TCR2(phy->band_idx), MT_TMAC_TCR2_SCH_DET_DIS);

	/* config rx filter for testmode rx */
	mt76_wr(dev, MT_WF_RFCR(phy->band_idx), 0xcf70a);
	mt76_wr(dev, MT_WF_RFCR1(phy->band_idx), 0);

	if (is_mt7916(&dev->mt76)) {
		/* enable MDP Tx block mode */
		mt76_clear(dev, MT_MDP_TOP_DBG_WDT_CTRL,
			   MT_MDP_TOP_DBG_WDT_CTRL_TDP_DIS_BLK);
		mt76_clear(dev, MT_MDP_TOP_DBG_CTRL,
			   MT_MDP_TOP_DBG_CTRL_ENQ_MODE);
	}
}

static void
mt7915_tm_init(struct mt7915_phy *phy, bool en)
{
	struct mt7915_dev *dev = phy->dev;

	if (!test_bit(MT76_STATE_RUNNING, &phy->mt76->state))
		return;

	mt7915_mcu_set_sku_en(phy, !en);

	mt7915_tm_mode_ctrl(dev, en);
	mt7915_tm_reg_backup_restore(phy);
	mt7915_tm_set_trx(phy, TM_MAC_TXRX, !en);

	mt7915_mcu_add_bss_info(phy, phy->monitor_vif, en);
	mt7915_mcu_add_sta(dev, phy->monitor_vif, NULL, en);

	phy->mt76->test.flag |= MT_TM_FW_RX_COUNT;

	if (!en) {
		mt7915_tm_set_tam_arb(phy, en, 0);

		phy->mt76->test.aid = 0;
		phy->mt76->test.tx_mpdu_len = 0;
		phy->test.bf_en = 0;
		mt7915_tm_set_entry(phy);
	}
}

static bool
mt7915_tm_check_skb(struct mt7915_phy *phy)
{
	struct mt76_testmode_entry_data *ed;
	struct mt76_wcid *wcid;

	mt76_tm_for_each_entry(phy->mt76, wcid, ed) {
		struct ieee80211_tx_info *info;

		if (!ed->tx_skb)
			return false;

		info = IEEE80211_SKB_CB(ed->tx_skb);
		info->control.vif = phy->monitor_vif;
	}

	return true;
}

static int
mt7915_tm_set_ba(struct mt7915_phy *phy)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_wcid *wcid;
	struct ieee80211_vif *vif = phy->monitor_vif;
	struct mt7915_vif *mvif = (struct mt7915_vif *)vif->drv_priv;
	struct ieee80211_ampdu_params params = { .buf_size = 256 };

	list_for_each_entry(wcid, &td->tm_entry_list, list) {
		int tid, ret;

		params.sta = wcid_to_sta(wcid);
		for (tid = 0; tid < 8; tid++) {
			params.tid = tid;
			ret = mt7915_mcu_add_tx_ba(phy->dev, &params, true);
			if (ret)
				return ret;
		}
	}

	mt76_wr(dev, MT_AGG_AALCR0(mvif->mt76.band_idx, mvif->mt76.wmm_idx),
		0x01010101);

	return 0;
}

static int
mt7915_tm_set_muru_cfg(struct mt7915_phy *phy, struct mt7915_tm_muru *muru)
{
/* #define MURU_SET_MANUAL_CFG	100 */
	struct mt7915_dev *dev = phy->dev;
	struct {
		__le32 cmd;
		struct mt7915_tm_muru muru;
	} __packed req = {
		.cmd = cpu_to_le32(MURU_SET_MANUAL_CFG),
	};

	memcpy(&req.muru, muru, sizeof(struct mt7915_tm_muru));

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(MURU_CTRL), &req,
				 sizeof(req), false);
}

static int
mt7915_tm_set_muru_dl(struct mt7915_phy *phy)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt76_testmode_entry_data *ed;
	struct mt76_wcid *wcid;
	struct cfg80211_chan_def *chandef = &phy->mt76->chandef;
	struct ieee80211_vif *vif = phy->monitor_vif;
	struct mt7915_vif *mvif = (struct mt7915_vif *)vif->drv_priv;
	struct mt7915_tm_muru muru = {};
	struct mt7915_tm_muru_comm *comm = &muru.comm;
	struct mt7915_tm_muru_dl *dl = &muru.dl;
	int i;

	comm->ppdu_format = MURU_PPDU_HE_MU;
	comm->band = mvif->mt76.band_idx;
	comm->wmm_idx = mvif->mt76.wmm_idx;
	comm->spe_idx = phy->test.spe_idx;

	dl->bw = mt7915_tm_chan_bw(chandef->width);
	dl->gi = td->tx_rate_sgi;;
	dl->ltf = td->tx_ltf;
	dl->tx_mode = MT_PHY_TYPE_HE_MU;

	for (i = 0; i < sizeof(dl->ru); i++)
		dl->ru[i] = 0x71;

	mt76_tm_for_each_entry(phy->mt76, wcid, ed) {
		struct mt7915_tm_muru_dl_usr *dl_usr = &dl->usr[dl->user_num];

		dl_usr->wlan_idx = cpu_to_le16(wcid->idx);
		dl_usr->ru_alloc_seg = ed->aid < 8 ? 0 : 1;
		dl_usr->ru_idx = ed->ru_idx;
		dl_usr->mcs = ed->tx_rate_idx;
		dl_usr->nss = ed->tx_rate_nss - 1;
		dl_usr->ldpc = ed->tx_rate_ldpc;
		dl->ru[dl->user_num] = ed->ru_alloc;

		dl->user_num++;
	}

	muru.cfg_comm = cpu_to_le32(MURU_COMM_SET);
	muru.cfg_dl = cpu_to_le32(MURU_DL_SET);

	return mt7915_tm_set_muru_cfg(phy, &muru);
}

static int
mt7915_tm_set_muru_pkt_cnt(struct mt7915_phy *phy, bool enable, u32 tx_count)
{
#define MURU_SET_TX_PKT_CNT 105
#define MURU_SET_TX_EN 106
	struct mt7915_dev *dev = phy->dev;
	struct {
		__le32 cmd;
		u8 band;
		u8 enable;
		u8 _rsv[2];
		__le32 tx_count;
	} __packed req = {
		.band = phy != &dev->phy,
		.enable = enable,
		.tx_count = enable ? cpu_to_le32(tx_count) : 0,
	};
	int ret;

	req.cmd = enable ? cpu_to_le32(MURU_SET_TX_PKT_CNT) :
			   cpu_to_le32(MURU_SET_TX_EN);

	ret = mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(MURU_CTRL), &req,
				sizeof(req), false);
	if (ret)
		return ret;

	req.cmd = enable ? cpu_to_le32(MURU_SET_TX_EN) :
			   cpu_to_le32(MURU_SET_TX_PKT_CNT);

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(MURU_CTRL), &req,
				 sizeof(req), false);
}

static void
mt7915_tm_tx_frames_mu(struct mt7915_phy *phy, bool enable)
{
	struct mt76_testmode_data *td = &phy->mt76->test;

	if (enable) {
		struct mt7915_dev *dev = phy->dev;

		mt7915_tm_set_ba(phy);
		mt7915_tm_set_muru_dl(phy);
		mt76_rr(dev, MT_MIB_DR8(phy != &dev->phy));
	} else {
		/* set to zero for counting real tx free num */
		td->tx_done = 0;
	}

	mt7915_tm_set_muru_pkt_cnt(phy, enable, td->tx_count);
	usleep_range(100000, 200000);
}

static void
mt7915_tm_set_tx_frames(struct mt7915_phy *phy, bool en)
{
	static const u8 spe_idx_map[] = {0, 0, 1, 0, 3, 2, 4, 0,
					 9, 8, 6, 10, 16, 12, 18, 0};
	struct mt76_testmode_data *td = &phy->mt76->test;

	mt7915_tm_set_trx(phy, TM_MAC_RX_RXV, false);
	mt7915_tm_set_trx(phy, TM_MAC_TX, false);

	if (en) {
		u32 tx_time = td->tx_time, ipg = td->tx_ipg;
		u8 duty_cycle = td->tx_duty_cycle;

		if (!phy->test.bf_en)
			mt7915_tm_update_channel(phy);

		if (td->tx_spe_idx)
			phy->test.spe_idx = td->tx_spe_idx;
		else
			phy->test.spe_idx = mt76_connac_spe_idx(td->tx_antenna_mask);

		/* if all three params are set, duty_cycle will be ignored */
		if (duty_cycle && tx_time && !ipg) {
			ipg = tx_time * 100 / duty_cycle - tx_time;
		} else if (duty_cycle && !tx_time && ipg) {
			if (duty_cycle < 100)
				tx_time = duty_cycle * ipg / (100 - duty_cycle);
		}
		mt7915_tm_set_ipg_params(phy, ipg, td->tx_rate_mode);
		mt7915_tm_set_tx_len(phy, tx_time);

		if (ipg)
			td->tx_queued_limit = MT76_TM_TIMEOUT * 1000000 / ipg / 2;

		if (!mt7915_tm_check_skb(phy))
			return;
	} else {
		mt7915_tm_clean_hwq(phy);
	}

	mt7915_tm_set_tam_arb(phy, en,
			      td->tx_rate_mode == MT76_TM_TX_MODE_HE_MU);

	if (td->tx_rate_mode == MT76_TM_TX_MODE_HE_MU)
		mt7915_tm_tx_frames_mu(phy, en);

	mt7915_tm_set_trx(phy, TM_MAC_TX, en);
}

static int
mt7915_tm_get_rx_stats(struct mt7915_phy *phy, bool clear)
{
#define CMD_RX_STAT_BAND	0x3
	struct mt76_testmode_data *td = &phy->mt76->test;
	struct mt7915_tm_rx_stat_band *rs_band;
	struct mt7915_dev *dev = phy->dev;
	struct sk_buff *skb;
	struct {
		u8 format_id;
		u8 band;
		u8 _rsv[2];
	} __packed req = {
		.format_id = CMD_RX_STAT_BAND,
		.band = phy != &dev->phy,
	};
	int ret;

	ret = mt76_mcu_send_and_get_msg(&dev->mt76, MCU_EXT_CMD(RX_STAT),
					&req, sizeof(req), true, &skb);
	if (ret)
		return ret;

	rs_band = (struct mt7915_tm_rx_stat_band *)skb->data;

	if (!clear) {
		enum mt76_rxq_id q = req.band ? MT_RXQ_BAND1 : MT_RXQ_MAIN;

		td->rx_stats.packets[q] += le32_to_cpu(rs_band->mdrdy_cnt);
		td->rx_stats.fcs_error[q] += le16_to_cpu(rs_band->fcs_err);
		td->rx_stats.len_mismatch += le16_to_cpu(rs_band->len_mismatch);
	}

	dev_kfree_skb(skb);

	return 0;
}

static int
mt7915_tm_set_rx_user_idx(struct mt7915_phy *phy, u8 aid)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt76_wcid *wcid = NULL;
	struct mt76_testmode_entry_data *ed;
	struct {
		u8 band;
		u8 _rsv;
		__le16 wlan_idx;
	} __packed req = {
		.band = phy->band_idx,
	};

	mt76_tm_for_each_entry(phy->mt76, wcid, ed)
		if (ed->aid == aid)
			break;

	if (!wcid)
		return -EINVAL;

	req.wlan_idx = cpu_to_le16(wcid->idx);

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(RX_STAT_USER_CTRL),
				 &req, sizeof(req), false);
}

static int
mt7915_tm_set_muru_aid(struct mt7915_phy *phy, u16 aid)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_tm_cmd req = {
		.testmode_en = 1,
		.param_idx = MCU_ATE_SET_MU_RX_AID,
		.param.rx_aid.band = cpu_to_le32(phy->band_idx),
		.param.rx_aid.aid = cpu_to_le16(aid),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(ATE_CTRL), &req,
				 sizeof(req), false);
}

static void
mt7915_tm_set_rx_frames(struct mt7915_phy *phy, bool en)
{
	struct mt76_testmode_data *td = &phy->mt76->test;

	mt7915_tm_set_trx(phy, TM_MAC_TX, false);
	mt7915_tm_set_trx(phy, TM_MAC_RX_RXV, false);

	if (en) {
		if (!phy->test.bf_en)
			mt7915_tm_update_channel(phy);
		if (td->aid)
			mt7915_tm_set_rx_user_idx(phy, td->aid);

		/* read-clear */
		mt7915_tm_get_rx_stats(phy, true);

		/* clear fw count */
		mt7915_tm_set_phy_count(phy, 0);
		mt7915_tm_set_phy_count(phy, 1);
	}

	if (td->tx_rate_mode == MT76_TM_TX_MODE_HE_MU)
		mt7915_tm_set_muru_aid(phy, en ? td->aid : 0xf800);

	mt7915_tm_set_trx(phy, TM_MAC_RX_RXV, en);
}

static int
mt7915_tm_rf_switch_mode(struct mt7915_dev *dev, u32 oper)
{
	struct mt7915_tm_rf_test req = {
		.op.op_mode = cpu_to_le32(oper),
	};

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(RF_TEST), &req,
				 sizeof(req), true);
}

static int
mt7915_tm_set_tx_cont(struct mt7915_phy *phy, bool en)
{
	struct mt7915_dev *dev = phy->dev;
	struct cfg80211_chan_def *chandef = &phy->mt76->chandef;
	int freq1 = ieee80211_frequency_to_channel(chandef->center_freq1);
	struct mt76_testmode_data *td = &phy->mt76->test;
	u32 func_idx = en ? RF_TEST_TX_CONT_START : RF_TEST_TX_CONT_STOP;
	u8 rate_idx = td->tx_rate_idx, mode;
	u16 rateval;
	struct mt7915_tm_rf_test req = {
		.action = RF_ACT_IN_RFTEST,
		.icap_len = 120,
		.op.rf.func_idx = cpu_to_le32(func_idx),
	};
	struct tm_tx_cont *tx_cont = &req.op.rf.param.tx_cont;

	tx_cont->control_ch = chandef->chan->hw_value;
	tx_cont->center_ch = freq1;
	tx_cont->tx_ant = td->tx_antenna_mask;
	tx_cont->band = phy != &dev->phy;
	tx_cont->bw = mt7915_tm_chan_bw(chandef->width);

	if (!en) {
		req.op.rf.param.func_data = cpu_to_le32(phy != &dev->phy);
		goto out;
	}

	if (td->tx_rate_mode <= MT76_TM_TX_MODE_OFDM) {
		struct ieee80211_supported_band *sband;
		u8 idx = rate_idx;

		if (chandef->chan->band == NL80211_BAND_5GHZ)
			sband = &phy->mt76->sband_5g.sband;
		else if (chandef->chan->band == NL80211_BAND_6GHZ)
			sband = &phy->mt76->sband_6g.sband;
		else
			sband = &phy->mt76->sband_2g.sband;

		if (td->tx_rate_mode == MT76_TM_TX_MODE_OFDM)
			idx += 4;
		rate_idx = sband->bitrates[idx].hw_value & 0xff;
	}

	switch (td->tx_rate_mode) {
	case MT76_TM_TX_MODE_CCK:
		mode = MT_PHY_TYPE_CCK;
		break;
	case MT76_TM_TX_MODE_OFDM:
		mode = MT_PHY_TYPE_OFDM;
		break;
	case MT76_TM_TX_MODE_HT:
		mode = MT_PHY_TYPE_HT;
		break;
	case MT76_TM_TX_MODE_VHT:
		mode = MT_PHY_TYPE_VHT;
		break;
	case MT76_TM_TX_MODE_HE_SU:
		mode = MT_PHY_TYPE_HE_SU;
		break;
	case MT76_TM_TX_MODE_HE_EXT_SU:
		mode = MT_PHY_TYPE_HE_EXT_SU;
		break;
	case MT76_TM_TX_MODE_HE_TB:
		mode = MT_PHY_TYPE_HE_TB;
		break;
	case MT76_TM_TX_MODE_HE_MU:
		mode = MT_PHY_TYPE_HE_MU;
		break;
	default:
		return -EINVAL;
	}

	rateval =  mode << 6 | rate_idx;
	tx_cont->rateval = cpu_to_le16(rateval);

out:
	if (!en) {
		int ret;

		ret = mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(RF_TEST), &req,
					sizeof(req), true);
		if (ret)
			return ret;

		return mt7915_tm_rf_switch_mode(dev, RF_OPER_NORMAL);
	}

	mt7915_tm_rf_switch_mode(dev, RF_OPER_RF_TEST);
	mt7915_tm_update_channel(phy);

	return mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(RF_TEST), &req,
				 sizeof(req), true);
}

static int
mt7915_tm_group_prek(struct mt7915_phy *phy, enum mt76_testmode_state state)
{
	u8 *eeprom;
	u32 i, group_size, dpd_size, size, offs, *pre_cal;
	int ret = 0;
	struct mt7915_dev *dev = phy->dev;
	struct mt76_dev *mdev = &dev->mt76;
	struct mt7915_tm_rf_test req = {
		.action = RF_ACT_IN_RFTEST,
		.icap_len = 8,
		.op.rf.func_idx = cpu_to_le32(RF_TEST_RE_CAL),
	};

	if (!dev->flash_mode && !dev->bin_file_mode) {
		dev_err(dev->mt76.dev, "Currently not in FLASH or BIN MODE,return!\n");
		return 1;
	}

	eeprom = mdev->eeprom.data;
	dev->cur_prek_offset = 0;
	group_size = mt7915_get_cal_group_size(dev);
	dpd_size = is_mt7915(&dev->mt76) ? MT_EE_CAL_DPD_SIZE_V1 : MT_EE_CAL_DPD_SIZE_V2;
	size = group_size + dpd_size;
	offs = is_mt7915(&dev->mt76) ? MT_EE_DO_PRE_CAL : MT_EE_DO_PRE_CAL_V2;

	switch (state) {
	case MT76_TM_STATE_GROUP_PREK:
		req.op.rf.param.cal_param.func_data = cpu_to_le32(RF_PRE_CAL);

		if (!dev->cal) {
			dev->cal = devm_kzalloc(mdev->dev, size, GFP_KERNEL);
			if (!dev->cal)
				return -ENOMEM;
		}

		ret = mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(RF_TEST), &req,
					sizeof(req), true);

		if (!ret)
			eeprom[offs] |= MT_EE_WIFI_CAL_GROUP;
		break;
	case MT76_TM_STATE_GROUP_PREK_DUMP:
		pre_cal = (u32 *)dev->cal;
		if (!pre_cal) {
			dev_info(dev->mt76.dev, "Not group pre-cal yet!\n");
			return ret;
		}
		dev_info(dev->mt76.dev, "Group Pre-Cal:\n");
		for (i = 0; i < (group_size / sizeof(u32)); i += 4) {
			dev_info(dev->mt76.dev, "[0x%08x] 0x%8x 0x%8x 0x%8x 0x%8x\n",
				 i * sizeof(u32), pre_cal[i], pre_cal[i + 1],
				 pre_cal[i + 2], pre_cal[i + 3]);
		}
		break;
	case MT76_TM_STATE_GROUP_PREK_CLEAN:
		pre_cal = (u32 *)dev->cal;
		if (!pre_cal)
			return ret;
		memset(pre_cal, 0, group_size);
		eeprom[offs] &= ~MT_EE_WIFI_CAL_GROUP;
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

static int
mt7915_tm_dpd_prek(struct mt7915_phy *phy, enum mt76_testmode_state state)
{
#define DPD_2G_CH_BW20_BITMAP_0         0x444
#define DPD_5G_CH_BW20_BITMAP_0         0xffffc0ff
#define DPD_5G_CH_BW20_BITMAP_1         0x3
#define DPD_5G_CH_BW20_BITMAP_7915_0    0x7dffc0ff
#define DPD_6G_CH_BW20_BITMAP_0         0xffffffff
#define DPD_6G_CH_BW20_BITMAP_1         0x07ffffff
	bool is_set = false;
	u8 band, do_precal, *eeprom;
	u16 bw20_size, bw160_size;
	u32 i, j, *bw160_freq, bw160_5g_freq[] = {5250, 5570, 5815};
	u32 bw160_6g_freq[] = {6025, 6185, 6345, 6505, 6665, 6825, 6985};
	u32 shift, freq, group_size, dpd_size, size, offs, *pre_cal, dpd_ch_bw20_bitmap[2] = {0};
	__le32 func_data = 0;
	int ret = 0;
	struct mt7915_dev *dev = phy->dev;
	struct mt76_dev *mdev = &dev->mt76;
	struct mt76_phy *mphy = phy->mt76;
	struct cfg80211_chan_def chandef_backup, *chandef = &mphy->chandef;
	struct ieee80211_channel chan_backup, chan, *bw20_ch;
	struct mt7915_tm_rf_test req = {
		.action = RF_ACT_IN_RFTEST,
		.icap_len = 8,
		.op.rf.func_idx = cpu_to_le32(RF_TEST_RE_CAL),
	};

	if (!dev->flash_mode && !dev->bin_file_mode) {
		dev_err(dev->mt76.dev, "Currently not in FLASH or BIN MODE,return!\n");
		return -EOPNOTSUPP;
	}

	eeprom = mdev->eeprom.data;
	dev->cur_prek_offset = 0;
	group_size = mt7915_get_cal_group_size(dev);
	dev->dpd_chan_num_2g = hweight32(DPD_2G_CH_BW20_BITMAP_0);
	if (is_mt7915(&dev->mt76)) {
		dev->dpd_chan_num_5g = hweight32(DPD_5G_CH_BW20_BITMAP_7915_0);
		dev->dpd_chan_num_6g = 0;
		dpd_size = MT_EE_CAL_DPD_SIZE_V1;
		offs = MT_EE_DO_PRE_CAL;
	} else {
		dev->dpd_chan_num_5g = hweight32(DPD_5G_CH_BW20_BITMAP_0) +
				       hweight32(DPD_5G_CH_BW20_BITMAP_1) +
				       ARRAY_SIZE(bw160_5g_freq);
		dev->dpd_chan_num_6g = hweight32(DPD_6G_CH_BW20_BITMAP_0) +
				       hweight32(DPD_6G_CH_BW20_BITMAP_1) +
				       ARRAY_SIZE(bw160_6g_freq);
		dpd_size = MT_EE_CAL_DPD_SIZE_V2;
		offs = MT_EE_DO_PRE_CAL_V2;
	}
	size = group_size + dpd_size;

	switch (state) {
	case MT76_TM_STATE_DPD_2G:
		if (!is_set) {
			func_data = cpu_to_le32(RF_DPD_FLAT_CAL);
			dpd_ch_bw20_bitmap[0] = DPD_2G_CH_BW20_BITMAP_0;
			bw20_ch = mphy->sband_2g.sband.channels;
			bw160_freq = NULL;
			bw160_size = 0;
			band = NL80211_BAND_2GHZ;
			do_precal = MT_EE_WIFI_CAL_DPD_2G;
			is_set = true;
		}
		fallthrough;
	case MT76_TM_STATE_DPD_5G:
		if (!is_set) {
			if (is_mt7915(&dev->mt76)) {
				func_data = cpu_to_le32(RF_DPD_FLAT_CAL);
				dpd_ch_bw20_bitmap[0] = DPD_5G_CH_BW20_BITMAP_7915_0;
				bw160_size = 0;
				dev->cur_prek_offset -= dev->dpd_chan_num_5g * MT_EE_CAL_UNIT * 2;
			} else {
				func_data = cpu_to_le32(RF_DPD_FLAT_5G_CAL);
				dpd_ch_bw20_bitmap[0] = DPD_5G_CH_BW20_BITMAP_0;
				dpd_ch_bw20_bitmap[1] = DPD_5G_CH_BW20_BITMAP_1;
				bw160_size = ARRAY_SIZE(bw160_5g_freq);
			}
			bw20_ch = mphy->sband_5g.sband.channels;
			bw160_freq = bw160_5g_freq;
			band = NL80211_BAND_5GHZ;
			do_precal = MT_EE_WIFI_CAL_DPD_5G;
			is_set = true;
		}
		fallthrough;
	case MT76_TM_STATE_DPD_6G:
		if (!is_set) {
			func_data = cpu_to_le32(RF_DPD_FLAT_6G_CAL);
			dpd_ch_bw20_bitmap[0] = DPD_6G_CH_BW20_BITMAP_0;
			dpd_ch_bw20_bitmap[1] = DPD_6G_CH_BW20_BITMAP_1;
			bw20_ch = mphy->sband_6g.sband.channels;
			bw160_freq = bw160_6g_freq;
			bw160_size = ARRAY_SIZE(bw160_6g_freq);
			band = NL80211_BAND_6GHZ;
			do_precal = MT_EE_WIFI_CAL_DPD_6G;
			is_set = true;
		}

		if (!bw20_ch)
			return -EOPNOTSUPP;
		if (!dev->cal) {
			dev->cal = devm_kzalloc(mdev->dev, size, GFP_KERNEL);
			if (!dev->cal)
				return -ENOMEM;
		}

		req.op.rf.param.cal_param.func_data = func_data;
		req.op.rf.param.cal_param.band_idx = phy->band_idx;

		memcpy(&chan_backup, chandef->chan, sizeof(struct ieee80211_channel));
		memcpy(&chandef_backup, chandef, sizeof(struct cfg80211_chan_def));

		bw20_size = hweight32(dpd_ch_bw20_bitmap[0]) + hweight32(dpd_ch_bw20_bitmap[1]);
		for (i = 0, j = 0; i < bw20_size + bw160_size; i++) {
			if (i < bw20_size) {
				freq = dpd_ch_bw20_bitmap[0] ? 0 : 1;
				shift = ffs(dpd_ch_bw20_bitmap[freq]);
				j += shift;
				memcpy(&chan, &bw20_ch[j - 1], sizeof(struct ieee80211_channel));
				chandef->width = NL80211_CHAN_WIDTH_20;
				dpd_ch_bw20_bitmap[0] >>= shift;
			} else {
				freq = bw160_freq[i - bw20_size];
				chan.center_freq = freq;
				chan.hw_value = ieee80211_frequency_to_channel(freq);
				chan.band = band;
				chandef->width = NL80211_CHAN_WIDTH_160;
			}

			memcpy(chandef->chan, &chan, sizeof(struct ieee80211_channel));
			if (is_mt7915(&dev->mt76))
				mphy->hw->conf.flags &= ~IEEE80211_CONF_OFFCHANNEL;
			else
				mphy->hw->conf.flags |= IEEE80211_CONF_OFFCHANNEL;

			mt7915_mcu_set_chan_info(phy, MCU_EXT_CMD(CHANNEL_SWITCH));

			ret = mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(RF_TEST), &req,
						sizeof(req), true);
			if (ret) {
				dev_err(dev->mt76.dev, "DPD Pre-cal: mcu send msg failed!\n");
				break;
			}
		}
		memcpy(chandef, &chandef_backup, sizeof(struct cfg80211_chan_def));
		memcpy(chandef->chan, &chan_backup, sizeof(struct ieee80211_channel));
		mt7915_mcu_set_chan_info(phy, MCU_EXT_CMD(CHANNEL_SWITCH));

		if (!ret)
			eeprom[offs] |= do_precal;

		break;
	case MT76_TM_STATE_DPD_DUMP:
		pre_cal = (u32 *)dev->cal;
		if (!dev->cal) {
			dev_info(dev->mt76.dev, "Not DPD pre-cal yet!\n");
			return ret;
		}
		dev_info(dev->mt76.dev, "DPD Pre-Cal:\n");
		for (i = 0; i < dpd_size / sizeof(u32); i += 4) {
			j = i + (group_size / sizeof(u32));
			dev_info(dev->mt76.dev, "[0x%08x] 0x%8x 0x%8x 0x%8x 0x%8x\n",
				 j * sizeof(u32), pre_cal[j], pre_cal[j + 1],
				 pre_cal[j + 2], pre_cal[j + 3]);
		}
		break;
	case MT76_TM_STATE_DPD_CLEAN:
		pre_cal = (u32 *)dev->cal;
		if (!pre_cal)
			return ret;
		memset(pre_cal + (group_size / sizeof(u32)), 0, dpd_size);
		do_precal = MT_EE_WIFI_CAL_DPD;
		eeprom[offs] &= ~do_precal;
		break;
	default:
		return -EINVAL;
	}
	return ret;
}

void mt7915_tm_re_cal_event(struct mt7915_dev *dev, struct mt7915_tm_rf_test_result *result,
			    struct mt7915_tm_rf_test_data *data)
{
#define DPD_PER_CHAN_SIZE_7915	2
#define DPD_PER_CHAN_SIZE_7986	3
	u32 base, dpd_offest_2g, dpd_offest_5g, cal_idx = 0, cal_type = 0, len = 0;
	u8 *pre_cal;

	pre_cal = dev->cal;
	dpd_offest_5g = dev->dpd_chan_num_6g * DPD_PER_CHAN_SIZE_7986 * MT_EE_CAL_UNIT;
	dpd_offest_2g = dpd_offest_5g + dev->dpd_chan_num_5g * MT_EE_CAL_UNIT *
			(is_mt7915(&dev->mt76) ? DPD_PER_CHAN_SIZE_7915 : DPD_PER_CHAN_SIZE_7986);
	cal_idx = le32_to_cpu(data->cal_idx);
	cal_type = le32_to_cpu(data->cal_type);
	len = le32_to_cpu(result->payload_len);
	len = len - sizeof(struct mt7915_tm_rf_test_data);

	switch (cal_type) {
	case RF_PRE_CAL:
		base = 0;
		break;
	case RF_DPD_FLAT_CAL:
		base = mt7915_get_cal_group_size(dev) + dpd_offest_2g;
		break;
	case RF_DPD_FLAT_5G_CAL:
		base = mt7915_get_cal_group_size(dev) + dpd_offest_5g;
		break;
	case RF_DPD_FLAT_6G_CAL:
		base = mt7915_get_cal_group_size(dev);
		break;
	default:
		dev_info(dev->mt76.dev, "Unknown calibration type!\n");
		return;
	}
	pre_cal += (base + dev->cur_prek_offset);

	memcpy(pre_cal, data->data, len);
	dev->cur_prek_offset += len;
}

void mt7915_tm_rf_test_event(struct mt7915_dev *dev, struct sk_buff *skb)
{
	struct mt7915_tm_rf_test_result *result;
	struct mt7915_tm_rf_test_data *data;
	static u32 event_type;

	result = (struct mt7915_tm_rf_test_result *)skb->data;
	data = (struct mt7915_tm_rf_test_data *)result->event;

	event_type = le32_to_cpu(result->func_idx);

	switch (event_type) {
	case RF_TEST_RE_CAL:
		mt7915_tm_re_cal_event(dev, result, data);
		break;
	default:
		break;
	}
}

static void
mt7915_tm_update_params(struct mt7915_phy *phy, u32 changed)
{
	struct mt76_testmode_data *td = &phy->mt76->test;
	bool en = phy->mt76->test.state != MT76_TM_STATE_OFF;

	if (changed & BIT(TM_CHANGED_FREQ_OFFSET))
		mt7915_tm_set_freq_offset(phy, en, en ? td->freq_offset : 0);
	if (changed & BIT(TM_CHANGED_TXPOWER))
		mt7915_tm_set_tx_power(phy);
	if (changed & BIT(TM_CHANGED_AID))
		mt7915_tm_set_entry(phy);
	if (changed & BIT(TM_CHANGED_CFG))
		mt7915_tm_set_cfg(phy);
	if (changed & BIT(TM_CHANGED_TXBF_ACT))
		mt7915_tm_set_txbf(phy);
}

static int
mt7915_tm_set_state(struct mt76_phy *mphy, enum mt76_testmode_state state)
{
	struct mt76_testmode_data *td = &mphy->test;
	struct mt7915_phy *phy = mphy->priv;
	enum mt76_testmode_state prev_state = td->state;

	mphy->test.state = state;

	if (prev_state == MT76_TM_STATE_TX_FRAMES ||
	    state == MT76_TM_STATE_TX_FRAMES)
		mt7915_tm_set_tx_frames(phy, state == MT76_TM_STATE_TX_FRAMES);
	else if (prev_state == MT76_TM_STATE_RX_FRAMES ||
		 state == MT76_TM_STATE_RX_FRAMES)
		mt7915_tm_set_rx_frames(phy, state == MT76_TM_STATE_RX_FRAMES);
	else if (prev_state == MT76_TM_STATE_TX_CONT ||
		 state == MT76_TM_STATE_TX_CONT)
		mt7915_tm_set_tx_cont(phy, state == MT76_TM_STATE_TX_CONT);
	else if (prev_state == MT76_TM_STATE_OFF ||
		 state == MT76_TM_STATE_OFF)
		mt7915_tm_init(phy, !(state == MT76_TM_STATE_OFF));
	else if (state >= MT76_TM_STATE_GROUP_PREK && state <= MT76_TM_STATE_GROUP_PREK_CLEAN)
		return mt7915_tm_group_prek(phy, state);
	else if (state >= MT76_TM_STATE_DPD_2G && state <= MT76_TM_STATE_DPD_CLEAN)
		return mt7915_tm_dpd_prek(phy, state);

	if ((state == MT76_TM_STATE_IDLE &&
	     prev_state == MT76_TM_STATE_OFF) ||
	    (state == MT76_TM_STATE_OFF &&
	     prev_state == MT76_TM_STATE_IDLE)) {
		u32 changed = 0;
		int i;

		for (i = 0; i < ARRAY_SIZE(tm_change_map); i++) {
			u16 cur = tm_change_map[i];

			if (td->param_set[cur / 32] & BIT(cur % 32))
				changed |= BIT(i);
		}

		mt7915_tm_update_params(phy, changed);
	}

	return 0;
}

static int
mt7915_tm_set_params(struct mt76_phy *mphy, struct nlattr **tb,
		     enum mt76_testmode_state new_state)
{
	struct mt76_testmode_data *td = &mphy->test;
	struct mt7915_phy *phy = mphy->priv;
	u32 changed = 0;
	int i;

	BUILD_BUG_ON(NUM_TM_CHANGED >= 32);

	if (new_state == MT76_TM_STATE_OFF ||
	    td->state == MT76_TM_STATE_OFF)
		return 0;

	if (td->tx_antenna_mask & ~mphy->chainmask)
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(tm_change_map); i++) {
		if (tb[tm_change_map[i]])
			changed |= BIT(i);
	}

	mt7915_tm_update_params(phy, changed);

	return 0;
}

static int
mt7915_tm_dump_stats(struct mt76_phy *mphy, struct sk_buff *msg)
{
	struct mt7915_phy *phy = mphy->priv;
	struct mt7915_dev *dev = phy->dev;
	void *rx, *rssi;
	int i;

	rx = nla_nest_start(msg, MT76_TM_STATS_ATTR_LAST_RX);
	if (!rx)
		return -ENOMEM;

	if (nla_put_s32(msg, MT76_TM_RX_ATTR_FREQ_OFFSET, phy->test.last_freq_offset))
		return -ENOMEM;

	rssi = nla_nest_start(msg, MT76_TM_RX_ATTR_RCPI);
	if (!rssi)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(phy->test.last_rcpi); i++)
		if (nla_put_u8(msg, i, phy->test.last_rcpi[i]))
			return -ENOMEM;

	nla_nest_end(msg, rssi);

	rssi = nla_nest_start(msg, MT76_TM_RX_ATTR_IB_RSSI);
	if (!rssi)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(phy->test.last_ib_rssi); i++)
		if (nla_put_s8(msg, i, phy->test.last_ib_rssi[i]))
			return -ENOMEM;

	nla_nest_end(msg, rssi);

	rssi = nla_nest_start(msg, MT76_TM_RX_ATTR_WB_RSSI);
	if (!rssi)
		return -ENOMEM;

	for (i = 0; i < ARRAY_SIZE(phy->test.last_wb_rssi); i++)
		if (nla_put_s8(msg, i, phy->test.last_wb_rssi[i]))
			return -ENOMEM;

	nla_nest_end(msg, rssi);

	if (nla_put_u8(msg, MT76_TM_RX_ATTR_SNR, phy->test.last_snr))
		return -ENOMEM;

	nla_nest_end(msg, rx);

	if (mphy->test.tx_rate_mode == MT76_TM_TX_MODE_HE_MU)
		mphy->test.tx_done += mt76_rr(dev, MT_MIB_DR8(phy != &dev->phy));

	return mt7915_tm_get_rx_stats(phy, false);
}

static int
mt7915_tm_write_back_to_efuse(struct mt7915_dev *dev)
{
	struct mt7915_mcu_eeprom_info req = {};
	u8 *eeprom = dev->mt76.eeprom.data;
	int i, ret = -EINVAL;

	/* prevent from damaging chip id in efuse */
	if (mt76_chip(&dev->mt76) != get_unaligned_le16(eeprom))
		goto out;

	for (i = 0; i < mt7915_eeprom_size(dev); i += MT76_TM_EEPROM_BLOCK_SIZE) {
		req.addr = cpu_to_le32(i);
		memcpy(&req.data, eeprom + i, MT76_TM_EEPROM_BLOCK_SIZE);

		ret = mt76_mcu_send_msg(&dev->mt76, MCU_EXT_CMD(EFUSE_ACCESS),
					&req, sizeof(req), true);
		if (ret)
			return ret;
	}

out:
	return ret;
}

static int
mt7915_tm_set_eeprom(struct mt76_phy *mphy, u32 offset, u8 *val, u8 action)
{
	struct mt7915_phy *phy = mphy->priv;
	struct mt7915_dev *dev = phy->dev;
	u8 *eeprom = dev->mt76.eeprom.data;
	int ret = 0;

	if (offset >= mt7915_eeprom_size(dev))
		return -EINVAL;

	switch (action) {
	case MT76_TM_EEPROM_ACTION_UPDATE_DATA:
		memcpy(eeprom + offset, val, MT76_TM_EEPROM_BLOCK_SIZE);
		break;
	case MT76_TM_EEPROM_ACTION_UPDATE_BUFFER_MODE:
		ret = mt7915_mcu_set_eeprom(dev, true);
		break;
	case MT76_TM_EEPROM_ACTION_WRITE_TO_EFUSE:
		ret = mt7915_tm_write_back_to_efuse(dev);
		break;
	default:
		break;
	}

	return ret;
}

static int
mt7915_tm_dump_precal(struct mt76_phy *mphy, struct sk_buff *msg, int flag, int type)
{
#define DPD_PER_CHAN_SIZE_MASK		GENMASK(31, 24)
#define DPD_CHAN_NUM_2G_MASK		GENMASK(23, 16)
#define DPD_CHAN_NUM_5G_MASK		GENMASK(15, 8)
#define DPD_CHAN_NUM_6G_MASK		GENMASK(7, 0)
	struct mt7915_phy *phy = mphy->priv;
	struct mt7915_dev *dev = phy->dev;
	u32 i, group_size, dpd_size, total_size, dpd_per_chan_size, dpd_info = 0;
	u32 base, size, total_chan_num, offs, transmit_size = 1000;
	u8 *pre_cal, *eeprom;
	void *precal;
	enum prek_ops {
		PREK_GET_INFO,
		PREK_SYNC_ALL,
		PREK_SYNC_GROUP,
		PREK_SYNC_DPD_2G,
		PREK_SYNC_DPD_5G,
		PREK_SYNC_DPD_6G,
		PREK_CLEAN_GROUP,
		PREK_CLEAN_DPD,
	};

	if (!dev->cal) {
		dev_info(dev->mt76.dev, "Not pre-cal yet!\n");
		return 0;
	}

	group_size = mt7915_get_cal_group_size(dev);
	dpd_size = is_mt7915(&dev->mt76) ? MT_EE_CAL_DPD_SIZE_V1 : MT_EE_CAL_DPD_SIZE_V2;
	dpd_per_chan_size = is_mt7915(&dev->mt76) ? 2 : 3;
	total_size = group_size + dpd_size;
	pre_cal = dev->cal;
	eeprom = dev->mt76.eeprom.data;
	offs = is_mt7915(&dev->mt76) ? MT_EE_DO_PRE_CAL : MT_EE_DO_PRE_CAL_V2;

	total_chan_num = dev->dpd_chan_num_2g + dev->dpd_chan_num_5g + dev->dpd_chan_num_6g;

	switch (type) {
	case PREK_SYNC_ALL:
		base = 0;
		size = total_size;
		break;
	case PREK_SYNC_GROUP:
		base = 0;
		size = group_size;
		break;
	case PREK_SYNC_DPD_6G:
		base = group_size;
		size = dpd_size * dev->dpd_chan_num_6g / total_chan_num;
		break;
	case PREK_SYNC_DPD_5G:
		base = group_size + dev->dpd_chan_num_6g * dpd_per_chan_size * MT_EE_CAL_UNIT;
		size = dpd_size * dev->dpd_chan_num_5g / total_chan_num;
		break;
	case PREK_SYNC_DPD_2G:
		base = group_size + (dev->dpd_chan_num_6g + dev->dpd_chan_num_5g) *
			   dpd_per_chan_size * MT_EE_CAL_UNIT;
		size = dpd_size * dev->dpd_chan_num_2g / total_chan_num;
		break;
	case PREK_GET_INFO:
		break;
	default:
		return 0;
	}

	if (!flag) {
		if (eeprom[offs] & MT_EE_WIFI_CAL_DPD) {
			dpd_info |= u32_encode_bits(dpd_per_chan_size, DPD_PER_CHAN_SIZE_MASK) |
				    u32_encode_bits(dev->dpd_chan_num_2g, DPD_CHAN_NUM_2G_MASK) |
				    u32_encode_bits(dev->dpd_chan_num_5g, DPD_CHAN_NUM_5G_MASK) |
				    u32_encode_bits(dev->dpd_chan_num_6g, DPD_CHAN_NUM_6G_MASK);
		}
		dev->cur_prek_offset = 0;
		precal = nla_nest_start(msg, MT76_TM_ATTR_PRECAL_INFO);
		if (!precal)
			return -ENOMEM;
		nla_put_u32(msg, 0, group_size);
		nla_put_u32(msg, 1, dpd_size);
		nla_put_u32(msg, 2, dpd_info);
		nla_put_u32(msg, 3, transmit_size);
		nla_put_u32(msg, 4, eeprom[offs]);
		nla_nest_end(msg, precal);
	} else {
		precal = nla_nest_start(msg, MT76_TM_ATTR_PRECAL);
		if (!precal)
			return -ENOMEM;

		transmit_size = (dev->cur_prek_offset + transmit_size < size) ?
				transmit_size : (size - dev->cur_prek_offset);
		for (i = 0; i < transmit_size; i++) {
			if (nla_put_u8(msg, i, pre_cal[base + dev->cur_prek_offset + i]))
				return -ENOMEM;
		}
		dev->cur_prek_offset += transmit_size;

		nla_nest_end(msg, precal);
	}

	return 0;
}

const struct mt76_testmode_ops mt7915_testmode_ops = {
	.set_state = mt7915_tm_set_state,
	.set_params = mt7915_tm_set_params,
	.dump_stats = mt7915_tm_dump_stats,
	.set_eeprom = mt7915_tm_set_eeprom,
	.dump_precal = mt7915_tm_dump_precal,
};
