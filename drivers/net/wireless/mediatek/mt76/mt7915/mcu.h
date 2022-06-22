/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2020 MediaTek Inc. */

#ifndef __MT7915_MCU_H
#define __MT7915_MCU_H

#include "../mt76_connac_mcu.h"

enum {
	MCU_ATE_SET_TRX = 0x1,
	MCU_ATE_SET_FREQ_OFFSET = 0xa,
	MCU_ATE_SET_PHY_COUNT = 0x11,
	MCU_ATE_SET_SLOT_TIME = 0x13,
	MCU_ATE_CLEAN_TXQUEUE = 0x1c,
};

struct mt7915_mcu_thermal_ctrl {
	u8 ctrl_id;
	u8 band_idx;
	union {
		struct {
			u8 protect_type; /* 1: duty admit, 2: radio off */
			u8 trigger_type; /* 0: low, 1: high */
		} __packed type;
		struct {
			u8 duty_level;	/* level 0~3 */
			u8 duty_cycle;
		} __packed duty;
	};
} __packed;

struct mt7915_mcu_thermal_notify {
	struct mt76_connac2_mcu_rxd rxd;

	struct mt7915_mcu_thermal_ctrl ctrl;
	__le32 temperature;
	u8 rsv[8];
} __packed;

struct mt7915_mcu_csa_notify {
	struct mt76_connac2_mcu_rxd rxd;

	u8 omac_idx;
	u8 csa_count;
	u8 band_idx;
	u8 rsv;
} __packed;

struct mt7915_mcu_bcc_notify {
	struct mt76_connac2_mcu_rxd rxd;

	u8 band_idx;
	u8 omac_idx;
	u8 cca_count;
	u8 rsv;
} __packed;

struct mt7915_mcu_rdd_report {
	struct mt76_connac2_mcu_rxd rxd;

	u8 band_idx;
	u8 long_detected;
	u8 constant_prf_detected;
	u8 staggered_prf_detected;
	u8 radar_type_idx;
	u8 periodic_pulse_num;
	u8 long_pulse_num;
	u8 hw_pulse_num;

	u8 out_lpn;
	u8 out_spn;
	u8 out_crpn;
	u8 out_crpw;
	u8 out_crbn;
	u8 out_stgpn;
	u8 out_stgpw;

	u8 rsv;

	__le32 out_pri_const;
	__le32 out_pri_stg[3];

	struct {
		__le32 start;
		__le16 pulse_width;
		__le16 pulse_power;
		u8 mdrdy_flag;
		u8 rsv[3];
	} long_pulse[32];

	struct {
		__le32 start;
		__le16 pulse_width;
		__le16 pulse_power;
		u8 mdrdy_flag;
		u8 rsv[3];
	} periodic_pulse[32];

	struct {
		__le32 start;
		__le16 pulse_width;
		__le16 pulse_power;
		u8 sc_pass;
		u8 sw_reset;
		u8 mdrdy_flag;
		u8 tx_active;
	} hw_pulse[32];
} __packed;

struct mt7915_mcu_background_chain_ctrl {
	u8 chan;		/* primary channel */
	u8 central_chan;	/* central channel */
	u8 bw;
	u8 tx_stream;
	u8 rx_stream;

	u8 monitor_chan;	/* monitor channel */
	u8 monitor_central_chan;/* monitor central channel */
	u8 monitor_bw;
	u8 monitor_tx_stream;
	u8 monitor_rx_stream;

	u8 scan_mode;		/* 0: ScanStop
				 * 1: ScanStart
				 * 2: ScanRunning
				 */
	u8 band_idx;		/* DBDC */
	u8 monitor_scan_type;
	u8 band;		/* 0: 2.4GHz, 1: 5GHz */
	u8 rsv[2];
} __packed;

struct mt7915_mcu_eeprom {
	u8 buffer_mode;
	u8 format;
	__le16 len;
} __packed;

struct mt7915_mcu_eeprom_info {
	__le32 addr;
	__le32 valid;
	u8 data[16];
} __packed;

