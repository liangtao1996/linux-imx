// SPDX-License-Identifier: GPL-2.0-only
/*
 * Driver for the AP130X external camera ISP from ON Semiconductor
 *
 * Copyright (C) 2021, Witekio, Inc.
 * Copyright (C) 2021, Xilinx, Inc.
 * Copyright (C) 2021, Laurent Pinchart <laurent.pinchart@ideasonboard.com>
 */

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/media.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <media/media-entity.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>

#define DRIVER_NAME "ap130x"

#define AP130X_FW_WINDOW_SIZE			0x2000
#define AP130X_FW_WINDOW_OFFSET			0x8000

#define AP130X_MIN_WIDTH			24U
#define AP130X_MIN_HEIGHT			16U
#define AP130X_MAX_WIDTH			4224U
#define AP130X_MAX_HEIGHT			4092U

#define AP130X_REG_16BIT(n)			((2 << 24) | (n))
#define AP130X_REG_32BIT(n)			((4 << 24) | (n))
#define AP130X_REG_SIZE(n)			((n) >> 24)
#define AP130X_REG_ADDR(n)			((n) & 0x0000ffff)
#define AP130X_REG_PAGE(n)			((n) & 0x00ff0000)
#define AP130X_REG_PAGE_MASK			0x00ff0000

/* Info Registers */
#define AP130X_CHIP_VERSION			AP130X_REG_16BIT(0x0000)
#define AP130X_CHIP_ID				0x0265
#define AP130X_FRAME_CNT			AP130X_REG_16BIT(0x0002)
#define AP130X_ERROR				AP130X_REG_16BIT(0x0006)
#define AP130X_ERR_FILE				AP130X_REG_32BIT(0x0008)
#define AP130X_ERR_LINE				AP130X_REG_16BIT(0x000c)
#define AP130X_SIPM_ERR_0			AP130X_REG_16BIT(0x0014)
#define AP130X_SIPM_ERR_1			AP130X_REG_16BIT(0x0016)
#define AP130X_CHIP_REV				AP130X_REG_16BIT(0x0050)
#define AP130X_CON_BUF(n)			AP130X_REG_16BIT(0x0a2c + (n))
#define AP130X_CON_BUF_SIZE			512

/* Control Registers */
#define AP130X_DZ_TGT_FCT			AP130X_REG_16BIT(0x1010)
#define AP130X_SFX_MODE				AP130X_REG_16BIT(0x1016)
#define AP130X_SFX_MODE_SFX_NORMAL		(0U << 0)
#define AP130X_SFX_MODE_SFX_ALIEN		(1U << 0)
#define AP130X_SFX_MODE_SFX_ANTIQUE		(2U << 0)
#define AP130X_SFX_MODE_SFX_BW			(3U << 0)
#define AP130X_SFX_MODE_SFX_EMBOSS		(4U << 0)
#define AP130X_SFX_MODE_SFX_EMBOSS_COLORED	(5U << 0)
#define AP130X_SFX_MODE_SFX_GRAYSCALE		(6U << 0)
#define AP130X_SFX_MODE_SFX_NEGATIVE		(7U << 0)
#define AP130X_SFX_MODE_SFX_BLUISH		(8U << 0)
#define AP130X_SFX_MODE_SFX_GREENISH		(9U << 0)
#define AP130X_SFX_MODE_SFX_REDISH		(10U << 0)
#define AP130X_SFX_MODE_SFX_POSTERIZE1		(11U << 0)
#define AP130X_SFX_MODE_SFX_POSTERIZE2		(12U << 0)
#define AP130X_SFX_MODE_SFX_SEPIA1		(13U << 0)
#define AP130X_SFX_MODE_SFX_SEPIA2		(14U << 0)
#define AP130X_SFX_MODE_SFX_SKETCH		(15U << 0)
#define AP130X_SFX_MODE_SFX_SOLARIZE		(16U << 0)
#define AP130X_SFX_MODE_SFX_FOGGY		(17U << 0)
#define AP130X_BUBBLE_OUT_FMT			AP130X_REG_16BIT(0x1164)
#define AP130X_BUBBLE_OUT_FMT_FT_YUV		(3U << 4)
#define AP130X_BUBBLE_OUT_FMT_FT_RGB		(4U << 4)
#define AP130X_BUBBLE_OUT_FMT_FT_YUV_JFIF	(5U << 4)
#define AP130X_BUBBLE_OUT_FMT_FST_RGB_888	(0U << 0)
#define AP130X_BUBBLE_OUT_FMT_FST_RGB_565	(1U << 0)
#define AP130X_BUBBLE_OUT_FMT_FST_RGB_555M	(2U << 0)
#define AP130X_BUBBLE_OUT_FMT_FST_RGB_555L	(3U << 0)
#define AP130X_BUBBLE_OUT_FMT_FST_YUV_422	(0U << 0)
#define AP130X_BUBBLE_OUT_FMT_FST_YUV_420	(1U << 0)
#define AP130X_BUBBLE_OUT_FMT_FST_YUV_400	(2U << 0)
#define AP130X_ATOMIC				AP130X_REG_16BIT(0x1184)
#define AP130X_ATOMIC_MODE			BIT(2)
#define AP130X_ATOMIC_FINISH			BIT(1)
#define AP130X_ATOMIC_RECORD			BIT(0)

/*
 * Preview Context Registers (preview_*). AP130X supports 3 "contexts"
 * (Preview, Snapshot, Video). These can be programmed for different size,
 * format, FPS, etc. There is no functional difference between the contexts,
 * so the only potential benefit of using them is reduced number of register
 * writes when switching output modes (if your concern is atomicity, see
 * "atomic" register).
 * So there's virtually no benefit in using contexts for this driver and it
 * would significantly increase complexity. Let's use preview context only.
 */
#define AP130X_PREVIEW_WIDTH			AP130X_REG_16BIT(0x2000)
#define AP130X_PREVIEW_HEIGHT			AP130X_REG_16BIT(0x2002)
#define AP130X_PREVIEW_ROI_X0			AP130X_REG_16BIT(0x2004)
#define AP130X_PREVIEW_ROI_Y0			AP130X_REG_16BIT(0x2006)
#define AP130X_PREVIEW_ROI_X1			AP130X_REG_16BIT(0x2008)
#define AP130X_PREVIEW_ROI_Y1			AP130X_REG_16BIT(0x200a)
#define AP130X_PREVIEW_OUT_FMT			AP130X_REG_16BIT(0x2012)
#define AP130X_PREVIEW_OUT_FMT_IPIPE_BYPASS	BIT(13)
#define AP130X_PREVIEW_OUT_FMT_SS		BIT(12)
#define AP130X_PREVIEW_OUT_FMT_FAKE_EN		BIT(11)
#define AP130X_PREVIEW_OUT_FMT_ST_EN		BIT(10)
#define AP130X_PREVIEW_OUT_FMT_IIS_NONE		(0U << 8)
#define AP130X_PREVIEW_OUT_FMT_IIS_POST_VIEW	(1U << 8)
#define AP130X_PREVIEW_OUT_FMT_IIS_VIDEO	(2U << 8)
#define AP130X_PREVIEW_OUT_FMT_IIS_BUBBLE	(3U << 8)
#define AP130X_PREVIEW_OUT_FMT_FT_JPEG_422	(0U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_JPEG_420	(1U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_YUV		(3U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_RGB		(4U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_YUV_JFIF	(5U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_RAW8		(8U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_RAW10		(9U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_RAW12		(10U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_RAW16		(11U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_DNG8		(12U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_DNG10		(13U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_DNG12		(14U << 4)
#define AP130X_PREVIEW_OUT_FMT_FT_DNG16		(15U << 4)
#define AP130X_PREVIEW_OUT_FMT_FST_JPEG_ROTATE	BIT(2)
#define AP130X_PREVIEW_OUT_FMT_FST_JPEG_SCAN	(0U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_JPEG_JFIF	(1U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_JPEG_EXIF	(2U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RGB_888	(0U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RGB_565	(1U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RGB_555M	(2U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RGB_555L	(3U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_YUV_422	(0U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_YUV_420	(1U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_YUV_400	(2U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_SENSOR	(0U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_CAPTURE	(1U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_CP	(2U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_BPC	(3U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_IHDR	(4U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_PP	(5U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_DENSH	(6U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_PM	(7U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_GC	(8U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_CURVE	(9U << 0)
#define AP130X_PREVIEW_OUT_FMT_FST_RAW_CCONV	(10U << 0)
#define AP130X_PREVIEW_S1_SENSOR_MODE		AP130X_REG_16BIT(0x202e)
#define AP130X_PREVIEW_HINF_CTRL		AP130X_REG_16BIT(0x2030)
#define AP130X_PREVIEW_HINF_CTRL_BT656_LE	BIT(15)
#define AP130X_PREVIEW_HINF_CTRL_BT656_16BIT	BIT(14)
#define AP130X_PREVIEW_HINF_CTRL_MUX_DELAY(n)	((n) << 8)
#define AP130X_PREVIEW_HINF_CTRL_LV_POL		BIT(7)
#define AP130X_PREVIEW_HINF_CTRL_FV_POL		BIT(6)
#define AP130X_PREVIEW_HINF_CTRL_MIPI_CONT_CLK	BIT(5)
#define AP130X_PREVIEW_HINF_CTRL_SPOOF		BIT(4)
#define AP130X_PREVIEW_HINF_CTRL_MIPI_MODE	BIT(3)
#define AP130X_PREVIEW_HINF_CTRL_MIPI_LANES(n)	((n) << 0)

/* IQ Registers */
#define AP130X_AE_CTRL			AP130X_REG_16BIT(0x5002)
#define AP130X_AE_CTRL_STATS_SEL		BIT(11)
#define AP130X_AE_CTRL_IMM				BIT(10)
#define AP130X_AE_CTRL_ROUND_ISO		BIT(9)
#define AP130X_AE_CTRL_UROI_FACE		BIT(7)
#define AP130X_AE_CTRL_UROI_LOCK		BIT(6)
#define AP130X_AE_CTRL_UROI_BOUND		BIT(5)
#define AP130X_AE_CTRL_IMM1				BIT(4)
#define AP130X_AE_CTRL_MANUAL_EXP_TIME_GAIN	(0U << 0)
#define AP130X_AE_CTRL_MANUAL_BV_EXP_TIME	(1U << 0)
#define AP130X_AE_CTRL_MANUAL_BV_GAIN		(2U << 0)
#define AP130X_AE_CTRL_MANUAL_BV_ISO		(3U << 0)
#define AP130X_AE_CTRL_AUTO_BV_EXP_TIME		(9U << 0)
#define AP130X_AE_CTRL_AUTO_BV_GAIN			(10U << 0)
#define AP130X_AE_CTRL_AUTO_BV_ISO			(11U << 0)
#define AP130X_AE_CTRL_FULL_AUTO			(12U << 0)
#define AP130X_AE_CTRL_MODE_MASK		0x000f
#define AP130X_AE_MANUAL_GAIN		AP130X_REG_16BIT(0x5006)
#define AP130X_AE_BV_OFF			AP130X_REG_16BIT(0x5014)
#define AP130X_AE_MET				AP130X_REG_16BIT(0x503E)
#define AP130X_AWB_CTRL				AP130X_REG_16BIT(0x5100)
#define AP130X_AWB_CTRL_RECALC			BIT(13)
#define AP130X_AWB_CTRL_POSTGAIN		BIT(12)
#define AP130X_AWB_CTRL_UNGAIN			BIT(11)
#define AP130X_AWB_CTRL_CLIP			BIT(10)
#define AP130X_AWB_CTRL_SKY			BIT(9)
#define AP130X_AWB_CTRL_FLASH			BIT(8)
#define AP130X_AWB_CTRL_FACE_OFF		(0U << 6)
#define AP130X_AWB_CTRL_FACE_IGNORE		(1U << 6)
#define AP130X_AWB_CTRL_FACE_CONSTRAINED	(2U << 6)
#define AP130X_AWB_CTRL_FACE_ONLY		(3U << 6)
#define AP130X_AWB_CTRL_IMM			BIT(5)
#define AP130X_AWB_CTRL_IMM1			BIT(4)
#define AP130X_AWB_CTRL_MODE_OFF		(0U << 0)
#define AP130X_AWB_CTRL_MODE_HORIZON		(1U << 0)
#define AP130X_AWB_CTRL_MODE_A			(2U << 0)
#define AP130X_AWB_CTRL_MODE_CWF		(3U << 0)
#define AP130X_AWB_CTRL_MODE_D50		(4U << 0)
#define AP130X_AWB_CTRL_MODE_D65		(5U << 0)
#define AP130X_AWB_CTRL_MODE_D75		(6U << 0)
#define AP130X_AWB_CTRL_MODE_MANUAL		(7U << 0)
#define AP130X_AWB_CTRL_MODE_MEASURE		(8U << 0)
#define AP130X_AWB_CTRL_MODE_AUTO		(15U << 0)
#define AP130X_AWB_CTRL_MODE_MASK		0x000f
#define AP130X_FLICK_CTRL			AP130X_REG_16BIT(0x5440)
#define AP130X_FLICK_CTRL_FREQ(n)		((n) << 8)
#define AP130X_FLICK_CTRL_ETC_IHDR_UP		BIT(6)
#define AP130X_FLICK_CTRL_ETC_DIS		BIT(5)
#define AP130X_FLICK_CTRL_FRC_OVERRIDE_MAX_ET	BIT(4)
#define AP130X_FLICK_CTRL_FRC_OVERRIDE_UPPER_ET	BIT(3)
#define AP130X_FLICK_CTRL_FRC_EN		BIT(2)
#define AP130X_FLICK_CTRL_MODE_DISABLED		(0U << 0)
#define AP130X_FLICK_CTRL_MODE_MANUAL		(1U << 0)
#define AP130X_FLICK_CTRL_MODE_AUTO		(2U << 0)
#define AP130X_SCENE_CTRL			AP130X_REG_16BIT(0x5454)
#define AP130X_SCENE_CTRL_MODE_NORMAL		(0U << 0)
#define AP130X_SCENE_CTRL_MODE_PORTRAIT		(1U << 0)
#define AP130X_SCENE_CTRL_MODE_LANDSCAPE	(2U << 0)
#define AP130X_SCENE_CTRL_MODE_SPORT		(3U << 0)
#define AP130X_SCENE_CTRL_MODE_CLOSE_UP		(4U << 0)
#define AP130X_SCENE_CTRL_MODE_NIGHT		(5U << 0)
#define AP130X_SCENE_CTRL_MODE_TWILIGHT		(6U << 0)
#define AP130X_SCENE_CTRL_MODE_BACKLIGHT	(7U << 0)
#define AP130X_SCENE_CTRL_MODE_HIGH_SENSITIVE	(8U << 0)
#define AP130X_SCENE_CTRL_MODE_NIGHT_PORTRAIT	(9U << 0)
#define AP130X_SCENE_CTRL_MODE_BEACH		(10U << 0)
#define AP130X_SCENE_CTRL_MODE_DOCUMENT		(11U << 0)
#define AP130X_SCENE_CTRL_MODE_PARTY		(12U << 0)
#define AP130X_SCENE_CTRL_MODE_FIREWORKS	(13U << 0)
#define AP130X_SCENE_CTRL_MODE_SUNSET		(14U << 0)
#define AP130X_SCENE_CTRL_MODE_AUTO		(0xffU << 0)

