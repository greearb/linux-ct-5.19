/* SPDX-License-Identifier: ISC */
/* Copyright (C) 2020 MediaTek Inc. */

#ifndef __MT7915_TESTMODE_H
#define __MT7915_TESTMODE_H

#include "mcu.h"

struct mt7915_tm_trx {
	u8 type;
	u8 enable;
	u8 band;
	u8 rsv;
};

struct mt7915_tm_freq_offset {
	u8 band;
	__le32 freq_offset;
};

struct mt7915_tm_slot_time {
	u8 slot_time;
	u8 sifs;
	u8 rifs;
	u8 _rsv;
	__le16 eifs;
	u8 band;
	u8 _rsv1[5];
};

struct mt7915_tm_clean_txq {
	bool sta_pause;
	u8 wcid;	/* 256 sta */
	u8 band;
	u8 rsv;
};

struct mt7915_tm_cfg {
	u8 enable;
	u8 band;
	u8 _rsv[2];
};

struct mt7915_tm_mu_rx_aid {
	__le32 band;
	__le16 aid;
};

struct mt7915_tm_cmd {
	u8 testmode_en;
	u8 param_idx;
	u8 _rsv[2];
	union {
		__le32 data;
		struct mt7915_tm_trx trx;
		struct mt7915_tm_freq_offset freq;
		struct mt7915_tm_slot_time slot;
		struct mt7915_tm_clean_txq clean;
		struct mt7915_tm_cfg cfg;
		struct mt7915_tm_mu_rx_aid rx_aid;
		u8 test[72];
	} param;
} __packed;

enum {
	TM_MAC_TX = 1,
	TM_MAC_RX,
	TM_MAC_TXRX,
	TM_MAC_TXRX_RXV,
	TM_MAC_RXV,
	TM_MAC_RX_RXV,
};

struct tm_tx_cont {
	u8 control_ch;
	u8 center_ch;
	u8 bw;
	u8 tx_ant;
	__le16 rateval;
	u8 band;
	u8 txfd_mode;
};

struct mt7915_tm_rf_test {
	u8 action;
	u8 icap_len;
	u8 _rsv[2];
	union {
		__le32 op_mode;
		__le32 freq;

		struct {
			__le32 func_idx;
			union {
				__le32 func_data;
				__le32 cal_dump;

				struct tm_tx_cont tx_cont;

				u8 _pad[80];
			} param;
		} rf;
	} op;
} __packed;

enum {
	RF_OPER_NORMAL,
	RF_OPER_RF_TEST,
	RF_OPER_ICAP,
	RF_OPER_ICAP_OVERLAP,
	RF_OPER_WIFI_SPECTRUM,
};

enum {
	TAM_ARB_OP_MODE_NORMAL = 1,
	TAM_ARB_OP_MODE_TEST,
	TAM_ARB_OP_MODE_FORCE_SU = 5,
};

enum {
	TM_CBW_20MHZ,
	TM_CBW_40MHZ,
	TM_CBW_80MHZ,
	TM_CBW_10MHZ,
	TM_CBW_5MHZ,
	TM_CBW_160MHZ,
	TM_CBW_8080MHZ,
};

struct mt7915_tm_rx_stat_band {
	u8 category;

	/* mac */
	__le16 fcs_err;
	__le16 len_mismatch;
	__le16 fcs_succ;
	__le32 mdrdy_cnt;
	/* phy */
	__le16 fcs_err_cck;
	__le16 fcs_err_ofdm;
	__le16 pd_cck;
	__le16 pd_ofdm;
	__le16 sig_err_cck;
	__le16 sfd_err_cck;
	__le16 sig_err_ofdm;
	__le16 tag_err_ofdm;
	__le16 mdrdy_cnt_cck;
	__le16 mdrdy_cnt_ofdm;
};