struct mt7915_mcu_phy_rx_info {
	u8 category;
	u8 rate;
	u8 mode;
	u8 nsts;
	u8 gi;
	u8 coding;
	u8 stbc;
	u8 bw;
};

struct mt7915_mcu_mib {
	__le32 band;
	__le32 offs;
	__le64 data;
} __packed;

enum mt7915_chan_mib_offs {
	/* mt7915 */
	MIB_BUSY_TIME = 14,
	MIB_TX_TIME = 81,
	MIB_RX_TIME,
	MIB_OBSS_AIRTIME = 86,
	/* mt7916 */
	MIB_BUSY_TIME_V2 = 0,
	MIB_TX_TIME_V2 = 6,
	MIB_RX_TIME_V2 = 8,
	MIB_OBSS_AIRTIME_V2 = 490
};

struct edca {
	u8 queue;
	u8 set;
	u8 aifs;
	u8 cw_min;
	__le16 cw_max;
	__le16 txop;
};

struct mt7915_mcu_tx {
	u8 total;
	u8 action;
	u8 valid;
	u8 mode;

	struct edca edca[IEEE80211_NUM_ACS];
} __packed;

struct mt7915_mcu_muru_stats {
	__le32 event_id;
	struct {
		__le32 cck_cnt;
		__le32 ofdm_cnt;
		__le32 htmix_cnt;
		__le32 htgf_cnt;
		__le32 vht_su_cnt;
		__le32 vht_2mu_cnt;
		__le32 vht_3mu_cnt;
		__le32 vht_4mu_cnt;
		__le32 he_su_cnt;
		__le32 he_ext_su_cnt;
		__le32 he_2ru_cnt;
		__le32 he_2mu_cnt;
		__le32 he_3ru_cnt;
		__le32 he_3mu_cnt;
		__le32 he_4ru_cnt;
		__le32 he_4mu_cnt;
		__le32 he_5to8ru_cnt;
		__le32 he_9to16ru_cnt;
		__le32 he_gtr16ru_cnt;
	} dl;

	struct {
		__le32 hetrig_su_cnt;
		__le32 hetrig_2ru_cnt;
		__le32 hetrig_3ru_cnt;
		__le32 hetrig_4ru_cnt;
		__le32 hetrig_5to8ru_cnt;
		__le32 hetrig_9to16ru_cnt;
		__le32 hetrig_gtr16ru_cnt;
		__le32 hetrig_2mu_cnt;
		__le32 hetrig_3mu_cnt;
		__le32 hetrig_4mu_cnt;
	} ul;
};

#define WMM_AIFS_SET		BIT(0)
#define WMM_CW_MIN_SET		BIT(1)
#define WMM_CW_MAX_SET		BIT(2)
#define WMM_TXOP_SET		BIT(3)
#define WMM_PARAM_SET		GENMASK(3, 0)

enum {
	MCU_FW_LOG_WM,
	MCU_FW_LOG_WA,
	MCU_FW_LOG_TO_HOST,
};

enum {
	MCU_TWT_AGRT_ADD,
	MCU_TWT_AGRT_MODIFY,
	MCU_TWT_AGRT_DELETE,
	MCU_TWT_AGRT_TEARDOWN,
	MCU_TWT_AGRT_GET_TSF,
};

enum {
	MCU_WA_PARAM_CMD_QUERY,
	MCU_WA_PARAM_CMD_SET,
	MCU_WA_PARAM_CMD_CAPABILITY,
	MCU_WA_PARAM_CMD_DEBUG,
};

enum {
	MCU_WA_PARAM_PDMA_RX = 0x04,
	MCU_WA_PARAM_CPU_UTIL = 0x0b,
	MCU_WA_PARAM_RED = 0x0e,
#ifdef MTK_DEBUG
	MCU_WA_PARAM_RED_SHOW_STA = 0xf,
	MCU_WA_PARAM_RED_TARGET_DELAY = 0x10,
#endif
};

enum mcu_mmps_mode {
	MCU_MMPS_STATIC,
	MCU_MMPS_DYNAMIC,
	MCU_MMPS_RSV,
	MCU_MMPS_DISABLE,
};