/* System Registers */
#define AP130X_BOOTDATA_STAGE			AP130X_REG_16BIT(0x6002)
#define AP130X_WARNING(n)			AP130X_REG_16BIT(0x6004 + (n) * 2)
#define AP130X_SENSOR_SELECT			AP130X_REG_16BIT(0x600c)
#define AP130X_SENSOR_SELECT_TP_MODE(n)		((n) << 8)
#define AP130X_SENSOR_SELECT_PATTERN_ON		BIT(7)
#define AP130X_SENSOR_SELECT_MODE_3D_ON		BIT(6)
#define AP130X_SENSOR_SELECT_CLOCK		BIT(5)
#define AP130X_SENSOR_SELECT_SINF_MIPI		BIT(4)
#define AP130X_SENSOR_SELECT_YUV		BIT(2)
#define AP130X_SENSOR_SELECT_SENSOR_TP		(0U << 0)
#define AP130X_SENSOR_SELECT_SENSOR(n)		(((n) + 1) << 0)
#define AP130X_SYS_START			AP130X_REG_16BIT(0x601a)
#define AP130X_SYS_START_PLL_LOCK		BIT(15)
#define AP130X_SYS_START_LOAD_OTP		BIT(12)
#define AP130X_SYS_START_RESTART_ERROR		BIT(11)
#define AP130X_SYS_START_STALL_STATUS		BIT(9)
#define AP130X_SYS_START_STALL_EN		BIT(8)
#define AP130X_SYS_START_STALL_MODE_FRAME	(0U << 6)
#define AP130X_SYS_START_STALL_MODE_DISABLED	(1U << 6)
#define AP130X_SYS_START_STALL_MODE_POWER_DOWN	(2U << 6)
#define AP130X_SYS_START_GO			BIT(4)
#define AP130X_SYS_START_PATCH_FUN		BIT(1)
#define AP130X_SYS_START_PLL_INIT		BIT(0)
#define AP130X_DMA_SRC				AP130X_REG_32BIT(0x60a0)
#define AP130X_DMA_DST				AP130X_REG_32BIT(0x60a4)
#define AP130X_DMA_SIP_SIPM(n)			((n) << 26)
#define AP130X_DMA_SIP_DATA_16_BIT		BIT(25)
#define AP130X_DMA_SIP_ADDR_16_BIT		BIT(24)
#define AP130X_DMA_SIP_ID(n)			((n) << 17)
#define AP130X_DMA_SIP_REG(n)			((n) << 0)
#define AP130X_DMA_SIZE				AP130X_REG_32BIT(0x60a8)
#define AP130X_DMA_CTRL				AP130X_REG_16BIT(0x60ac)
#define AP130X_DMA_CTRL_SCH_NORMAL		(0 << 12)
#define AP130X_DMA_CTRL_SCH_NEXT		(1 << 12)
#define AP130X_DMA_CTRL_SCH_NOW			(2 << 12)
#define AP130X_DMA_CTRL_DST_REG			(0 << 8)
#define AP130X_DMA_CTRL_DST_SRAM		(1 << 8)
#define AP130X_DMA_CTRL_DST_SPI			(2 << 8)
#define AP130X_DMA_CTRL_DST_SIP			(3 << 8)
#define AP130X_DMA_CTRL_SRC_REG			(0 << 4)
#define AP130X_DMA_CTRL_SRC_SRAM		(1 << 4)
#define AP130X_DMA_CTRL_SRC_SPI			(2 << 4)
#define AP130X_DMA_CTRL_SRC_SIP			(3 << 4)
#define AP130X_DMA_CTRL_MODE_32_BIT		BIT(3)
#define AP130X_DMA_CTRL_MODE_MASK		(7 << 0)
#define AP130X_DMA_CTRL_MODE_IDLE		(0 << 0)
#define AP130X_DMA_CTRL_MODE_SET		(1 << 0)
#define AP130X_DMA_CTRL_MODE_COPY		(2 << 0)
#define AP130X_DMA_CTRL_MODE_MAP		(3 << 0)
#define AP130X_DMA_CTRL_MODE_UNPACK		(4 << 0)
#define AP130X_DMA_CTRL_MODE_OTP_READ		(5 << 0)
#define AP130X_DMA_CTRL_MODE_SIP_PROBE		(6 << 0)

#define AP130X_BRIGHTNESS			AP130X_REG_16BIT(0x7000)
#define AP130X_CONTRAST				AP130X_REG_16BIT(0x7002)
#define AP130X_SATURATION			AP130X_REG_16BIT(0x7006)
#define AP130X_GAMMA				AP130X_REG_16BIT(0x700A)

/* Misc Registers */
#define AP130X_REG_ADV_START			0xe000
#define AP130X_ADVANCED_BASE			AP130X_REG_32BIT(0xf038)
#define AP130X_SIP_CRC				AP130X_REG_16BIT(0xf052)

/* Advanced System Registers */
#define AP130X_ADV_IRQ_SYS_INTE			AP130X_REG_32BIT(0x00230000)
#define AP130X_ADV_IRQ_SYS_INTE_TEST_COUNT	BIT(25)
#define AP130X_ADV_IRQ_SYS_INTE_HINF_1		BIT(24)
#define AP130X_ADV_IRQ_SYS_INTE_HINF_0		BIT(23)
#define AP130X_ADV_IRQ_SYS_INTE_SINF_B_MIPI_L	(7U << 20)
#define AP130X_ADV_IRQ_SYS_INTE_SINF_B_MIPI	BIT(19)
#define AP130X_ADV_IRQ_SYS_INTE_SINF_A_MIPI_L	(15U << 14)
#define AP130X_ADV_IRQ_SYS_INTE_SINF_A_MIPI	BIT(13)
#define AP130X_ADV_IRQ_SYS_INTE_SINF		BIT(12)
#define AP130X_ADV_IRQ_SYS_INTE_IPIPE_S		BIT(11)
#define AP130X_ADV_IRQ_SYS_INTE_IPIPE_B		BIT(10)
#define AP130X_ADV_IRQ_SYS_INTE_IPIPE_A		BIT(9)
#define AP130X_ADV_IRQ_SYS_INTE_IP		BIT(8)
#define AP130X_ADV_IRQ_SYS_INTE_TIMER		BIT(7)
#define AP130X_ADV_IRQ_SYS_INTE_SIPM		(3U << 6)
#define AP130X_ADV_IRQ_SYS_INTE_SIPS_ADR_RANGE	BIT(5)
#define AP130X_ADV_IRQ_SYS_INTE_SIPS_DIRECT_WRITE	BIT(4)
#define AP130X_ADV_IRQ_SYS_INTE_SIPS_FIFO_WRITE	BIT(3)
#define AP130X_ADV_IRQ_SYS_INTE_SPI		BIT(2)
#define AP130X_ADV_IRQ_SYS_INTE_GPIO_CNT	BIT(1)
#define AP130X_ADV_IRQ_SYS_INTE_GPIO_PIN	BIT(0)

/* Advanced Slave MIPI Registers */
#define AP130X_ADV_SINF_MIPI_INTERNAL_p_LANE_n_STAT(p, n) \
	AP130X_REG_32BIT(0x00420008 + (p) * 0x50000 + (n) * 0x20)
#define AP130X_LANE_ERR_LP_VAL(n)		(((n) >> 30) & 3)
#define AP130X_LANE_ERR_STATE(n)		(((n) >> 24) & 0xf)
#define AP130X_LANE_ERR				BIT(18)
#define AP130X_LANE_ABORT			BIT(17)
#define AP130X_LANE_LP_VAL(n)			(((n) >> 6) & 3)
#define AP130X_LANE_STATE(n)			((n) & 0xf)
#define AP130X_LANE_STATE_STOP_S		0x0
#define AP130X_LANE_STATE_HS_REQ_S		0x1
#define AP130X_LANE_STATE_LP_REQ_S		0x2
#define AP130X_LANE_STATE_HS_S			0x3
#define AP130X_LANE_STATE_LP_S			0x4
#define AP130X_LANE_STATE_ESC_REQ_S		0x5
#define AP130X_LANE_STATE_TURN_REQ_S		0x6
#define AP130X_LANE_STATE_ESC_S			0x7
#define AP130X_LANE_STATE_ESC_0			0x8
#define AP130X_LANE_STATE_ESC_1			0x9
#define AP130X_LANE_STATE_TURN_S		0xa
#define AP130X_LANE_STATE_TURN_MARK		0xb
#define AP130X_LANE_STATE_ERROR_S		0xc

#define AP130X_ADV_CAPTURE_A_FV_CNT		AP130X_REG_32BIT(0x00490040)

struct ap130x_device;

enum {
	AP130X_PAD_SINK_0,
	AP130X_PAD_SINK_1,
	AP130X_PAD_SOURCE,
	AP130X_PAD_MAX,
};

struct ap130x_format_info {
	unsigned int code;
	u16 out_fmt;
};

struct ap130x_format {
	struct v4l2_mbus_framefmt format;
	const struct ap130x_format_info *info;
};

struct ap130x_size {
	unsigned int width;
	unsigned int height;
};

struct ap130x_sensor_supply {
	const char *name;
	unsigned int post_delay_us;
};

static const struct ap130x_sensor_supply ap130x_supplies[] = {
	{ .name = "DVDD",        .post_delay_us = 2000, },
	{ .name = "VDDIO_HMISC", .post_delay_us = 2000, },
	{ .name = "VDDIO_SMISC", .post_delay_us = 2000, },
};

#define AP130X_NUM_SUPPLIES	ARRAY_SIZE(ap130x_supplies)

struct ap130x_sensor_info {
	const char *model;
	const char *name;
	unsigned int i2c_addr;
	struct ap130x_size resolution;
	u32 format;
	const struct ap130x_sensor_supply *supplies;
};

struct ap130x_sensor {
	struct ap130x_device *ap130x;
	unsigned int index;

	struct device_node *of_node;
	struct device *dev;
	unsigned int num_supplies;
	struct regulator_bulk_data *supplies;

	struct v4l2_subdev sd;
	struct media_pad pad;
};

static inline struct ap130x_sensor *to_ap130x_sensor(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ap130x_sensor, sd);
}

