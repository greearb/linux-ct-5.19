#include <linux/firmware.h>
#include <linux/fs.h>
#include<linux/inet.h>
#include "mt7915.h"
#include "mcu.h"
#include "mac.h"

int mt7915_mcu_set_txpower_level(struct mt7915_phy *phy, u8 drop_level)
{
	struct mt7915_dev *dev = phy->dev;
	struct mt7915_sku_val {
		u8 format_id;
		u8 val;
		u8 band;
		u8 _rsv;
	} __packed req = {
		.format_id = 1,
		.band = phy->band_idx,
		.val = !!drop_level,
	};
	int ret;

	ret = mt76_mcu_send_msg(&dev->mt76,
				MCU_EXT_CMD(TX_POWER_FEATURE_CTRL), &req,
				sizeof(req), true);
	if (ret)
		return ret;

	req.format_id = 2;
	if ((drop_level > 90 && drop_level < 100) || !drop_level)
		req.val = 0;
	else if (drop_level > 60 && drop_level <= 90)
		/* reduce Pwr for 1 dB. */
		req.val = 2;
	else if (drop_level > 30 && drop_level <= 60)
		/* reduce Pwr for 3 dB. */
		req.val = 6;
	else if (drop_level > 15 && drop_level <= 30)
		/* reduce Pwr for 6 dB. */
		req.val = 12;
	else if (drop_level > 9 && drop_level <= 15)
		/* reduce Pwr for 9 dB. */
		req.val = 18;
	else if (drop_level > 0 && drop_level <= 9)
		/* reduce Pwr for 12 dB. */
		req.val = 24;

	return mt76_mcu_send_msg(&dev->mt76,
				 MCU_EXT_CMD(TX_POWER_FEATURE_CTRL), &req,
				 sizeof(req), true);
}