struct bss_info_bmc_rate {
	__le16 tag;
	__le16 len;
	__le16 bc_trans;
	__le16 mc_trans;
	u8 short_preamble;
	u8 rsv[7];
} __packed;

struct bss_info_ra {
	__le16 tag;
	__le16 len;
	u8 op_mode;
	u8 adhoc_en;
	u8 short_preamble;
	u8 tx_streams;
	u8 rx_streams;
	u8 algo;
	u8 force_sgi;
	u8 force_gf;
	u8 ht_mode;
	u8 has_20_sta;		/* Check if any sta support GF. */
	u8 bss_width_trigger_events;
	u8 vht_nss_cap;
	u8 vht_bw_signal;	/* not use */
	u8 vht_force_sgi;	/* not use */
	u8 se_off;
	u8 antenna_idx;
	u8 train_up_rule;
	u8 rsv[3];
	unsigned short train_up_high_thres;
	short train_up_rule_rssi;
	unsigned short low_traffic_thres;
	__le16 max_phyrate;
	__le32 phy_cap;
	__le32 interval;
	__le32 fast_interval;
} __packed;

struct bss_info_hw_amsdu {
	__le16 tag;
	__le16 len;
	__le32 cmp_bitmap_0;
	__le32 cmp_bitmap_1;
	__le16 trig_thres;
	u8 enable;
	u8 rsv;
} __packed;

struct bss_info_color {
	__le16 tag;
	__le16 len;
	u8 disable;
	u8 color;
	u8 rsv[2];
} __packed;

struct bss_info_he {
	__le16 tag;
	__le16 len;
	u8 he_pe_duration;
	u8 vht_op_info_present;
	__le16 he_rts_thres;
	__le16 max_nss_mcs[CMD_HE_MCS_BW_NUM];
	u8 rsv[6];
} __packed;

struct bss_info_bcn {
	__le16 tag;
	__le16 len;
	u8 ver;
	u8 enable;
	__le16 sub_ntlv;
} __packed __aligned(4);

struct bss_info_bcn_cntdwn {
	__le16 tag;
	__le16 len;
	u8 cnt;
	u8 rsv[3];
} __packed __aligned(4);

struct bss_info_bcn_mbss {
#define MAX_BEACON_NUM	32
	__le16 tag;
	__le16 len;
	__le32 bitmap;
	__le16 offset[MAX_BEACON_NUM];
	u8 rsv[8];
} __packed __aligned(4);

struct bss_info_bcn_cont {
	__le16 tag;
	__le16 len;
	__le16 tim_ofs;
	__le16 csa_ofs;
	__le16 bcc_ofs;
	__le16 pkt_len;
} __packed __aligned(4);

struct bss_info_inband_discovery {
	__le16 tag;
	__le16 len;
	u8 tx_type;
	u8 tx_mode;
	u8 tx_interval;
	u8 enable;
	__le16 rsv;
	__le16 prob_rsp_len;
} __packed __aligned(4);

enum {
	BSS_INFO_BCN_CSA,
	BSS_INFO_BCN_BCC,
	BSS_INFO_BCN_MBSSID,
	BSS_INFO_BCN_CONTENT,
	BSS_INFO_BCN_DISCOV,
	BSS_INFO_BCN_MAX
};

enum {
	RATE_PARAM_FIXED = 3,
	RATE_PARAM_MMPS_UPDATE = 5,
	RATE_PARAM_FIXED_HE_LTF = 7,
	RATE_PARAM_FIXED_MCS = 8,
	RATE_PARAM_FIXED_GI = 11,
	RATE_PARAM_AUTO = 20,
	RATE_PARAM_SPE_UPDATE = 22,
#ifdef CONFIG_MTK_VENDOR
	RATE_PARAM_FIXED_MIMO = 30,
	RATE_PARAM_FIXED_OFDMA = 31,
	RATE_PARAM_AUTO_HEMU = 32,
#endif
};