struct ap130x_device {
	struct device *dev;
	struct i2c_client *client;

	struct gpio_desc *reset_gpio;
	struct gpio_desc *standby_gpio;
	struct gpio_desc *isp_en_gpio;
	struct clk *clock;
	struct regmap *regmap16;
	struct regmap *regmap32;
	u32 reg_page;

	const struct firmware *fw;

	struct v4l2_fwnode_endpoint bus_cfg;

	struct mutex lock;	/* Protects formats */

	struct v4l2_subdev sd;
	struct media_pad pads[AP130X_PAD_MAX];
	struct ap130x_format formats[AP130X_PAD_MAX];
	unsigned int width_factor;
	bool streaming;

	struct v4l2_ctrl_handler ctrls;

	const struct ap130x_sensor_info *sensor_info;
	struct ap130x_sensor sensors[2];

	struct regulator_bulk_data supplies[AP130X_NUM_SUPPLIES];

	struct {
		struct dentry *dir;
		struct mutex lock;
		u32 sipm_addr;
	} debugfs;
};

static inline struct ap130x_device *to_ap130x(struct v4l2_subdev *sd)
{
	return container_of(sd, struct ap130x_device, sd);
}

struct ap130x_firmware_header {
	u32 crc;
	u32 checksum;
	u32 pll_init_size;
	u32 total_size;
} __packed;

static const struct ap130x_format_info supported_video_formats[] = {
	{
		.code = MEDIA_BUS_FMT_UYVY8_1X16,
		.out_fmt = AP130X_PREVIEW_OUT_FMT_FT_YUV_JFIF
			 | AP130X_PREVIEW_OUT_FMT_FST_YUV_422,
	}, {
		.code = MEDIA_BUS_FMT_UYYVYY8_0_5X24,
		.out_fmt = AP130X_PREVIEW_OUT_FMT_FT_YUV_JFIF
			 | AP130X_PREVIEW_OUT_FMT_FST_YUV_420,
	},
};

/* -----------------------------------------------------------------------------
 * Sensor Info
 */

static const struct ap130x_sensor_info ap130x_sensor_info[] = {
	{
		.model = "onnn,ar0144",
		.name = "ar0144",
		.i2c_addr = 0x10,
		.resolution = { 1280, 800 },
		.format = MEDIA_BUS_FMT_SGRBG12_1X12,
		.supplies = (const struct ap130x_sensor_supply[]) {
			{ "vaa", 100 },
			{ "vddio", 100 },
			{ "vdd", 0 },
			{ NULL, 0 },
		},
	}, {
		.model = "onnn,ar0330",
		.name = "ar0330",
		.i2c_addr = 0x10,
		.resolution = { 2304, 1536 },
		.format = MEDIA_BUS_FMT_SGRBG12_1X12,
		.supplies = (const struct ap130x_sensor_supply[]) {
			{ "vddpll", 0 },
			{ "vaa", 0 },
			{ "vdd", 0 },
			{ "vddio", 0 },
			{ NULL, 0 },
		},
	}, {
		.model = "onnn,ar1335",
		.name = "ar1335",
		.i2c_addr = 0x36,
		.resolution = { 4208, 3120 },
		.format = MEDIA_BUS_FMT_SGRBG10_1X10,
		.supplies = (const struct ap130x_sensor_supply[]) {
			{ "vaa", 0 },
			{ "vddio", 0 },
			{ "vdd", 0 },
			{ NULL, 0 },
		},
	},
};

static const struct ap130x_sensor_info ap130x_sensor_info_tpg = {
	.model = "",
	.name = "tpg",
	.resolution = { 1920, 1080 },
};

/* -----------------------------------------------------------------------------
 * Register Configuration
 */