struct mt7915_tm_muru_comm {
	u8 ppdu_format;
	u8 sch_type;
	u8 band;
	u8 wmm_idx;
	u8 spe_idx;
	u8 proc_type;
};

struct mt7915_tm_muru_dl_usr {
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
};

struct mt7915_tm_muru_dl {
	u8 user_num;
	u8 tx_mode;
	u8 bw;
	u8 gi;
	u8 ltf;
	/* sigB */
	u8 mcs;
	u8 dcm;
	u8 cmprs;

	u8 tx_power;
	u8 ru[8];
	u8 c26[2];
	u8 ack_policy;

	struct mt7915_tm_muru_dl_usr usr[16];
};

struct mt7915_tm_muru_ul_usr {
	__le16 wlan_idx;
	u8 ru_alloc;
	u8 ru_idx;
	u8 ldpc;
	u8 nss;
	u8 mcs;
	u8 target_rssi;
	__le32 trig_pkt_size;
};

struct mt7915_tm_muru_ul {
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

	struct mt7915_tm_muru_ul_usr usr[16];
	/* HE TB RX Debug */
	__le32 rx_hetb_nonsf_en_bitmap;
	__le32 rx_hetb_cfg[2];

	/* DL TX */
	u8 ba_type;
};

struct mt7915_tm_muru {
	__le32 cfg_comm;
	__le32 cfg_dl;
	__le32 cfg_ul;

	struct mt7915_tm_muru_comm comm;
	struct mt7915_tm_muru_dl dl;
	struct mt7915_tm_muru_ul ul;
};

#define MURU_PPDU_HE_MU		BIT(3)

/* Common Config */
/* #define MURU_COMM_PPDU_FMT		BIT(0) */
/* #define MURU_COMM_SCH_TYPE		BIT(1) */
/* #define MURU_COMM_BAND			BIT(2) */
/* #define MURU_COMM_WMM			BIT(3) */
/* #define MURU_COMM_SPE_IDX		BIT(4) */
/* #define MURU_COMM_PROC_TYPE		BIT(5) */
/* #define MURU_COMM_SET		(MURU_COMM_PPDU_FMT | MURU_COMM_BAND | \ */
/* 				 MURU_COMM_WMM | MURU_COMM_SPE_IDX) */
/* DL Config */
#define MURU_DL_BW			BIT(0)
#define MURU_DL_GI			BIT(1)
#define MURU_DL_TX_MODE			BIT(2)
#define MURU_DL_TONE_PLAN		BIT(3)
#define MURU_DL_USER_CNT		BIT(4)
#define MURU_DL_LTF			BIT(5)
#define MURU_DL_SIGB_MCS		BIT(6)
#define MURU_DL_SIGB_DCM		BIT(7)
#define MURU_DL_SIGB_CMPRS		BIT(8)
#define MURU_DL_ACK_POLICY		BIT(9)
#define MURU_DL_TXPOWER			BIT(10)
/* DL Per User Config */
#define MURU_DL_USER_WLAN_ID		BIT(16)
#define MURU_DL_USER_COD		BIT(17)
#define MURU_DL_USER_MCS		BIT(18)
#define MURU_DL_USER_NSS		BIT(19)
#define MURU_DL_USER_RU_ALLOC		BIT(20)
#define MURU_DL_USER_MUMIMO_GRP		BIT(21)
#define MURU_DL_USER_MUMIMO_VHT		BIT(22)
#define MURU_DL_USER_ACK_POLICY		BIT(23)
#define MURU_DL_USER_MUMIMO_HE		BIT(24)
#define MURU_DL_USER_PWR_ALPHA		BIT(25)
#define MURU_DL_SET		(GENMASK(7, 0) | GENMASK(20, 16) | BIT(25))

#define MAX_PHASE_GROUP_NUM	9