#define RATE_CFG_MCS			GENMASK(3, 0)
#define RATE_CFG_NSS			GENMASK(7, 4)
#define RATE_CFG_GI			GENMASK(11, 8)
#define RATE_CFG_BW			GENMASK(15, 12)
#define RATE_CFG_STBC			GENMASK(19, 16)
#define RATE_CFG_LDPC			GENMASK(23, 20)
#define RATE_CFG_PHY_TYPE		GENMASK(27, 24)
#define RATE_CFG_HE_LTF			GENMASK(31, 28)

#define RATE_CFG_MODE			GENMASK(15, 8)
#define RATE_CFG_VAL			GENMASK(7, 0)

enum {
	THERMAL_PROTECT_PARAMETER_CTRL,
	THERMAL_PROTECT_BASIC_INFO,
	THERMAL_PROTECT_ENABLE,
	THERMAL_PROTECT_DISABLE,
	THERMAL_PROTECT_DUTY_CONFIG,
	THERMAL_PROTECT_MECH_INFO,
	THERMAL_PROTECT_DUTY_INFO,
	THERMAL_PROTECT_STATE_ACT,
};

enum {
	MT_BF_SOUNDING_ON = 1,
	MT_BF_TYPE_UPDATE = 20,
	MT_BF_MODULE_UPDATE = 25
};

enum {
	MURU_SET_ARB_OP_MODE = 14,
	MURU_SET_PLATFORM_TYPE = 25,
};

enum {
	MURU_PLATFORM_TYPE_PERF_LEVEL_1 = 1,
	MURU_PLATFORM_TYPE_PERF_LEVEL_2,
};

enum mt_debug {
	DEBUG_TXCMD = 62,
	DEBUG_CMD_RPT_TX,
	DEBUG_CMD_RPT_TRIG,
	DEBUG_SPL,
	DEBUG_RPT_RX,
};

/* tx cmd tx statistics */
enum {
	MURU_SET_TXC_TX_STATS_EN = 150,
	MURU_GET_TXC_TX_STATS = 151,
};

enum {
	SER_QUERY,
	SER_SET,
	/* recovery */
	SER_SET_RECOVER_L1 = 1,
	SER_SET_RECOVER_L2,
	SER_SET_RECOVER_L3_RX_ABORT,
	SER_SET_RECOVER_L3_TX_ABORT,
	SER_SET_RECOVER_L3_TX_DISABLE,
	SER_SET_RECOVER_L3_BF,
	/* action */
	SER_ENABLE = 2,
	SER_RECOVER
};

#define MT7915_MAX_BEACON_SIZE		512
#define MT7915_MAX_INBAND_FRAME_SIZE	256
#define MT7915_MAX_BSS_OFFLOAD_SIZE	(MT7915_MAX_BEACON_SIZE +	  \
					 MT7915_MAX_INBAND_FRAME_SIZE +	  \
					 MT7915_BEACON_UPDATE_SIZE)

#define MT7915_BSS_UPDATE_MAX_SIZE	(sizeof(struct sta_req_hdr) +	\
					 sizeof(struct bss_info_omac) +	\
					 sizeof(struct bss_info_basic) +\
					 sizeof(struct bss_info_rf_ch) +\
					 sizeof(struct bss_info_ra) +	\
					 sizeof(struct bss_info_hw_amsdu) +\
					 sizeof(struct bss_info_he) +	\
					 sizeof(struct bss_info_bmc_rate) +\
					 sizeof(struct bss_info_ext_bss))

#define MT7915_BEACON_UPDATE_SIZE	(sizeof(struct sta_req_hdr) +	\
					 sizeof(struct bss_info_bcn_cntdwn) + \
					 sizeof(struct bss_info_bcn_mbss) + \
					 sizeof(struct bss_info_bcn_cont) + \
					 sizeof(struct bss_info_inband_discovery))

#ifdef CONFIG_MTK_VENDOR
struct mt7915_mcu_csi {
	u8 band;
	u8 mode;
	u8 cfg;
	u8 v1;
	__le32 v2;
	u8 mac_addr[ETH_ALEN];
	u8 _rsv[34];
} __packed;

struct csi_tlv {
	__le32 tag;
	__le32 len;
} __packed;

#define CSI_MAX_COUNT	256
#define CSI_MAX_BUF_NUM	3000