static const struct regmap_config ap130x_reg16_config = {
	.name = "val_16bits",
	.reg_bits = 16,
	.val_bits = 16,
	.reg_stride = 2,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static const struct regmap_config ap130x_reg32_config = {
	.name = "val_32bits",
	.reg_bits = 16,
	.val_bits = 32,
	.reg_stride = 4,
	.reg_format_endian = REGMAP_ENDIAN_BIG,
	.val_format_endian = REGMAP_ENDIAN_BIG,
	.cache_type = REGCACHE_NONE,
};

static int __ap130x_write(struct ap130x_device *ap130x, u32 reg, u32 val)
{
	unsigned int size = AP130X_REG_SIZE(reg);
	u16 addr = AP130X_REG_ADDR(reg);
	int ret;

	switch (size) {
	case 2:
		ret = regmap_write(ap130x->regmap16, addr, val);
		break;
	case 4:
		ret = regmap_write(ap130x->regmap32, addr, val);
		break;
	default:
		return -EINVAL;
	}

	if (ret) {
		dev_err(ap130x->dev, "%s: register 0x%04x %s failed: %d\n",
			__func__, addr, "write", ret);
		return ret;
	}

	return 0;
}

static int ap130x_write(struct ap130x_device *ap130x, u32 reg, u32 val,
			int *err)
{
	u32 page = AP130X_REG_PAGE(reg);
	int ret;

	if (err && *err)
		return *err;

	if (page) {
		if (ap130x->reg_page != page) {
			ret = __ap130x_write(ap130x, AP130X_ADVANCED_BASE,
					     page);
			if (ret < 0)
				goto done;

			ap130x->reg_page = page;
		}

		reg &= ~AP130X_REG_PAGE_MASK;
		reg += AP130X_REG_ADV_START;
	}

	ret = __ap130x_write(ap130x, reg, val);

done:
	if (err && ret)
		*err = ret;

	return ret;
}

static int __ap130x_read(struct ap130x_device *ap130x, u32 reg, u32 *val)
{
	unsigned int size = AP130X_REG_SIZE(reg);
	u16 addr = AP130X_REG_ADDR(reg);
	int ret;

	switch (size) {
	case 2:
		ret = regmap_read(ap130x->regmap16, addr, val);
		break;
	case 4:
		ret = regmap_read(ap130x->regmap32, addr, val);
		break;
	default:
		return -EINVAL;
	}

	if (ret) {
		dev_err(ap130x->dev, "%s: register 0x%04x %s failed: %d\n",
			__func__, addr, "read", ret);
		return ret;
	}

	dev_dbg(ap130x->dev, "%s: R0x%04x = 0x%0*x\n", __func__,
		addr, size * 2, *val);

	return 0;
}

static int ap130x_read(struct ap130x_device *ap130x, u32 reg, u32 *val)
{
	u32 page = AP130X_REG_PAGE(reg);
	int ret;

	if (page) {
		if (ap130x->reg_page != page) {
			ret = __ap130x_write(ap130x, AP130X_ADVANCED_BASE,
					     page);
			if (ret < 0)
				return ret;

			ap130x->reg_page = page;
		}

		reg &= ~AP130X_REG_PAGE_MASK;
		reg += AP130X_REG_ADV_START;
	}

	return __ap130x_read(ap130x, reg, val);
}

/* -----------------------------------------------------------------------------
 * Sensor Registers Access
 *
 * Read and write sensor registers through the AP130X DMA interface.
 */

static int ap130x_dma_wait_idle(struct ap130x_device *ap130x)
{
	unsigned int i;
	u32 ctrl;
	int ret;

	for (i = 50; i > 0; i--) {
		ret = ap130x_read(ap130x, AP130X_DMA_CTRL, &ctrl);
		if (ret < 0)
			return ret;

		if ((ctrl & AP130X_DMA_CTRL_MODE_MASK) ==
		    AP130X_DMA_CTRL_MODE_IDLE)
			break;

		usleep_range(1000, 1500);
	}

	if (!i) {
		dev_err(ap130x->dev, "DMA timeout\n");
		return -ETIMEDOUT;
	}

	return 0;
}

static int ap130x_sipm_read(struct ap130x_device *ap130x, unsigned int port,
			    u32 reg, u32 *val)
{
	unsigned int size = AP130X_REG_SIZE(reg);
	u32 src;
	int ret;

	if (size > 2)
		return -EINVAL;

	ret = ap130x_dma_wait_idle(ap130x);
	if (ret < 0)
		return ret;

	ap130x_write(ap130x, AP130X_DMA_SIZE, size, &ret);
	src = AP130X_DMA_SIP_SIPM(port)
	    | (size == 2 ? AP130X_DMA_SIP_DATA_16_BIT : 0)
	    | AP130X_DMA_SIP_ADDR_16_BIT
	    | AP130X_DMA_SIP_ID(ap130x->sensor_info->i2c_addr)
	    | AP130X_DMA_SIP_REG(AP130X_REG_ADDR(reg));
	ap130x_write(ap130x, AP130X_DMA_SRC, src, &ret);

	/*
	 * Use the AP130X_DMA_DST register as both the destination address, and
	 * the scratch pad to store the read value.
	 */
	ap130x_write(ap130x, AP130X_DMA_DST, AP130X_REG_ADDR(AP130X_DMA_DST),
		     &ret);

	ap130x_write(ap130x, AP130X_DMA_CTRL,
		     AP130X_DMA_CTRL_SCH_NORMAL |
		     AP130X_DMA_CTRL_DST_REG |
		     AP130X_DMA_CTRL_SRC_SIP |
		     AP130X_DMA_CTRL_MODE_COPY, &ret);
	if (ret < 0)
		return ret;

	ret = ap130x_dma_wait_idle(ap130x);
	if (ret < 0)
		return ret;

	ret = ap130x_read(ap130x, AP130X_DMA_DST, val);
	if (ret < 0)
		return ret;

	/*
	 * The value is stored in big-endian at the DMA_DST address. The regmap
	 * uses big-endian, so 8-bit values are stored in bits 31:24 and 16-bit
	 * values in bits 23:16.
	 */
	*val >>= 32 - size * 8;

	return 0;
}

static int ap130x_sipm_write(struct ap130x_device *ap130x, unsigned int port,
			     u32 reg, u32 val)
{
	unsigned int size = AP130X_REG_SIZE(reg);
	u32 dst;
	int ret;

	if (size > 2)
		return -EINVAL;

	ret = ap130x_dma_wait_idle(ap130x);
	if (ret < 0)
		return ret;

	ap130x_write(ap130x, AP130X_DMA_SIZE, size, &ret);

	/*
	 * Use the AP130X_DMA_SRC register as both the source address, and the
	 * scratch pad to store the write value.
	 *
	 * As the AP130X uses big endian, to store the value at address DMA_SRC
	 * it must be written in the high order bits of the registers. However,
	 * 8-bit values seem to be incorrectly handled by the AP130X, which
	 * expects them to be stored at DMA_SRC + 1 instead of DMA_SRC. The
	 * value is thus unconditionally shifted by 16 bits, unlike for DMA
	 * reads.
	 */
	ap130x_write(ap130x, AP130X_DMA_SRC,
		     (val << 16) | AP130X_REG_ADDR(AP130X_DMA_SRC), &ret);
	if (ret < 0)
		return ret;

	dst = AP130X_DMA_SIP_SIPM(port)
	    | (size == 2 ? AP130X_DMA_SIP_DATA_16_BIT : 0)
	    | AP130X_DMA_SIP_ADDR_16_BIT
	    | AP130X_DMA_SIP_ID(ap130x->sensor_info->i2c_addr)
	    | AP130X_DMA_SIP_REG(AP130X_REG_ADDR(reg));
	ap130x_write(ap130x, AP130X_DMA_DST, dst, &ret);

	ap130x_write(ap130x, AP130X_DMA_CTRL,
		     AP130X_DMA_CTRL_SCH_NORMAL |
		     AP130X_DMA_CTRL_DST_SIP |
		     AP130X_DMA_CTRL_SRC_REG |
		     AP130X_DMA_CTRL_MODE_COPY, &ret);
	if (ret < 0)
		return ret;

	ret = ap130x_dma_wait_idle(ap130x);
	if (ret < 0)
		return ret;

	return 0;
}

/* -----------------------------------------------------------------------------
 * Debugfs
 */

static int ap130x_sipm_addr_get(void *arg, u64 *val)
{
	struct ap130x_device *ap130x = arg;

	mutex_lock(&ap130x->debugfs.lock);
	*val = ap130x->debugfs.sipm_addr;
	mutex_unlock(&ap130x->debugfs.lock);

	return 0;
}

static int ap130x_sipm_addr_set(void *arg, u64 val)
{
	struct ap130x_device *ap130x = arg;

	if (val & ~0x8700ffff)
		return -EINVAL;

	switch ((val >> 24) & 7) {
	case 1:
	case 2:
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&ap130x->debugfs.lock);
	ap130x->debugfs.sipm_addr = val;
	mutex_unlock(&ap130x->debugfs.lock);

	return 0;
}

static int ap130x_sipm_data_get(void *arg, u64 *val)
{
	struct ap130x_device *ap130x = arg;
	u32 value;
	u32 addr;
	int ret;

	mutex_lock(&ap130x->debugfs.lock);

	addr = ap130x->debugfs.sipm_addr;
	if (!addr) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = ap130x_sipm_read(ap130x, addr >> 30, addr & ~BIT(31),
			       &value);
	if (!ret)
		*val = value;

unlock:
	mutex_unlock(&ap130x->debugfs.lock);

	return ret;
}

static int ap130x_sipm_data_set(void *arg, u64 val)
{
	struct ap130x_device *ap130x = arg;
	u32 addr;
	int ret;

	mutex_lock(&ap130x->debugfs.lock);

	addr = ap130x->debugfs.sipm_addr;
	if (!addr) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = ap130x_sipm_write(ap130x, addr >> 30, addr & ~BIT(31),
				val);

unlock:
	mutex_unlock(&ap130x->debugfs.lock);

	return ret;
}

/*
 * The sipm_addr and sipm_data attributes expose access to the sensor I2C bus.
 *
 * To read or write a register, sipm_addr has to first be written with the
 * register address. The address is a 32-bit integer formatted as follows.
 *
 * I000 0SSS 0000 0000 RRRR RRRR RRRR RRRR
 *
 * I: SIPM index (0 or 1)
 * S: Size (1: 8-bit, 2: 16-bit)
 * R: Register address (16-bit)
 *
 * The sipm_data attribute can then be read to read the register value, or
 * written to write it.
 */

DEFINE_DEBUGFS_ATTRIBUTE(ap130x_sipm_addr_fops, ap130x_sipm_addr_get,
			 ap130x_sipm_addr_set, "0x%08llx\n");
DEFINE_DEBUGFS_ATTRIBUTE(ap130x_sipm_data_fops, ap130x_sipm_data_get,
			 ap130x_sipm_data_set, "0x%08llx\n");

static void ap130x_debugfs_init(struct ap130x_device *ap130x)
{
	struct dentry *dir;
	char name[16];

	mutex_init(&ap130x->debugfs.lock);

	snprintf(name, sizeof(name), "ap130x.%s", dev_name(ap130x->dev));

	dir = debugfs_create_dir(name, NULL);
	if (IS_ERR(dir))
		return;

	ap130x->debugfs.dir = dir;

	debugfs_create_file_unsafe("sipm_addr", 0600, ap130x->debugfs.dir,
				   ap130x, &ap130x_sipm_addr_fops);
	debugfs_create_file_unsafe("sipm_data", 0600, ap130x->debugfs.dir,
				   ap130x, &ap130x_sipm_data_fops);
}

static void ap130x_debugfs_cleanup(struct ap130x_device *ap130x)
{
	debugfs_remove_recursive(ap130x->debugfs.dir);
	mutex_destroy(&ap130x->debugfs.lock);
}

/* -----------------------------------------------------------------------------
 * Power Handling
 */

static int ap130x_power_on_sensors(struct ap130x_device *ap130x)
{
	struct ap130x_sensor *sensor;
	unsigned int i, j;
	int ret;

	if (!ap130x->sensor_info->supplies)
		return 0;

	for (i = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		sensor = &ap130x->sensors[i];
		ret = 0;

		for (j = 0; j < sensor->num_supplies; ++j) {
			unsigned int delay;

			/*
			 * We can't use regulator_bulk_enable() as it would
			 * enable all supplies in parallel, breaking the sensor
			 * power sequencing constraints.
			 */
			ret = regulator_enable(sensor->supplies[j].consumer);
			if (ret < 0) {
				dev_err(ap130x->dev,
					"Failed to enable supply %u for sensor %u\n",
					j, i);
				goto error;
			}

			delay = ap130x->sensor_info->supplies[j].post_delay_us;
			usleep_range(delay, delay + 100);
		}
	}

	return 0;

error:
	for (; j > 0; --j)
		regulator_disable(sensor->supplies[j - 1].consumer);

	for (; i > 0; --i) {
		sensor = &ap130x->sensors[i - 1];
		regulator_bulk_disable(sensor->num_supplies, sensor->supplies);
	}

	return ret;
}

static void ap130x_power_off_sensors(struct ap130x_device *ap130x)
{
	unsigned int i;

	if (!ap130x->sensor_info->supplies)
		return;

	for (i = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		struct ap130x_sensor *sensor = &ap130x->sensors[i];

		regulator_bulk_disable(sensor->num_supplies, sensor->supplies);
	}
}

static int ap130x_power_on(struct ap130x_device *ap130x)
{
	int ret;
	int i;

	/* 0. RESET was asserted when getting the GPIO. */

	/* 1. Assert STANDBY. */
	if (ap130x->standby_gpio) {
		gpiod_set_value_cansleep(ap130x->standby_gpio, 1);
		usleep_range(200, 1000);
	}

	/* 2. Power up the regulators. To be implemented. */
	for (i = 0; i < AP130X_NUM_SUPPLIES; ++i) {
		unsigned int delay;

		ret = regulator_enable(ap130x->supplies[i].consumer);
		if (ret < 0) {
			dev_err(ap130x->dev, "enabel regulator fail\n");
			return ret;
		}

		delay = ap130x_supplies[i].post_delay_us;
		usleep_range(delay, delay + 100);
	}

	/* 3. De-assert STANDBY. */
	if (ap130x->standby_gpio) {
		gpiod_set_value_cansleep(ap130x->standby_gpio, 0);
		usleep_range(200, 1000);
	}

	/* 4. Turn the clock on. */
	ret = clk_prepare_enable(ap130x->clock);
	if (ret < 0) {
		dev_err(ap130x->dev, "Failed to enable clock: %d\n", ret);
		return ret;
	}

	/* 5. De-assert RESET. */
	gpiod_set_value_cansleep(ap130x->reset_gpio, 0);

	/*
	 * 6. Wait for the AP130X to initialize. The datasheet doesn't specify
	 * how long this takes.
	 */
	usleep_range(10000, 11000);

	return 0;
}

static void ap130x_power_off(struct ap130x_device *ap130x)
{
	/* 1. Assert RESET. */
	gpiod_set_value_cansleep(ap130x->reset_gpio, 1);

	/* 2. Turn the clock off. */
	clk_disable_unprepare(ap130x->clock);

	/* 3. Assert STANDBY. */
	if (ap130x->standby_gpio) {
		gpiod_set_value_cansleep(ap130x->standby_gpio, 1);
		usleep_range(200, 1000);
	}

	/* 4. Power down the regulators. To be implemented. */
	regulator_bulk_disable(AP130X_NUM_SUPPLIES, ap130x->supplies);

	/* 5. De-assert STANDBY. */
	if (ap130x->standby_gpio) {
		usleep_range(200, 1000);
		gpiod_set_value_cansleep(ap130x->standby_gpio, 0);
	}
}

/* -----------------------------------------------------------------------------
 * Hardware Configuration
 */

static int ap130x_dump_console(struct ap130x_device *ap130x)
{
	u8 *buffer;
	u8 *endp;
	u8 *p;
	int ret;

	buffer = kmalloc(AP130X_CON_BUF_SIZE + 1, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	ret = regmap_raw_read(ap130x->regmap16, AP130X_CON_BUF(0), buffer,
			      AP130X_CON_BUF_SIZE);
	if (ret < 0) {
		dev_err(ap130x->dev, "Failed to read console buffer: %d\n",
			ret);
		goto done;
	}

	print_hex_dump(KERN_INFO, "console ", DUMP_PREFIX_OFFSET, 16, 1, buffer,
		       AP130X_CON_BUF_SIZE, true);

	buffer[AP130X_CON_BUF_SIZE] = '\0';

	for (p = buffer; p < buffer + AP130X_CON_BUF_SIZE && *p; p = endp + 1) {
		endp = strchrnul(p, '\n');
		*endp = '\0';

		pr_info("console %s\n", p);
	}

	ret = 0;

done:
	kfree(buffer);
	return ret;
}

static int ap130x_configure(struct ap130x_device *ap130x)
{
	const struct ap130x_format *format = &ap130x->formats[AP130X_PAD_SOURCE];
	unsigned int data_lanes = ap130x->bus_cfg.bus.mipi_csi2.num_data_lanes;
	int ret = 0;

	ap130x_write(ap130x, AP130X_PREVIEW_HINF_CTRL,
		     AP130X_PREVIEW_HINF_CTRL_SPOOF |
		     AP130X_PREVIEW_HINF_CTRL_MIPI_LANES(data_lanes), &ret);

	ap130x_write(ap130x, AP130X_PREVIEW_WIDTH,
		     format->format.width / ap130x->width_factor, &ret);
	ap130x_write(ap130x, AP130X_PREVIEW_HEIGHT,
		     format->format.height, &ret);
	ap130x_write(ap130x, AP130X_PREVIEW_OUT_FMT,
		     format->info->out_fmt, &ret);
	if (ret < 0)
		return ret;

	__v4l2_ctrl_handler_setup(&ap130x->ctrls);

	return 0;
}

static int ap130x_stall(struct ap130x_device *ap130x, bool stall)
{
	int ret = 0;

	if (stall) {
		ap130x_write(ap130x, AP130X_SYS_START,
			     AP130X_SYS_START_PLL_LOCK |
			     AP130X_SYS_START_STALL_MODE_DISABLED, &ret);
		ap130x_write(ap130x, AP130X_SYS_START,
			     AP130X_SYS_START_PLL_LOCK |
			     AP130X_SYS_START_STALL_EN |
			     AP130X_SYS_START_STALL_MODE_DISABLED, &ret);
		if (ret < 0)
			return ret;

		msleep(200);

		return ap130x_write(ap130x, AP130X_ADV_IRQ_SYS_INTE,
			     AP130X_ADV_IRQ_SYS_INTE_SIPM |
			     AP130X_ADV_IRQ_SYS_INTE_SIPS_FIFO_WRITE, NULL);
	} else {
		return ap130x_write(ap130x, AP130X_SYS_START,
				    AP130X_SYS_START_PLL_LOCK |
				    AP130X_SYS_START_STALL_STATUS |
				    AP130X_SYS_START_STALL_EN |
				    AP130X_SYS_START_STALL_MODE_DISABLED, NULL);
	}
}

/* -----------------------------------------------------------------------------
 * V4L2 Controls
 */

static u16 ap130x_wb_values[] = {
	AP130X_AWB_CTRL_MODE_OFF,	/* V4L2_WHITE_BALANCE_MANUAL */
	AP130X_AWB_CTRL_MODE_AUTO,	/* V4L2_WHITE_BALANCE_AUTO */
	AP130X_AWB_CTRL_MODE_A,		/* V4L2_WHITE_BALANCE_INCANDESCENT */
	AP130X_AWB_CTRL_MODE_D50,	/* V4L2_WHITE_BALANCE_FLUORESCENT */
	AP130X_AWB_CTRL_MODE_D65,	/* V4L2_WHITE_BALANCE_FLUORESCENT_H */
	AP130X_AWB_CTRL_MODE_HORIZON,	/* V4L2_WHITE_BALANCE_HORIZON */
	AP130X_AWB_CTRL_MODE_D65,	/* V4L2_WHITE_BALANCE_DAYLIGHT */
	AP130X_AWB_CTRL_MODE_AUTO,	/* V4L2_WHITE_BALANCE_FLASH */
	AP130X_AWB_CTRL_MODE_D75,	/* V4L2_WHITE_BALANCE_CLOUDY */
	AP130X_AWB_CTRL_MODE_D75,	/* V4L2_WHITE_BALANCE_SHADE */
};

static inline struct ap130x_device *ctrl_to_sd(struct v4l2_ctrl *ctrl)
{
	return container_of(ctrl->handler, struct ap130x_device, ctrls);
}

static int ap130x_set_wb_mode(struct ap130x_device *ap130x, s32 mode)
{
	u32 val;
	int ret;

	ret = ap130x_read(ap130x, AP130X_AWB_CTRL, &val);
	if (ret)
		return ret;
	val &= ~AP130X_AWB_CTRL_MODE_MASK;
	val |= ap130x_wb_values[mode];

	if (mode == V4L2_WHITE_BALANCE_FLASH)
		val |= AP130X_AWB_CTRL_FLASH;
	else
		val &= ~AP130X_AWB_CTRL_FLASH;

	return ap130x_write(ap130x, AP130X_AWB_CTRL, val, NULL);
}

static int ap130x_set_exposure(struct ap130x_device *ap130x, s32 mode)
{
	u32 val;
	int ret;

	ret = ap130x_read(ap130x, AP130X_AE_CTRL, &val);
	if (ret)
		return ret;

	val &= ~AP130X_AE_CTRL_MODE_MASK;
	val |= mode;

	return ap130x_write(ap130x, AP130X_AE_CTRL, val, NULL);
}

static int ap130x_set_exp_met(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_AE_MET, val, NULL);
}

static int ap130x_set_gain(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_AE_MANUAL_GAIN, val, NULL);
}