struct mt7915_tm_txbf_phase {
	u8 status;
	struct {
		u8 r0_uh;
		u8 r0_h;
		u8 r0_m;
		u8 r0_l;
		u8 r0_ul;
		u8 r1_uh;
		u8 r1_h;
		u8 r1_m;
		u8 r1_l;
		u8 r1_ul;
		u8 r2_uh;
		u8 r2_h;
		u8 r2_m;
		u8 r2_l;
		u8 r2_ul;
		u8 r3_uh;
		u8 r3_h;
		u8 r3_m;
		u8 r3_l;
		u8 r3_ul;
		u8 r2_uh_sx2;
		u8 r2_h_sx2;
		u8 r2_m_sx2;
		u8 r2_l_sx2;
		u8 r2_ul_sx2;
		u8 r3_uh_sx2;
		u8 r3_h_sx2;
		u8 r3_m_sx2;
		u8 r3_l_sx2;
		u8 r3_ul_sx2;
		u8 m_t0_h;
		u8 m_t1_h;
		u8 m_t2_h;
		u8 m_t2_h_sx2;
		u8 r0_reserved;
		u8 r1_reserved;
		u8 r2_reserved;
		u8 r3_reserved;
		u8 r2_sx2_reserved;
		u8 r3_sx2_reserved;
	} phase;
};

struct mt7915_tm_pfmu_tag1 {
	__le32 pfmu_idx:10;
	__le32 ebf:1;
	__le32 data_bw:2;
	__le32 lm:2;
	__le32 is_mu:1;
	__le32 nr:3, nc:3;
	__le32 codebook:2;
	__le32 ngroup:2;
	__le32 _rsv:2;
	__le32 invalid_prof:1;
	__le32 rmsd:3;

	__le32 col_id1:6, row_id1:10;
	__le32 col_id2:6, row_id2:10;
	__le32 col_id3:6, row_id3:10;
	__le32 col_id4:6, row_id4:10;

	__le32 ru_start_id:7;
	__le32 _rsv1:1;
	__le32 ru_end_id:7;
	__le32 _rsv2:1;
	__le32 mob_cal_en:1;
	__le32 _rsv3:15;

	__le32 snr_sts0:8, snr_sts1:8, snr_sts2:8, snr_sts3:8;
	__le32 snr_sts4:8, snr_sts5:8, snr_sts6:8, snr_sts7:8;

	__le32 _rsv4;
} __packed;

struct mt7915_tm_pfmu_tag2 {
	__le32 smart_ant:24;
	__le32 se_idx:5;
	__le32 _rsv:3;

	__le32 _rsv1:8;
	__le32 rmsd_thres:3;
	__le32 _rsv2:5;
	__le32 ibf_timeout:8;
	__le32 _rsv3:8;

	__le32 _rsv4:16;
	__le32 ibf_data_bw:2;
	__le32 ibf_nc:3;
	__le32 ibf_nr:3;
	__le32 ibf_ru:8;

	__le32 mob_delta_t:8;
	__le32 mob_lq_result:7;
	__le32 _rsv5:1;
	__le32 _rsv6:16;

	__le32 _rsv7;
} __packed;

struct mt7915_tm_pfmu_tag {
	struct mt7915_tm_pfmu_tag1 t1;
	struct mt7915_tm_pfmu_tag2 t2;
};

struct mt7915_tm_pfmu_data {
	__le16 subc_idx;
	__le16 phi11;
	__le16 phi21;
	__le16 phi31;
};

struct mt7915_tm_ibf_cal_info {
	u8 format_id;
	u8 group_l_m_n;
	u8 group;
	bool sx2;
	u8 status;
	u8 cal_type;
	u8 _rsv[2];
	u8 buf[1000];
} __packed;

enum {
	IBF_PHASE_CAL_UNSPEC,
	IBF_PHASE_CAL_NORMAL,
	IBF_PHASE_CAL_VERIFY,
	IBF_PHASE_CAL_NORMAL_INSTRUMENT,
	IBF_PHASE_CAL_VERIFY_INSTRUMENT,
};

#endif