struct mt7915_mcu_csi_report {
	struct csi_tlv _t0;
	__le32 ver;
	struct csi_tlv _t1;
	__le32 ch_bw;
	struct csi_tlv _t2;
	__le32 rssi;
	struct csi_tlv _t3;
	__le32 snr;
	struct csi_tlv _t4;
	__le32 band;
	struct csi_tlv _t5;
	__le32 data_num;
	struct csi_tlv _t6;
	__le16 data_i[CSI_MAX_COUNT];
	struct csi_tlv _t7;
	__le16 data_q[CSI_MAX_COUNT];
	struct csi_tlv _t8;
	__le32 data_bw;
	struct csi_tlv _t9;
	__le32 pri_ch_idx;
	struct csi_tlv _t10;
	u8 ta[8];
	struct csi_tlv _t11;
	__le32 info;
	struct csi_tlv _t12;
	__le32 rx_mode;
	struct csi_tlv _t17;
	__le32 h_idx;
	struct csi_tlv _t18;
	__le32 trx_idx;
	struct csi_tlv _t19;
	__le32 ts;
} __packed;

struct csi_data {
	u8 ch_bw;
	u16 data_num;
	s16 data_i[CSI_MAX_COUNT];
	s16 data_q[CSI_MAX_COUNT];
	u8 band;
	s8 rssi;
	u8 snr;
	u32 ts;
	u8 data_bw;
	u8 pri_ch_idx;
	u8 ta[ETH_ALEN];
	u32 info;
	u8 rx_mode;
	u32 h_idx;
	u16 tx_idx;
	u16 rx_idx;

	struct list_head node;
};
#endif

/* MURU */
#define OFDMA_DL                       BIT(0)
#define OFDMA_UL                       BIT(1)
#define MUMIMO_DL                      BIT(2)
#define MUMIMO_UL                      BIT(3)
#define MUMIMO_DL_CERT                 BIT(4)

#ifdef CONFIG_MTK_VENDOR
struct mt7915_muru_comm {
   u8 ppdu_format;
   u8 sch_type;
   u8 band;
   u8 wmm_idx;
   u8 spe_idx;
   u8 proc_type;
};

struct mt7915_muru_dl {
   u8 user_num;
   u8 tx_mode;
   u8 bw;
   u8 gi;
   u8 ltf;
   /* sigB */
   u8 mcs;
   u8 dcm;
   u8 cmprs;

   u8 ru[8];
   u8 c26[2];
   u8 ack_policy;

   struct {
	   __le16 wlan_idx;
       u8 ru_alloc_seg;
       u8 ru_idx;
       u8 ldpc;
       u8 nss;
       u8 mcs;
       u8 mu_group_idx;
       u8 vht_groud_id;
       u8 vht_up;
       u8 he_start_stream;
       u8 he_mu_spatial;
       u8 ack_policy;
       __le16 tx_power_alpha;
   } usr[16];
};

struct mt7915_muru_ul {
   u8 user_num;

   /* UL TX */
   u8 trig_type;
   __le16 trig_cnt;
   __le16 trig_intv;
   u8 bw;
   u8 gi_ltf;
   __le16 ul_len;
   u8 pad;
   u8 trig_ta[ETH_ALEN];
   u8 ru[8];
   u8 c26[2];

   struct {
       __le16 wlan_idx;
       u8 ru_alloc;
       u8 ru_idx;
       u8 ldpc;
       u8 nss;
       u8 mcs;
       u8 target_rssi;
       __le32 trig_pkt_size;
   } usr[16];

   /* HE TB RX Debug */
   __le32 rx_hetb_nonsf_en_bitmap;
   __le32 rx_hetb_cfg[2];

   /* DL TX */
   u8 ba_type;
};

struct mt7915_muru {
   __le32 cfg_comm;
   __le32 cfg_dl;
   __le32 cfg_ul;

   struct mt7915_muru_comm comm;
   struct mt7915_muru_dl dl;
   struct mt7915_muru_ul ul;
};

#define MURU_PPDU_HE_TRIG      BIT(2)
#define MURU_PPDU_HE_MU                 BIT(3)