static int ap130x_set_contrast(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_CONTRAST, val, NULL);
}

static int ap130x_set_brightness(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_BRIGHTNESS, val, NULL);
}

static int ap130x_set_saturation(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_SATURATION, val, NULL);
}

static int ap130x_set_gamma(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_GAMMA, val, NULL);
}

static int ap130x_set_zoom(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_DZ_TGT_FCT, val, NULL);
}

static u16 ap130x_sfx_values[] = {
	AP130X_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_NONE */
	AP130X_SFX_MODE_SFX_BW,		/* V4L2_COLORFX_BW */
	AP130X_SFX_MODE_SFX_SEPIA1,	/* V4L2_COLORFX_SEPIA */
	AP130X_SFX_MODE_SFX_NEGATIVE,	/* V4L2_COLORFX_NEGATIVE */
	AP130X_SFX_MODE_SFX_EMBOSS,	/* V4L2_COLORFX_EMBOSS */
	AP130X_SFX_MODE_SFX_SKETCH,	/* V4L2_COLORFX_SKETCH */
	AP130X_SFX_MODE_SFX_BLUISH,	/* V4L2_COLORFX_SKY_BLUE */
	AP130X_SFX_MODE_SFX_GREENISH,	/* V4L2_COLORFX_GRASS_GREEN */
	AP130X_SFX_MODE_SFX_REDISH,	/* V4L2_COLORFX_SKIN_WHITEN */
	AP130X_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_VIVID */
	AP130X_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_AQUA */
	AP130X_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_ART_FREEZE */
	AP130X_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_SILHOUETTE */
	AP130X_SFX_MODE_SFX_SOLARIZE, /* V4L2_COLORFX_SOLARIZATION */
	AP130X_SFX_MODE_SFX_ANTIQUE, /* V4L2_COLORFX_ANTIQUE */
	AP130X_SFX_MODE_SFX_NORMAL,	/* V4L2_COLORFX_SET_CBCR */
};

static int ap130x_set_special_effect(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_SFX_MODE, ap130x_sfx_values[val],
			    NULL);
}

static u16 ap130x_scene_mode_values[] = {
	AP130X_SCENE_CTRL_MODE_NORMAL,		/* V4L2_SCENE_MODE_NONE */
	AP130X_SCENE_CTRL_MODE_BACKLIGHT,	/* V4L2_SCENE_MODE_BACKLIGHT */
	AP130X_SCENE_CTRL_MODE_BEACH,		/* V4L2_SCENE_MODE_BEACH_SNOW */
	AP130X_SCENE_CTRL_MODE_TWILIGHT,	/* V4L2_SCENE_MODE_CANDLE_LIGHT */
	AP130X_SCENE_CTRL_MODE_NORMAL,		/* V4L2_SCENE_MODE_DAWN_DUSK */
	AP130X_SCENE_CTRL_MODE_NORMAL,		/* V4L2_SCENE_MODE_FALL_COLORS */
	AP130X_SCENE_CTRL_MODE_FIREWORKS,	/* V4L2_SCENE_MODE_FIREWORKS */
	AP130X_SCENE_CTRL_MODE_LANDSCAPE,	/* V4L2_SCENE_MODE_LANDSCAPE */
	AP130X_SCENE_CTRL_MODE_NIGHT,		/* V4L2_SCENE_MODE_NIGHT */
	AP130X_SCENE_CTRL_MODE_PARTY,		/* V4L2_SCENE_MODE_PARTY_INDOOR */
	AP130X_SCENE_CTRL_MODE_PORTRAIT,	/* V4L2_SCENE_MODE_PORTRAIT */
	AP130X_SCENE_CTRL_MODE_SPORT,		/* V4L2_SCENE_MODE_SPORTS */
	AP130X_SCENE_CTRL_MODE_SUNSET,		/* V4L2_SCENE_MODE_SUNSET */
	AP130X_SCENE_CTRL_MODE_DOCUMENT,	/* V4L2_SCENE_MODE_TEXT */
};

static int ap130x_set_scene_mode(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_SCENE_CTRL,
			    ap130x_scene_mode_values[val], NULL);
}

static const u16 ap130x_flicker_values[] = {
	AP130X_FLICK_CTRL_MODE_DISABLED,
	AP130X_FLICK_CTRL_FREQ(50) | AP130X_FLICK_CTRL_MODE_MANUAL,
	AP130X_FLICK_CTRL_FREQ(60) | AP130X_FLICK_CTRL_MODE_MANUAL,
	AP130X_FLICK_CTRL_MODE_AUTO,
};

static int ap130x_set_flicker_freq(struct ap130x_device *ap130x, s32 val)
{
	return ap130x_write(ap130x, AP130X_FLICK_CTRL,
			    ap130x_flicker_values[val], NULL);
}

static int ap130x_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ap130x_device *ap130x = ctrl_to_sd(ctrl);

	switch (ctrl->id) {
	case V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE:
		return ap130x_set_wb_mode(ap130x, ctrl->val);

	case V4L2_CID_EXPOSURE:
		return ap130x_set_exposure(ap130x, ctrl->val);

	case V4L2_CID_EXPOSURE_METERING:
		return ap130x_set_exp_met(ap130x, ctrl->val);

	case V4L2_CID_GAIN:
		return ap130x_set_gain(ap130x, ctrl->val);

	case V4L2_CID_GAMMA:
		return ap130x_set_gamma(ap130x, ctrl->val);

	case V4L2_CID_CONTRAST:
		return ap130x_set_contrast(ap130x, ctrl->val);

	case V4L2_CID_BRIGHTNESS:
		return ap130x_set_brightness(ap130x, ctrl->val);

	case V4L2_CID_SATURATION:
		return ap130x_set_saturation(ap130x, ctrl->val);

	case V4L2_CID_ZOOM_ABSOLUTE:
		return ap130x_set_zoom(ap130x, ctrl->val);

	case V4L2_CID_COLORFX:
		return ap130x_set_special_effect(ap130x, ctrl->val);

	case V4L2_CID_SCENE_MODE:
		return ap130x_set_scene_mode(ap130x, ctrl->val);

	case V4L2_CID_POWER_LINE_FREQUENCY:
		return ap130x_set_flicker_freq(ap130x, ctrl->val);

	default:
		return -EINVAL;
	}
}

static const s64 ap130x_link_freqs[] = {
	445000000,
};

static int ap130x_g_volatile_ctrl(struct v4l2_ctrl *ctrl)
{
	struct ap130x_device *ap130x = ctrl_to_sd(ctrl);
	int i;
	u32 val;

	switch (ctrl->id) {
	case V4L2_CID_LINK_FREQ:
		ap130x_read(ap130x, AP130X_REG_16BIT(0x0068), &val);
		for (i = 0; i < ARRAY_SIZE(ap130x_link_freqs); i++) {
			if (ap130x_link_freqs[i] == (val / 2) * 1000000)
			break;
		}
		WARN_ON(i == ARRAY_SIZE(ap130x_link_freqs));
		__v4l2_ctrl_s_ctrl(ctrl, i);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops ap130x_ctrl_ops = {
	.s_ctrl = ap130x_s_ctrl,
	.g_volatile_ctrl = ap130x_g_volatile_ctrl,
};

static const struct v4l2_ctrl_config ap130x_ctrls[] = {
	{
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE,
		.min = 0,
		.max = 9,
		.def = 1,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_GAMMA,
		.name = "Gamma",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x0100,
		.max = 0xFFFF,
		.step = 0x100,
		.def = 0x1000,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_CONTRAST,
		.name = "Contrast",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x100,
		.max = 0xFFFF,
		.step = 0x100,
		.def = 0x100,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_BRIGHTNESS,
		.name = "Brightness",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x100,
		.max = 0xFFFF,
		.step = 0x100,
		.def = 0x100,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_SATURATION,
		.name = "Saturation",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x0100,
		.max = 0xFFFF,
		.step = 0x100,
		.def = 0x1000,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_EXPOSURE,
		.name = "Exposure",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x0,
		.max = 0xC,
		.step = 1,
		.def = 0xC,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_EXPOSURE_METERING,
		.name = "Exposure Metering",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x0,
		.max = 0x3,
		.step = 1,
		.def = 0x1,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_GAIN,
		.name = "Gain",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 0x0100,
		.max = 0xFFFF,
		.step = 0x100,
		.def = 0x100,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_ZOOM_ABSOLUTE,
		.min = 0x0100,
		.max = 0x1000,
		.step = 1,
		.def = 0x0100,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_COLORFX,
		.min = 0,
		.max = 15,
		.def = 0,
		.menu_skip_mask = BIT(15) | BIT(12) | BIT(11) | BIT(10) | BIT(9),
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_SCENE_MODE,
		.min = 0,
		.max = 13,
		.def = 0,
		.menu_skip_mask = BIT(5) | BIT(4),
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_POWER_LINE_FREQUENCY,
		.min = 0,
		.max = 3,
		.def = 3,
	}, {
		.ops = &ap130x_ctrl_ops,
		.id = V4L2_CID_LINK_FREQ,
		.min = 0,
		.max = ARRAY_SIZE(ap130x_link_freqs) - 1,
		.def = 0,
		.qmenu_int = ap130x_link_freqs,
	},
};

static int ap130x_ctrls_init(struct ap130x_device *ap130x)
{
	unsigned int i;
	int ret;

	ret = v4l2_ctrl_handler_init(&ap130x->ctrls, ARRAY_SIZE(ap130x_ctrls));
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(ap130x_ctrls); i++)
		v4l2_ctrl_new_custom(&ap130x->ctrls, &ap130x_ctrls[i], NULL);

	if (ap130x->ctrls.error) {
		ret = ap130x->ctrls.error;
		v4l2_ctrl_handler_free(&ap130x->ctrls);
		return ret;
	}

	/* Use same lock for controls as for everything else. */
	ap130x->ctrls.lock = &ap130x->lock;
	ap130x->sd.ctrl_handler = &ap130x->ctrls;

	return 0;
}

static void ap130x_ctrls_cleanup(struct ap130x_device *ap130x)
{
	v4l2_ctrl_handler_free(&ap130x->ctrls);
}

/* -----------------------------------------------------------------------------
 * V4L2 Subdev Operations
 */

static struct v4l2_mbus_framefmt *
ap130x_get_pad_format(struct ap130x_device *ap130x,
		      struct v4l2_subdev_state *state,
		      unsigned int pad, u32 which)
{
	switch (which) {
	case V4L2_SUBDEV_FORMAT_TRY:
		return v4l2_subdev_get_try_format(&ap130x->sd, state, pad);
	case V4L2_SUBDEV_FORMAT_ACTIVE:
		return &ap130x->formats[pad].format;
	default:
		return NULL;
	}
}

static int ap130x_init_cfg(struct v4l2_subdev *sd,
			   struct v4l2_subdev_state *state)
{
	u32 which = state ? V4L2_SUBDEV_FORMAT_TRY : V4L2_SUBDEV_FORMAT_ACTIVE;
	struct ap130x_device *ap130x = to_ap130x(sd);
	const struct ap130x_sensor_info *info = ap130x->sensor_info;
	unsigned int pad;

	for (pad = 0; pad < ARRAY_SIZE(ap130x->formats); ++pad) {
		struct v4l2_mbus_framefmt *format =
			ap130x_get_pad_format(ap130x, state, pad, which);

		format->width = info->resolution.width;
		format->height = info->resolution.height;

		/*
		 * The source pad combines images side by side in multi-sensor
		 * setup.
		 */
		if (pad == AP130X_PAD_SOURCE) {
			format->width *= ap130x->width_factor;
			format->code = ap130x->formats[pad].info->code;
		} else {
			format->code = info->format;
		}

		format->field = V4L2_FIELD_NONE;
		format->colorspace = V4L2_COLORSPACE_SRGB;
	}

	return 0;
}

static int ap130x_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	struct ap130x_device *ap130x = to_ap130x(sd);

	if (code->pad != AP130X_PAD_SOURCE) {
		/*
		 * On the sink pads, only the format produced by the sensor is
		 * supported.
		 */
		if (code->index)
			return -EINVAL;

		code->code = ap130x->sensor_info->format;
	} else {
		/* On the source pad, multiple formats are supported. */
		if (code->index >= ARRAY_SIZE(supported_video_formats))
			return -EINVAL;

		code->code = supported_video_formats[code->index].code;
	}

	return 0;
}

static int ap130x_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	unsigned int i;

	if (fse->index)
		return -EINVAL;

	if (fse->pad != AP130X_PAD_SOURCE) {
		/*
		 * On the sink pads, only the size produced by the sensor is
		 * supported.
		 */
		if (fse->code != ap130x->sensor_info->format)
			return -EINVAL;

		fse->min_width = ap130x->sensor_info->resolution.width;
		fse->min_height = ap130x->sensor_info->resolution.height;
		fse->max_width = ap130x->sensor_info->resolution.width;
		fse->max_height = ap130x->sensor_info->resolution.height;
	} else {
		/*
		 * On the source pad, the AP130X can freely scale within the
		 * scaler's limits.
		 */
		for (i = 0; i < ARRAY_SIZE(supported_video_formats); i++) {
			if (supported_video_formats[i].code == fse->code)
				break;
		}

		if (i >= ARRAY_SIZE(supported_video_formats))
			return -EINVAL;

#if 0
		fse->min_width = AP130X_MIN_WIDTH * ap130x->width_factor;
		fse->min_height = AP130X_MIN_HEIGHT;
		fse->max_width = AP130X_MAX_WIDTH;
		fse->max_height = AP130X_MAX_HEIGHT;
#else
		fse->min_width = ap130x->sensor_info->resolution.width;
		fse->min_height = ap130x->sensor_info->resolution.height;
		fse->max_width = ap130x->sensor_info->resolution.width;
		fse->max_height = ap130x->sensor_info->resolution.height;
#endif
	}

	return 0;
}

static int ap130x_get_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	const struct v4l2_mbus_framefmt *format;

	format = ap130x_get_pad_format(ap130x, state, fmt->pad, fmt->which);

	mutex_lock(&ap130x->lock);
	fmt->format = *format;
	mutex_unlock(&ap130x->lock);

	return 0;
}

static int ap130x_set_fmt(struct v4l2_subdev *sd,
			  struct v4l2_subdev_state *state,
			  struct v4l2_subdev_format *fmt)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	const struct ap130x_format_info *info = NULL;
	struct v4l2_mbus_framefmt *format;
	unsigned int i;

	/* Formats on the sink pads can't be changed. */
	if (fmt->pad != AP130X_PAD_SOURCE)
		return ap130x_get_fmt(sd, state, fmt);

	format = ap130x_get_pad_format(ap130x, state, fmt->pad, fmt->which);

	/* Validate the media bus code, default to the first supported value. */
	for (i = 0; i < ARRAY_SIZE(supported_video_formats); i++) {
		if (supported_video_formats[i].code == fmt->format.code) {
			info = &supported_video_formats[i];
			break;
		}
	}

	if (!info)
		info = &supported_video_formats[0];

	/*
	 * Clamp the size. The width must be a multiple of 4 (or 8 in the
	 * dual-sensor case) and the height a multiple of 2.
	 */
	fmt->format.width = clamp(ALIGN_DOWN(fmt->format.width,
					     4 * ap130x->width_factor),
				  AP130X_MIN_WIDTH * ap130x->width_factor,
				  AP130X_MAX_WIDTH);
	fmt->format.height = clamp(ALIGN_DOWN(fmt->format.height, 2),
				   AP130X_MIN_HEIGHT, AP130X_MAX_HEIGHT);

	mutex_lock(&ap130x->lock);

	format->width = fmt->format.width;
	format->height = fmt->format.height;
	format->code = info->code;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		ap130x->formats[fmt->pad].info = info;

	mutex_unlock(&ap130x->lock);

	fmt->format = *format;

	return 0;
}

static int ap130x_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	const struct ap130x_size *resolution = &ap130x->sensor_info->resolution;

	switch (sel->target) {
	case V4L2_SEL_TGT_NATIVE_SIZE:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = resolution->width * ap130x->width_factor;
		sel->r.height = resolution->height;
		break;

	default:
		return -EINVAL;
	}

	return 0;
}

static int ap130x_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	int ret = 0;

	mutex_lock(&ap130x->lock);

	if (enable == ap130x->streaming)
		goto done;

	if (enable) {
		ret = ap130x_configure(ap130x);
		if (ret < 0)
			goto done;

		ret = ap130x_stall(ap130x, false);
		if (!ret)
			ap130x->streaming = true;
	} else {
		ret = ap130x_stall(ap130x, true);
		if (!ret)
			ap130x->streaming = false;
	}

done:
	mutex_unlock(&ap130x->lock);

	if (ret < 0)
		dev_err(ap130x->dev, "Failed to %s stream: %d\n",
			enable ? "start" : "stop", ret);

	return ret;
}

static const char * const ap130x_warnings[] = {
	"HINF_BANDWIDTH",
	"FLICKER_DETECTION",
	"FACED_NE",
	"SMILED_NE",
	"HINF_OVERRUN",
	NULL,
	"FRAME_TOO_SMALL",
	"MISSING_PHASES",
	"SPOOF_UNDERRUN",
	"JPEG_NOLAST",
	"NO_IN_FREQ_SPEC",
	"SINF0",
	"SINF1",
	"CAPTURE0",
	"CAPTURE1",
	"ISR_UNHANDLED",
	"INTERLEAVE_SPOOF",
	"INTERLEAVE_BUF",
	"COORD_OUT_OF_RANGE",
	"ICP_CLOCKING",
	"SENSOR_CLOCKING",
	"SENSOR_NO_IHDR",
	"DIVIDE_BY_ZERO",
	"INT0_UNDERRUN",
	"INT1_UNDERRUN",
	"SCRATCHPAD_TOO_BIG",
	"OTP_RECORD_READ",
	"NO_LSC_IN_OTP",
	"GPIO_INT_LOST",
	"NO_PDAF_DATA",
	"FAR_PDAF_ACCESS_SKIP",
	"PDAF_ERROR",
	"ATM_TVI_BOUNDS",
	"SIPM_0_RTY",
	"SIPM_1_TRY",
	"SIPM_0_NO_ACK",
	"SIPM_1_NO_ACK",
	"SMILE_DIS",
	"DVS_DIS",
	"TEST_DIS",
	"SENSOR_LV2LV",
	"SENSOR_FV2FV",
	"FRAME_LOST",
};

static const char * const ap130x_lane_states[] = {
	"stop_s",
	"hs_req_s",
	"lp_req_s",
	"hs_s",
	"lp_s",
	"esc_req_s",
	"turn_req_s",
	"esc_s",
	"esc_0",
	"esc_1",
	"turn_s",
	"turn_mark",
	"error_s",
};

#define NUM_LANES 4
static void ap130x_log_lane_state(struct ap130x_sensor *sensor,
				  unsigned int index)
{
	static const char * const lp_states[] = {
		"00", "10", "01", "11",
	};
	unsigned int counts[NUM_LANES][ARRAY_SIZE(ap130x_lane_states)];
	unsigned int samples = 0;
	unsigned int lane;
	unsigned int i;
	u32 first[NUM_LANES] = { 0, };
	u32 last[NUM_LANES] = { 0, };
	int ret;

	memset(counts, 0, sizeof(counts));

	for (i = 0; i < 1000; ++i) {
		u32 values[NUM_LANES];

		/*
		 * Read the state of all lanes and skip read errors and invalid
		 * values.
		 */
		for (lane = 0; lane < NUM_LANES; ++lane) {
			ret = ap130x_read(sensor->ap130x,
					  AP130X_ADV_SINF_MIPI_INTERNAL_p_LANE_n_STAT(index, lane),
					  &values[lane]);
			if (ret < 0)
				break;

			if (AP130X_LANE_STATE(values[lane]) >=
			    ARRAY_SIZE(ap130x_lane_states)) {
				ret = -EINVAL;
				break;
			}
		}

		if (ret < 0)
			continue;

		/* Accumulate the samples and save the first and last states. */
		for (lane = 0; lane < NUM_LANES; ++lane)
			counts[lane][AP130X_LANE_STATE(values[lane])]++;

		if (!samples)
			memcpy(first, values, sizeof(first));
		memcpy(last, values, sizeof(last));

		samples++;
	}

	if (!samples)
		return;

	/*
	 * Print the LP state from the first sample, the error state from the
	 * last sample, and the states accumulators for each lane.
	 */
	for (lane = 0; lane < NUM_LANES; ++lane) {
		u32 state = last[lane];
		char error_msg[25] = "";

		if (state & (AP130X_LANE_ERR | AP130X_LANE_ABORT)) {
			unsigned int err = AP130X_LANE_ERR_STATE(state);
			const char *err_state = NULL;

			err_state = err < ARRAY_SIZE(ap130x_lane_states)
				  ? ap130x_lane_states[err] : "INVALID";

			snprintf(error_msg, sizeof(error_msg), "ERR (%s%s) %s LP%s",
				 state & AP130X_LANE_ERR ? "E" : "",
				 state & AP130X_LANE_ABORT ? "A" : "",
				 err_state,
				 lp_states[AP130X_LANE_ERR_LP_VAL(state)]);
		}

		dev_info(sensor->ap130x->dev, "SINF%u L%u state: LP%s %s",
			 index, lane, lp_states[AP130X_LANE_LP_VAL(first[lane])],
			 error_msg);

		for (i = 0; i < ARRAY_SIZE(ap130x_lane_states); ++i) {
			if (counts[lane][i])
				pr_cont(" %s:%u",
				       ap130x_lane_states[i],
				       counts[lane][i]);
		}
		pr_cont("\n");
	}

	/* Reset the error flags. */
	for (lane = 0; lane < NUM_LANES; ++lane)
		ap130x_write(sensor->ap130x,
			     AP130X_ADV_SINF_MIPI_INTERNAL_p_LANE_n_STAT(index, lane),
			     AP130X_LANE_ERR | AP130X_LANE_ABORT, NULL);
}

static int ap130x_log_status(struct v4l2_subdev *sd)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	u16 frame_count_icp;
	u16 frame_count_brac;
	u16 frame_count_hinf;
	u32 warning[4];
	u32 error[3];
	unsigned int i;
	u32 value;
	int ret;

	/* Dump the console buffer. */
	ret = ap130x_dump_console(ap130x);
	if (ret < 0)
		return ret;

	/* Print errors. */
	ret = ap130x_read(ap130x, AP130X_ERROR, &error[0]);
	if (ret < 0)
		return ret;

	ret = ap130x_read(ap130x, AP130X_ERR_FILE, &error[1]);
	if (ret < 0)
		return ret;

	ret = ap130x_read(ap130x, AP130X_ERR_LINE, &error[2]);
	if (ret < 0)
		return ret;

	dev_info(ap130x->dev, "ERROR: 0x%04x (file 0x%08x:%u)\n",
		 error[0], error[1], error[2]);

	ret = ap130x_read(ap130x, AP130X_SIPM_ERR_0, &error[0]);
	if (ret < 0)
		return ret;

	ret = ap130x_read(ap130x, AP130X_SIPM_ERR_1, &error[1]);
	if (ret < 0)
		return ret;

	dev_info(ap130x->dev, "SIPM_ERR [0] 0x%04x [1] 0x%04x\n",
		 error[0], error[1]);

	/* Print warnings. */
	for (i = 0; i < ARRAY_SIZE(warning); ++i) {
		ret = ap130x_read(ap130x, AP130X_WARNING(i), &warning[i]);
		if (ret < 0)
			return ret;
	}

	dev_info(ap130x->dev,
		 "WARNING [0] 0x%04x [1] 0x%04x [2] 0x%04x [3] 0x%04x\n",
		 warning[0], warning[1], warning[2], warning[3]);

	for (i = 0; i < ARRAY_SIZE(ap130x_warnings); ++i) {
		if ((warning[i / 16] & BIT(i % 16)) &&
		    ap130x_warnings[i])
			dev_info(ap130x->dev, "- WARN_%s\n",
				 ap130x_warnings[i]);
	}

	/* Print the frame counter. */
	ret = ap130x_read(ap130x, AP130X_FRAME_CNT, &value);
	if (ret < 0)
		return ret;

	frame_count_hinf = value >> 8;
	frame_count_brac = value & 0xff;

	ret = ap130x_read(ap130x, AP130X_ADV_CAPTURE_A_FV_CNT, &value);
	if (ret < 0)
		return ret;

	frame_count_icp = value & 0xffff;

	dev_info(ap130x->dev, "Frame counters: ICP %u, HINF %u, BRAC %u\n",
		 frame_count_icp, frame_count_hinf, frame_count_brac);

	/* Sample the lane state. */
	for (i = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		struct ap130x_sensor *sensor = &ap130x->sensors[i];

		if (!sensor->ap130x)
			continue;

		ap130x_log_lane_state(sensor, i);
	}

	return 0;
}