#define MURU_OFDMA_SCH_TYPE_DL          BIT(0)
#define MURU_OFDMA_SCH_TYPE_UL          BIT(1)

/* Common Config */
#define MURU_COMM_PPDU_FMT              BIT(0)
#define MURU_COMM_SCH_TYPE              BIT(1)
#define MURU_COMM_SET                   (MURU_COMM_PPDU_FMT | MURU_COMM_SCH_TYPE)
/* DL&UL User config*/
#define MURU_USER_CNT                   BIT(4)

enum {
   CAPI_SU,
   CAPI_MU,
   CAPI_ER_SU,
   CAPI_TB,
   CAPI_LEGACY
};

enum {
   CAPI_BASIC,
   CAPI_BRP,
   CAPI_MU_BAR,
   CAPI_MU_RTS,
   CAPI_BSRP,
   CAPI_GCR_MU_BAR,
   CAPI_BQRP,
   CAPI_NDP_FRP
};

enum {
   MURU_SET_BSRP_CTRL = 1,
   MURU_SET_SUTX = 16,
   MURU_SET_MUMIMO_CTRL = 17,
   MURU_SET_MANUAL_CFG = 100,
   MURU_SET_MU_DL_ACK_POLICY = 200,
   MURU_SET_TRIG_TYPE = 201,
   MURU_SET_20M_DYN_ALGO = 202,
   MURU_SET_PROT_FRAME_THR = 204,
   MURU_SET_CERT_MU_EDCA_OVERRIDE = 205,
};

enum {
   MU_DL_ACK_POLICY_MU_BAR = 3,
   MU_DL_ACK_POLICY_TF_FOR_ACK = 4,
  MU_DL_ACK_POLICY_SU_BAR = 5,
};

enum {
   BF_SOUNDING_OFF = 0,
   BF_SOUNDING_ON,
   BF_DATA_PACKET_APPLY,
   BF_PFMU_MEM_ALLOCATE,
   BF_PFMU_MEM_RELEASE,
   BF_PFMU_TAG_READ,
   BF_PFMU_TAG_WRITE,
   BF_PROFILE_READ,
   BF_PROFILE_WRITE,
   BF_PN_READ,
   BF_PN_WRITE,
   BF_PFMU_MEM_ALLOC_MAP_READ,
   BF_AID_SET,
   BF_STA_REC_READ,
   BF_PHASE_CALIBRATION,
   BF_IBF_PHASE_COMP,
   BF_LNA_GAIN_CONFIG,
   BF_PROFILE_WRITE_20M_ALL,
   BF_APCLIENT_CLUSTER,
   BF_AWARE_CTRL,
   BF_HW_ENABLE_STATUS_UPDATE,
   BF_REPT_CLONED_STA_TO_NORMAL_STA,
   BF_GET_QD,
   BF_BFEE_HW_CTRL,
   BF_PFMU_SW_TAG_WRITE,
   BF_MOD_EN_CTRL,
   BF_DYNSND_EN_INTR,
   BF_DYNSND_CFG_DMCS_TH,
   BF_DYNSND_EN_PFID_INTR,
   BF_CONFIG,
   BF_PFMU_DATA_WRITE,
   BF_FBRPT_DBG_INFO_READ,
   BF_CMD_TXSND_INFO,
   BF_CMD_PLY_INFO,
   BF_CMD_MU_METRIC,
   BF_CMD_TXCMD,
   BF_CMD_CFG_PHY,
   BF_CMD_SND_CNT,
   BF_CMD_MAX
};

enum {
   BF_SND_READ_INFO = 0,
   BF_SND_CFG_OPT,
   BF_SND_CFG_INTV,
   BF_SND_STA_STOP,
   BF_SND_CFG_MAX_STA,
   BF_SND_CFG_BFRP,
   BF_SND_CFG_INF
};

enum {
   MURU_UPDATE = 0,
   MURU_DL_USER_CNT,
   MURU_UL_USER_CNT,
   MURU_DL_INIT,
   MURU_UL_INIT,
};
#endif

#endif