static int ap130x_subdev_registered(struct v4l2_subdev *sd)
{
	struct ap130x_device *ap130x = to_ap130x(sd);
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		struct ap130x_sensor *sensor = &ap130x->sensors[i];

		if (!sensor->dev)
			continue;

		dev_dbg(ap130x->dev, "registering sensor %u\n", i);

		ret = v4l2_device_register_subdev(sd->v4l2_dev, &sensor->sd);
		if (ret)
			return ret;

		ret = media_create_pad_link(&sensor->sd.entity, 0,
					    &sd->entity, i,
					    MEDIA_LNK_FL_IMMUTABLE |
					    MEDIA_LNK_FL_ENABLED);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct media_entity_operations ap130x_media_ops = {
	.link_validate = v4l2_subdev_link_validate
};

static const struct v4l2_subdev_pad_ops ap130x_pad_ops = {
	.init_cfg = ap130x_init_cfg,
	.enum_mbus_code = ap130x_enum_mbus_code,
	.enum_frame_size = ap130x_enum_frame_size,
	.get_fmt = ap130x_get_fmt,
	.set_fmt = ap130x_set_fmt,
	.get_selection = ap130x_get_selection,
	.set_selection = ap130x_get_selection,
};

static const struct v4l2_subdev_video_ops ap130x_video_ops = {
	.s_stream = ap130x_s_stream,
};

static const struct v4l2_subdev_core_ops ap130x_core_ops = {
	.log_status = ap130x_log_status,
};

static const struct v4l2_subdev_ops ap130x_subdev_ops = {
	.core = &ap130x_core_ops,
	.video = &ap130x_video_ops,
	.pad = &ap130x_pad_ops,
};

static const struct v4l2_subdev_internal_ops ap130x_subdev_internal_ops = {
	.registered = ap130x_subdev_registered,
};

/* -----------------------------------------------------------------------------
 * Sensor
 */

static int ap130x_sensor_enum_mbus_code(struct v4l2_subdev *sd,
					struct v4l2_subdev_state *state,
					struct v4l2_subdev_mbus_code_enum *code)
{
	struct ap130x_sensor *sensor = to_ap130x_sensor(sd);
	const struct ap130x_sensor_info *info = sensor->ap130x->sensor_info;

	if (code->index)
		return -EINVAL;

	code->code = info->format;

	return 0;
}

static int ap130x_sensor_enum_frame_size(struct v4l2_subdev *sd,
					 struct v4l2_subdev_state *state,
					 struct v4l2_subdev_frame_size_enum *fse)
{
	struct ap130x_sensor *sensor = to_ap130x_sensor(sd);
	const struct ap130x_sensor_info *info = sensor->ap130x->sensor_info;

	if (fse->index)
		return -EINVAL;

	if (fse->code != info->format)
		return -EINVAL;

	fse->min_width = info->resolution.width;
	fse->min_height = info->resolution.height;
	fse->max_width = info->resolution.width;
	fse->max_height = info->resolution.height;

	return 0;
}

static int ap130x_sensor_get_fmt(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct ap130x_sensor *sensor = to_ap130x_sensor(sd);
	const struct ap130x_sensor_info *info = sensor->ap130x->sensor_info;

	memset(&fmt->format, 0, sizeof(fmt->format));

	fmt->format.width = info->resolution.width;
	fmt->format.height = info->resolution.height;
	fmt->format.field = V4L2_FIELD_NONE;
	fmt->format.code = info->format;
	fmt->format.colorspace = V4L2_COLORSPACE_SRGB;

	return 0;
}

static const struct v4l2_subdev_pad_ops ap130x_sensor_pad_ops = {
	.enum_mbus_code = ap130x_sensor_enum_mbus_code,
	.enum_frame_size = ap130x_sensor_enum_frame_size,
	.get_fmt = ap130x_sensor_get_fmt,
	.set_fmt = ap130x_sensor_get_fmt,
};

static const struct v4l2_subdev_ops ap130x_sensor_subdev_ops = {
	.pad = &ap130x_sensor_pad_ops,
};

static int ap130x_sensor_parse_of(struct ap130x_device *ap130x,
				  struct device_node *node)
{
	struct ap130x_sensor *sensor;
	u32 reg;
	int ret;

	/* Retrieve the sensor index from the reg property. */
	ret = of_property_read_u32(node, "reg", &reg);
	if (ret < 0) {
		dev_warn(ap130x->dev,
			 "'reg' property missing in sensor node\n");
		return -EINVAL;
	}

	if (reg >= ARRAY_SIZE(ap130x->sensors)) {
		dev_warn(ap130x->dev, "Out-of-bounds 'reg' value %u\n",
			 reg);
		return -EINVAL;
	}

	sensor = &ap130x->sensors[reg];
	if (sensor->ap130x) {
		dev_warn(ap130x->dev, "Duplicate entry for sensor %u\n", reg);
		return -EINVAL;
	}

	sensor->ap130x = ap130x;
	sensor->of_node = of_node_get(node);

	return 0;
}

static void ap130x_sensor_dev_release(struct device *dev)
{
	of_node_put(dev->of_node);
	kfree(dev);
}

static int ap130x_sensor_init(struct ap130x_sensor *sensor, unsigned int index)
{
	struct ap130x_device *ap130x = sensor->ap130x;
	struct v4l2_subdev *sd = &sensor->sd;
	unsigned int i;
	int ret;

	sensor->index = index;

	/*
	 * Register a device for the sensor, to support usage of the regulator
	 * API.
	 */
	sensor->dev = kzalloc(sizeof(*sensor->dev), GFP_KERNEL);
	if (!sensor->dev)
		return -ENOMEM;

	sensor->dev->parent = ap130x->dev;
	sensor->dev->of_node = of_node_get(sensor->of_node);
	sensor->dev->release = &ap130x_sensor_dev_release;
	dev_set_name(sensor->dev, "%s-%s.%u", dev_name(ap130x->dev),
		     ap130x->sensor_info->name, index);

	ret = device_register(sensor->dev);
	if (ret < 0) {
		dev_err(ap130x->dev,
			"Failed to register device for sensor %u\n", index);
		goto error;
	}

	/* Retrieve the power supplies for the sensor, if any. */
	if (ap130x->sensor_info->supplies) {
		const struct ap130x_sensor_supply *supplies =
			ap130x->sensor_info->supplies;
		unsigned int num_supplies;

		for (num_supplies = 0; supplies[num_supplies].name;)
			++num_supplies;

		sensor->supplies = devm_kcalloc(ap130x->dev, num_supplies,
						sizeof(*sensor->supplies),
						GFP_KERNEL);
		if (!sensor->supplies) {
			ret = -ENOMEM;
			goto error;
		}

		for (i = 0; i < num_supplies; ++i)
			sensor->supplies[i].supply = supplies[i].name;

		ret = regulator_bulk_get(sensor->dev, num_supplies,
					 sensor->supplies);
		if (ret < 0) {
			dev_err(ap130x->dev,
				"Failed to get supplies for sensor %u\n",
				 index);
			goto error;
		}

		sensor->num_supplies = i;
	}

	sd->dev = sensor->dev;
	v4l2_subdev_init(sd, &ap130x_sensor_subdev_ops);

	snprintf(sd->name, sizeof(sd->name), "%s %u",
		 ap130x->sensor_info->name, index);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	sd->entity.function = MEDIA_ENT_F_CAM_SENSOR;
	sensor->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, 1, &sensor->pad);
	if (ret < 0) {
		dev_err(ap130x->dev,
			"failed to initialize media entity for sensor %u: %d\n",
			index, ret);
		goto error;
	}

	return 0;

error:
	put_device(sensor->dev);
	return ret;
}

static void ap130x_sensor_cleanup(struct ap130x_sensor *sensor)
{
	media_entity_cleanup(&sensor->sd.entity);

	if (sensor->num_supplies)
		regulator_bulk_free(sensor->num_supplies, sensor->supplies);

	put_device(sensor->dev);
	of_node_put(sensor->of_node);
}

/* -----------------------------------------------------------------------------
 * Boot & Firmware Handling
 */

static int ap130x_request_firmware(struct ap130x_device *ap130x)
{
	static const char * const suffixes[] = {
		"",
		"_single",
		"_dual",
	};

	const struct ap130x_firmware_header *fw_hdr;
	unsigned int num_sensors;
	unsigned int fw_size;
	unsigned int i;
	char name[64];
	int ret;

	for (i = 0, num_sensors = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		if (ap130x->sensors[i].dev)
			num_sensors++;
	}

	ret = snprintf(name, sizeof(name), "ap130x_%s%s_fw.bin",
		       ap130x->sensor_info->name, suffixes[num_sensors]);
	if (ret >= sizeof(name)) {
		dev_err(ap130x->dev, "Firmware name too long\n");
		return -EINVAL;
	}

	dev_dbg(ap130x->dev, "Requesting firmware %s\n", name);

	ret = request_firmware(&ap130x->fw, name, ap130x->dev);
	if (ret) {
		dev_err(ap130x->dev, "Failed to request firmware: %d\n", ret);
		return ret;
	}

	if (ap130x->fw->size < sizeof(*fw_hdr)) {
		dev_err(ap130x->dev, "Invalid firmware: too small\n");
		return -EINVAL;
	}

	/*
	 * The firmware binary contains a header defined by the
	 * ap130x_firmware_header structure. The firmware itself (also referred
	 * to as bootdata) follows the header. Perform sanity checks to ensure
	 * the firmware is valid.
	 */
	fw_hdr = (const struct ap130x_firmware_header *)ap130x->fw->data;
	fw_size = ap130x->fw->size - sizeof(*fw_hdr);

	if (fw_hdr->pll_init_size > fw_size) {
		dev_err(ap130x->dev,
			"Invalid firmware: PLL init size too large\n");
		return -EINVAL;
	}

	return 0;
}

/*
 * ap130x_write_fw_window() - Write a piece of firmware to the AP130X
 * @win_pos: Firmware load window current position
 * @buf: Firmware data buffer
 * @len: Firmware data length
 *
 * The firmware is loaded through a window in the registers space. Writes are
 * sequential starting at address 0x8000, and must wrap around when reaching
 * 0x9fff. This function write the firmware data stored in @buf to the AP130X,
 * keeping track of the window position in the @win_pos argument.
 */
static int ap130x_write_fw_window(struct ap130x_device *ap130x, const u8 *buf,
				  u32 len, unsigned int *win_pos)
{
	while (len > 0) {
		unsigned int write_addr;
		unsigned int write_size;
		int ret;

		/*
		 * Write at most len bytes, from the current position to the
		 * end of the window.
		 */
		write_addr = *win_pos + AP130X_FW_WINDOW_OFFSET;
		write_size = min(len, AP130X_FW_WINDOW_SIZE - *win_pos);

		ret = regmap_raw_write(ap130x->regmap16, write_addr, buf,
				       write_size);
		if (ret)
			return ret;

		buf += write_size;
		len -= write_size;

		*win_pos += write_size;
		if (*win_pos >= AP130X_FW_WINDOW_SIZE)
			*win_pos = 0;
	}

	return 0;
}

static int ap130x_load_firmware(struct ap130x_device *ap130x)
{
	const struct ap130x_firmware_header *fw_hdr;
	unsigned int fw_size;
	const u8 *fw_data;
	unsigned int win_pos = 0;
	int ret;

	fw_hdr = (const struct ap130x_firmware_header *)ap130x->fw->data;
	fw_data = (u8 *)&fw_hdr[1];
	fw_size = ap130x->fw->size - sizeof(*fw_hdr);

	/* Clear the CRC register. */
	ret = ap130x_write(ap130x, AP130X_SIP_CRC, 0xffff, NULL);
	if (ret)
		return ret;

	/*
	 * Load the PLL initialization settings, set the bootdata stage to 2 to
	 * apply the basic_init_hp settings, and wait 1ms for the PLL to lock.
	 */
	ret = ap130x_write_fw_window(ap130x, fw_data, fw_hdr->pll_init_size,
				     &win_pos);
	if (ret)
		return ret;

	ret = ap130x_write(ap130x, AP130X_BOOTDATA_STAGE, 0x0002, NULL);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	/* Load the rest of the bootdata content and verify the CRC. */
	ret = ap130x_write_fw_window(ap130x, fw_data + fw_hdr->pll_init_size,
				     fw_size - fw_hdr->pll_init_size, &win_pos);
	if (ret)
		return ret;

	msleep(40);

#if 0  // disable CRC check for tempory

	ret = ap130x_read(ap130x, AP130X_SIP_CRC, &crc);
	if (ret)
		return ret;

	if (crc != fw_hdr->crc) {
		dev_warn(ap130x->dev,
			 "CRC mismatch: expected 0x%04x, got 0x%04x\n",
			 fw_hdr->crc, crc);
		return -EAGAIN;
	}
#endif

	/*
	 * Write 0xffff to the bootdata_stage register to indicate to the
	 * AP130X that the whole bootdata content has been loaded.
	 */
	ret = ap130x_write(ap130x, AP130X_BOOTDATA_STAGE, 0xffff, NULL);
	if (ret)
		return ret;

	/* The AP130X starts outputting frames right after boot, stop it. */
	ret = ap130x_stall(ap130x, true);
	if (!ret)
		ap130x->streaming = false;

	return ret;
}

static int ap130x_detect_chip(struct ap130x_device *ap130x)
{
	unsigned int version;
	unsigned int revision;
	int ret;

	ret = ap130x_read(ap130x, AP130X_CHIP_VERSION, &version);
	if (ret)
		return ret;

	ret = ap130x_read(ap130x, AP130X_CHIP_REV, &revision);
	if (ret)
		return ret;

	if (version != AP130X_CHIP_ID) {
		dev_err(ap130x->dev,
			"Invalid chip version, expected 0x%04x, got 0x%04x\n",
			AP130X_CHIP_ID, version);
		return -EINVAL;
	}

	dev_info(ap130x->dev, "AP130X revision %u.%u.%u detected\n",
		 (revision & 0xf000) >> 12, (revision & 0x0f00) >> 8,
		 revision & 0x00ff);

	return 0;
}

static int ap130x_hw_init(struct ap130x_device *ap130x)
{
	unsigned int retries;
	int ret;

	/* Request and validate the firmware. */
	ret = ap130x_request_firmware(ap130x);
	if (ret)
		return ret;

	/*
	 * Power the sensors first, as the firmware will access them once it
	 * gets loaded.
	 */
	ret = ap130x_power_on_sensors(ap130x);
	if (ret < 0)
		goto error_firmware;

#define MAX_FW_LOAD_RETRIES 3
	/*
	 * Load the firmware, retrying in case of CRC errors. The AP130X is
	 * reset with a full power cycle between each attempt.
	 */
	for (retries = 0; retries < MAX_FW_LOAD_RETRIES; ++retries) {
		ret = ap130x_power_on(ap130x);
		if (ret < 0)
			goto error_power_sensors;

		ret = ap130x_detect_chip(ap130x);
		if (ret)
			goto error_power;

		ret = ap130x_load_firmware(ap130x);
		if (!ret)
			break;

		if (ret != -EAGAIN)
			goto error_power;

		ap130x_power_off(ap130x);
	}

	if (retries == MAX_FW_LOAD_RETRIES) {
		dev_err(ap130x->dev,
			"Firmware load retries exceeded, aborting\n");
		ret = -ETIMEDOUT;
		goto error_power_sensors;
	}

	return 0;

error_power:
	ap130x_power_off(ap130x);
error_power_sensors:
	ap130x_power_off_sensors(ap130x);
error_firmware:
	release_firmware(ap130x->fw);

	return ret;
}

static void ap130x_hw_cleanup(struct ap130x_device *ap130x)
{
	ap130x_power_off(ap130x);
	ap130x_power_off_sensors(ap130x);
}

/* -----------------------------------------------------------------------------
 * Probe & Remove
 */

static int ap130x_config_v4l2(struct ap130x_device *ap130x)
{
	struct v4l2_subdev *sd;
	unsigned int i;
	int ret;

	sd = &ap130x->sd;
	sd->dev = ap130x->dev;
	v4l2_i2c_subdev_init(sd, ap130x->client, &ap130x_subdev_ops);

	strscpy(sd->name, DRIVER_NAME, sizeof(sd->name));
	strlcat(sd->name, ".", sizeof(sd->name));
	strlcat(sd->name, dev_name(ap130x->dev), sizeof(sd->name));
	dev_dbg(ap130x->dev, "name %s\n", sd->name);

	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE | V4L2_SUBDEV_FL_HAS_EVENTS;
	sd->internal_ops = &ap130x_subdev_internal_ops;
	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_ISP;
	sd->entity.ops = &ap130x_media_ops;

	for (i = 0; i < ARRAY_SIZE(ap130x->pads); ++i)
		ap130x->pads[i].flags = i == AP130X_PAD_SOURCE
				      ? MEDIA_PAD_FL_SOURCE : MEDIA_PAD_FL_SINK;

	ret = media_entity_pads_init(&sd->entity, ARRAY_SIZE(ap130x->pads),
				     ap130x->pads);
	if (ret < 0) {
		dev_err(ap130x->dev, "media_entity_init failed %d\n", ret);
		return ret;
	}

	for (i = 0; i < ARRAY_SIZE(ap130x->formats); ++i)
		ap130x->formats[i].info = &supported_video_formats[0];

	ret = ap130x_init_cfg(sd, NULL);
	if (ret < 0)
		goto error_media;

	ret = ap130x_ctrls_init(ap130x);
	if (ret < 0)
		goto error_media;

	ret = v4l2_async_register_subdev(sd);
	if (ret < 0) {
		dev_err(ap130x->dev, "v4l2_async_register_subdev failed %d\n", ret);
		goto error_ctrls;
	}

	return 0;

error_ctrls:
	ap130x_ctrls_cleanup(ap130x);
error_media:
	media_entity_cleanup(&sd->entity);
	return ret;
}

static int ap130x_parse_of(struct ap130x_device *ap130x)
{
	struct device_node *sensors;
	struct device_node *node;
	struct fwnode_handle *ep;
	unsigned int num_sensors = 0;
	const char *model;
	unsigned int i;
	int ret;

	/* Clock */
	ap130x->clock = devm_clk_get(ap130x->dev, NULL);
	if (IS_ERR(ap130x->clock)) {
		dev_err(ap130x->dev, "Failed to get clock: %ld\n",
			PTR_ERR(ap130x->clock));
		return PTR_ERR(ap130x->clock);
	}

	/* GPIOs */
	ap130x->reset_gpio = devm_gpiod_get(ap130x->dev, "reset",
					    GPIOD_OUT_HIGH);
	if (IS_ERR(ap130x->reset_gpio)) {
		dev_err(ap130x->dev, "Can't get reset GPIO: %ld\n",
			PTR_ERR(ap130x->reset_gpio));
		return PTR_ERR(ap130x->reset_gpio);
	}

	ap130x->standby_gpio = devm_gpiod_get_optional(ap130x->dev, "standby",
						       GPIOD_OUT_LOW);
	if (IS_ERR(ap130x->standby_gpio)) {
		dev_err(ap130x->dev, "Can't get standby GPIO: %ld\n",
			PTR_ERR(ap130x->standby_gpio));
		return PTR_ERR(ap130x->standby_gpio);
	}

	ap130x->isp_en_gpio = devm_gpiod_get_optional(ap130x->dev, "isp_en",
						       GPIOD_OUT_HIGH);
	if (IS_ERR(ap130x->isp_en_gpio)) {
		dev_err(ap130x->dev, "Can't get ISP enable GPIO: %ld\n",
			PTR_ERR(ap130x->isp_en_gpio));
		return PTR_ERR(ap130x->isp_en_gpio);
	}
	gpiod_set_value_cansleep(ap130x->isp_en_gpio, 1);

	// has issue?
	ep = fwnode_graph_get_endpoint_by_id(dev_fwnode(ap130x->dev),
					     AP130X_PAD_SOURCE, 0,
					     FWNODE_GRAPH_ENDPOINT_NEXT);
	if (!ep) {
		dev_err(ap130x->dev, "no sink port found");
		return -EINVAL;
	}

	ap130x->bus_cfg.bus_type = V4L2_MBUS_CSI2_DPHY;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &ap130x->bus_cfg);
	if (ret < 0) {
		dev_err(ap130x->dev, "Failed to parse bus configuration\n");
		return ret;
	}

	/* Sensors */
	sensors = of_get_child_by_name(dev_of_node(ap130x->dev), "sensors");
	if (!sensors) {
		dev_err(ap130x->dev, "'sensors' child node not found\n");
		return -EINVAL;
	}

	ret = of_property_read_string(sensors, "onnn,model", &model);
	if (ret < 0) {
		/*
		 * If no sensor is connected, we can still support operation
		 * with the test pattern generator.
		 */
		ap130x->sensor_info = &ap130x_sensor_info_tpg;
		ap130x->width_factor = 1;
		ret = 0;
		goto done;
	}

	for (i = 0; i < ARRAY_SIZE(ap130x_sensor_info); ++i) {
		const struct ap130x_sensor_info *info =
			&ap130x_sensor_info[i];

		if (!strcmp(info->model, model)) {
			ap130x->sensor_info = info;
			break;
		}
	}

	if (!ap130x->sensor_info) {
		dev_warn(ap130x->dev, "Unsupported sensor model %s\n", model);
		ret = -EINVAL;
		goto done;
	}

	for_each_child_of_node(sensors, node) {
		if (of_node_name_eq(node, "sensor")) {
			if (!ap130x_sensor_parse_of(ap130x, node))
				num_sensors++;
		}
	}

	if (!num_sensors) {
		dev_err(ap130x->dev, "No sensor found\n");
		ret = -EINVAL;
		goto done;
	}

	ap130x->width_factor = num_sensors;

done:
	of_node_put(sensors);
	return ret;
}

static void ap130x_cleanup(struct ap130x_device *ap130x)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		struct ap130x_sensor *sensor = &ap130x->sensors[i];

		if (!sensor->ap130x)
			continue;

		ap130x_sensor_cleanup(sensor);
	}

	v4l2_fwnode_endpoint_free(&ap130x->bus_cfg);

	mutex_destroy(&ap130x->lock);
}

static int ap130x_probe(struct i2c_client *client)
{
	struct ap130x_device *ap130x;
	unsigned int i;
	int ret;

	ap130x = devm_kzalloc(&client->dev, sizeof(*ap130x), GFP_KERNEL);
	if (!ap130x)
		return -ENOMEM;

	ap130x->dev = &client->dev;
	ap130x->client = client;

	mutex_init(&ap130x->lock);

	ap130x->regmap16 = devm_regmap_init_i2c(client, &ap130x_reg16_config);
	if (IS_ERR(ap130x->regmap16)) {
		dev_err(ap130x->dev, "regmap16 init failed: %ld\n",
			PTR_ERR(ap130x->regmap16));
		ret = -ENODEV;
		goto error;
	}

	ap130x->regmap32 = devm_regmap_init_i2c(client, &ap130x_reg32_config);
	if (IS_ERR(ap130x->regmap32)) {
		dev_err(ap130x->dev, "regmap32 init failed: %ld\n",
			PTR_ERR(ap130x->regmap32));
		ret = -ENODEV;
		goto error;
	}

	ret = ap130x_parse_of(ap130x);
	if (ret < 0)
		goto error;

	for (i = 0; i < ARRAY_SIZE(ap130x->sensors); ++i) {
		struct ap130x_sensor *sensor = &ap130x->sensors[i];

		if (!sensor->ap130x)
			continue;

		ret = ap130x_sensor_init(sensor, i);
		if (ret < 0)
			goto error;
	}

	for (i = 0; i < AP130X_NUM_SUPPLIES; ++i)
		ap130x->supplies[i].supply = ap130x_supplies[i].name;

	ret = devm_regulator_bulk_get(&client->dev, AP130X_NUM_SUPPLIES,
				      ap130x->supplies);
	if (ret < 0)
		return ret;

	ret = ap130x_hw_init(ap130x);
	if (ret)
		goto error;

	ap130x_debugfs_init(ap130x);

	ret = ap130x_config_v4l2(ap130x);
	if (ret)
		goto error_hw_cleanup;

	dev_dbg(ap130x->dev, "%d: successfully\n", __LINE__);
	return 0;

error_hw_cleanup:
	ap130x_hw_cleanup(ap130x);
error:
	ap130x_cleanup(ap130x);
	return ret;
}

static void ap130x_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct ap130x_device *ap130x = to_ap130x(sd);

	ap130x_debugfs_cleanup(ap130x);

	ap130x_hw_cleanup(ap130x);

	release_firmware(ap130x->fw);

	v4l2_async_unregister_subdev(sd);
	media_entity_cleanup(&sd->entity);

	ap130x_ctrls_cleanup(ap130x);

	ap130x_cleanup(ap130x);
}

static const struct of_device_id ap130x_of_id_table[] = {
	{ .compatible = "onnn,ap130x" },
	{ }
};
MODULE_DEVICE_TABLE(of, ap130x_of_id_table);

static struct i2c_driver ap130x_i2c_driver = {
	.driver = {
		.name	= DRIVER_NAME,
		.of_match_table	= ap130x_of_id_table,
	},
	.probe		= ap130x_probe,
	.remove		= ap130x_remove,
};

module_i2c_driver(ap130x_i2c_driver);

MODULE_AUTHOR("Florian Rebaudo <frebaudo@witekio.com>");
MODULE_AUTHOR("Laurent Pinchart <laurent.pinchart@ideasonboard.com>");
MODULE_AUTHOR("Anil Kumar M <anil.mamidala@xilinx.com>");

MODULE_DESCRIPTION("ON Semiconductor AP130X ISP driver");
MODULE_LICENSE("GPL");
