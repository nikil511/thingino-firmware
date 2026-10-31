// SPDX-License-Identifier: GPL-2.0+
/*
 * sc2336.c
 * Copyright (C) 2012 Ingenic Semiconductor Co., Ltd.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/proc_fs.h>
#include <tx-isp-common.h>
#include <sensor-common.h>
#include <sensor-info.h>

// ============================================================================
// SENSOR IDENTIFICATION
// ============================================================================
#define SENSOR_NAME "sc2336"
#define SENSOR_VERSION "H20210805a"
#define SENSOR_CHIP_ID_H (0xcb)
#define SENSOR_CHIP_ID_L (0x3a)
#define SENSOR_CHIP_ID ((SENSOR_CHIP_ID_H << 8) | SENSOR_CHIP_ID_L)

// ============================================================================
// HARDWARE INTERFACE
// ============================================================================
#define SENSOR_BUS_TYPE TX_SENSOR_CONTROL_INTERFACE_I2C
#define SENSOR_I2C_ADDRESS 0x30

// ============================================================================
// SENSOR CAPABILITIES
// ============================================================================
#define SENSOR_MAX_WIDTH 1920
#define SENSOR_MAX_HEIGHT 1080

// ============================================================================
// REGISTER DEFINITIONS
// ============================================================================
#define SENSOR_REG_END 0xffff
#define SENSOR_REG_DELAY 0xfffe

// ============================================================================
// TIMING AND PERFORMANCE
// ============================================================================
#define SENSOR_SUPPORT_30FPS_SCLK (81000000)
#define SENSOR_OUTPUT_MAX_FPS 30
#define SENSOR_OUTPUT_MIN_FPS 5

static int reset_gpio = GPIO_PA(18);
module_param(reset_gpio, int, S_IRUGO);
MODULE_PARM_DESC(reset_gpio, "Reset GPIO NUM");

static int pwdn_gpio = -1;
module_param(pwdn_gpio, int, S_IRUGO);
MODULE_PARM_DESC(pwdn_gpio, "Power down GPIO NUM");

static int data_interface = TX_SENSOR_DATA_INTERFACE_MIPI;
module_param(data_interface, int, S_IRUGO);
MODULE_PARM_DESC(data_interface, "Sensor Date interface");

static int shvflip = 0;
module_param(shvflip, int, S_IRUGO);
MODULE_PARM_DESC(shvflip, "Sensor HV Flip Enable interface");

/*
 * Sensor readout mode, chosen at load time (/etc/modules.d/30-sensor):
 *   0  1920x1080 full array, HTS 2250, 25 fps default, 30 max (stock)
 *   1  1280x720  centre crop (2/3 FOV), HTS 1800 VTS 750, 60 fps (published)
 *   2-7          EXPERIMENTAL derived crops/strips/binning, see sensor_modes[]
 * All modes share the stock PLL: 24 MHz MCLK, 2 lanes x 405 Mbps RAW10,
 * 81 MHz pixel clock.  fps = 81e6 / (HTS * VTS).
 */
static int sensor_mode = 0;
module_param(sensor_mode, int, S_IRUGO);
MODULE_PARM_DESC(sensor_mode, "Readout mode: 0=1080p25-32 1=720p60 2=1920x540@60-63 3=960x540@75-78 4=640x480@75-88 5=640x360@90-115 6=1920x360@90-92 7=960x540bin@60 (2-7 experimental)");

static struct sensor_info sensor_info = {
    .name = SENSOR_NAME,
    .chip_id = SENSOR_CHIP_ID,
    .version = SENSOR_VERSION,
    .min_fps = SENSOR_OUTPUT_MIN_FPS,
    .max_fps = SENSOR_OUTPUT_MAX_FPS,
    .actual_fps = 0,
    .chip_i2c_addr = SENSOR_I2C_ADDRESS,
    .width = SENSOR_MAX_WIDTH,
    .height = SENSOR_MAX_HEIGHT,
};

struct regval_list {
    uint16_t reg_num;
    unsigned char value;
};

struct again_lut {
    unsigned int value;
    unsigned int gain;
};

struct again_lut sensor_again_lut[] = {
    //cnt_gain = 161 cnt_reg = 161
    {0x80, 0},
    {0x84, 2886},
    {0x88, 5776},
    {0x8c, 8494},
    {0x90, 11136},
    {0x94, 13706},
    {0x98, 16287},
    {0x9c, 18723},
    {0xa0, 21097},
    {0xa4, 23414},
    {0xa8, 25746},
    {0xac, 27953},
    {0xb0, 30109},
    {0xb4, 32217},
    {0xb8, 34345},
    {0xbc, 36361},
    {0xc0, 38336},
    {0xc4, 40270},
    {0xc8, 42226},
    {0xcc, 44082},
    {0xd0, 45904},
    {0xd4, 47690},
    {0xd8, 49500},
    {0xdc, 51220},
    {0xe0, 52910},
    {0xe4, 54571},
    {0xe8, 56254},
    {0xec, 57857},
    {0xf0, 59433},
    {0xf4, 60984},
    {0xf8, 62558},
    {0xfc, 64059},
    {0x880, 65536},
    {0x884, 68422},
    {0x888, 71312},
    {0x88c, 74030},
    {0x890, 76672},
    {0x894, 79242},
    {0x898, 81823},
    {0x89c, 84259},
    {0x8a0, 86633},
    {0x8a4, 88950},
    {0x8a8, 91282},
    {0x8ac, 93489},
    {0x8b0, 95645},
    {0x8b4, 97753},
    {0x8b8, 99881},
    {0x8bc, 101897},
    {0x8c0, 103872},
    {0x8c4, 105806},
    {0x8c8, 107762},
    {0x8cc, 109618},
    {0x8d0, 111440},
    {0x8d4, 113226},
    {0x8d8, 115036},
    {0x8dc, 116756},
    {0x8e0, 118446},
    {0x8e4, 120107},
    {0x8e8, 121790},
    {0x8ec, 123393},
    {0x8f0, 124969},
    {0x8f4, 126520},
    {0x8f8, 128094},
    {0x8fc, 129595},
    {0x980, 131072},
    {0x984, 133958},
    {0x988, 136848},
    {0x98c, 139566},
    {0x990, 142208},
    {0x994, 144778},
    {0x998, 147359},
    {0x99c, 149795},
    {0x9a0, 152169},
    {0x9a4, 154486},
    {0x9a8, 156818},
    {0x9ac, 159025},
    {0x9b0, 161181},
    {0x9b4, 163289},
    {0x9b8, 165417},
    {0x9bc, 167433},
    {0x9c0, 169408},
    {0x9c4, 171342},
    {0x9c8, 173298},
    {0x9cc, 175154},
    {0x9d0, 176976},
    {0x9d4, 178762},
    {0x9d8, 180572},
    {0x9dc, 182292},
    {0x9e0, 183982},
    {0x9e4, 185643},
    {0x9e8, 187326},
    {0x9ec, 188929},
    {0x9f0, 190505},
    {0x9f4, 192056},
    {0x9f8, 193630},
    {0x9fc, 195131},
    {0xb80, 196608},
    {0xb84, 199494},
    {0xb88, 202384},
    {0xb8c, 205102},
    {0xb90, 207744},
    {0xb94, 210314},
    {0xb98, 212895},
    {0xb9c, 215331},
    {0xba0, 217705},
    {0xba4, 220022},
    {0xba8, 222354},
    {0xbac, 224561},
    {0xbb0, 226717},
    {0xbb4, 228825},
    {0xbb8, 230953},
    {0xbbc, 232969},
    {0xbc0, 234944},
    {0xbc4, 236878},
    {0xbc8, 238834},
    {0xbcc, 240690},
    {0xbd0, 242512},
    {0xbd4, 244298},
    {0xbd8, 246108},
    {0xbdc, 247828},
    {0xbe0, 249518},
    {0xbe4, 251179},
    {0xbe8, 252862},
    {0xbec, 254465},
    {0xbf0, 256041},
    {0xbf4, 257592},
    {0xbf8, 259166},
    {0xbfc, 260667},
    {0xf80, 262144},
    {0xf84, 265030},
    {0xf88, 267920},
    {0xf8c, 270638},
    {0xf90, 273280},
    {0xf94, 275850},
    {0xf98, 278431},
    {0xf9c, 280867},
    {0xfa0, 283241},
    {0xfa4, 285558},
    {0xfa8, 287890},
    {0xfac, 290097},
    {0xfb0, 292253},
    {0xfb4, 294361},
    {0xfb8, 296489},
    {0xfbc, 298505},
    {0xfc0, 300480},
    {0xfc4, 302414},
    {0xfc8, 304370},
    {0xfcc, 306226},
    {0xfd0, 308048},
    {0xfd4, 309834},
    {0xfd8, 311644},
    {0xfdc, 313364},
    {0xfe0, 315054},
    {0xfe4, 316715},
    {0xfe8, 318398},
    {0xfec, 320001},
    {0xff0, 321577},
    {0xff4, 323128},
    {0xff8, 324702},
    {0xffc, 326203},
    {0x1f80, 327680},
};

struct tx_isp_sensor_attribute sensor_attr;

unsigned int sensor_alloc_again(unsigned int isp_gain, unsigned char shift, unsigned int *sensor_again)
{
    struct again_lut *lut = sensor_again_lut;
    while (lut->gain <= sensor_attr.max_again) {
        if (isp_gain == 0) {
            *sensor_again = lut[0].value;
            return 0;
        } else if (isp_gain < lut->gain) {
            *sensor_again = (lut - 1)->value;
            return (lut - 1)->gain;
        } else {
            if ((lut->gain == sensor_attr.max_again) && (isp_gain >= lut->gain)) {
                *sensor_again = lut->value;
                return lut->gain;
            }
        }

        lut++;
    }

    return isp_gain;
}

unsigned int sensor_alloc_dgain(unsigned int isp_gain, unsigned char shift, unsigned int *sensor_dgain)
{
    return 0;
}

struct tx_isp_sensor_attribute sensor_attr = {
    .name = SENSOR_NAME,
    .chip_id = SENSOR_CHIP_ID,
    .cbus_type = SENSOR_BUS_TYPE,
    .cbus_mask = V4L2_SBUS_MASK_SAMPLE_8BITS | V4L2_SBUS_MASK_ADDR_16BITS,
    .cbus_device = SENSOR_I2C_ADDRESS,
    .dbus_type = TX_SENSOR_DATA_INTERFACE_MIPI,
    .mipi = {
        .mode = SENSOR_MIPI_OTHER_MODE,
        .clk = 405,
        .lans = 2,
        .settle_time_apative_en = 1,
        .mipi_sc.sensor_csi_fmt = TX_SENSOR_RAW10,
        .mipi_sc.hcrop_diff_en = 0,
        .mipi_sc.mipi_vcomp_en = 0,
        .mipi_sc.mipi_hcomp_en = 0,
        .mipi_sc.line_sync_mode = 0,
        .mipi_sc.work_start_flag = 0,
        .image_twidth = 1920,
        .image_theight = 1080,
        .mipi_sc.mipi_crop_start0x = 0,
        .mipi_sc.mipi_crop_start0y = 0,
        .mipi_sc.mipi_crop_start1x = 0,
        .mipi_sc.mipi_crop_start1y = 0,
        .mipi_sc.mipi_crop_start2x = 0,
        .mipi_sc.mipi_crop_start2y = 0,
        .mipi_sc.mipi_crop_start3x = 0,
        .mipi_sc.mipi_crop_start3y = 0,
        .mipi_sc.data_type_en = 0,
        .mipi_sc.data_type_value = RAW10,
        .mipi_sc.del_start = 0,
        .mipi_sc.sensor_frame_mode = TX_SENSOR_DEFAULT_FRAME_MODE,
        .mipi_sc.sensor_fid_mode = 0,
        .mipi_sc.sensor_mode = TX_SENSOR_DEFAULT_MODE,
    },
    .data_type = TX_SENSOR_DATA_TYPE_LINEAR,
    .max_again = 327680,
    .max_dgain = 0,
    .min_integration_time = 2,
    .min_integration_time_native = 2,
    .max_integration_time_native = 0x5a0 - 4,
    .integration_time_limit = 0x5a0 - 4,
    .total_width = 0x8ca,
    .total_height = 0x5a0,
    .max_integration_time = 0x5a0 - 4,
    .one_line_expr_in_us = 28,
    .integration_time_apply_delay = 2,
    .again_apply_delay = 2,
    .dgain_apply_delay = 0,
    .sensor_ctrl.alloc_again = sensor_alloc_again,
    .sensor_ctrl.alloc_dgain = sensor_alloc_dgain,
};

static struct regval_list sensor_init_regs_1920_1080_30fps_mipi[] = {
    /*
     * cleaned 0x02 24Mmclk 2lane 405Mbps 10bit 1080p@30fps
     */
    {0x0103, 0x01},
    {0x0100, 0x00},
    {0x36e9, 0x80},
    {0x37f9, 0x80},
    {0x301f, 0x02},
    {0x3106, 0x05},
    {0x320c, 0x08},
    {0x320d, 0xca},
    {0x320e, 0x05},
    {0x320f, 0xa0},
    {0x3248, 0x04},
    {0x3249, 0x0b},
    {0x3253, 0x08},
    {0x3301, 0x09},
    {0x3302, 0xff},
    {0x3303, 0x10},
    {0x3306, 0x60},
    {0x3307, 0x02},
    {0x330a, 0x01},
    {0x330b, 0x10},
    {0x330c, 0x16},
    {0x330d, 0x90},
    {0x3318, 0x02},
    {0x3321, 0x0a},
    {0x3327, 0x0e},
    {0x332b, 0x12},
    {0x3333, 0x10},
    {0x3334, 0x40},
    {0x335e, 0x06},
    {0x335f, 0x0a},
    {0x3364, 0x1f},
    {0x337c, 0x02},
    {0x337d, 0x0e},
    {0x3390, 0x09},
    {0x3391, 0x0f},
    {0x3392, 0x1f},
    {0x3393, 0x20},
    {0x3394, 0x20},
    {0x3395, 0xff},
    {0x33a2, 0x04},
    {0x33b1, 0x80},
    {0x33b2, 0x68},
    {0x33b3, 0x42},
    {0x33f9, 0x70},
    {0x33fb, 0xd0},
    {0x33fc, 0x0f},
    {0x33fd, 0x1f},
    {0x349f, 0x03},
    {0x34a6, 0x0f},
    {0x34a7, 0x1f},
    {0x34a8, 0x42},
    {0x34a9, 0x06},
    {0x34aa, 0x01},
    {0x34ab, 0x23},
    {0x34ac, 0x01},
    {0x34ad, 0x84},
    {0x3630, 0xf4},
    {0x3633, 0x22},
    {0x3639, 0xf4},
    {0x363c, 0x47},
    {0x3670, 0x09},
    {0x3674, 0xf8},
    {0x3675, 0xfb},
    {0x3676, 0xed},
    {0x367c, 0x09},
    {0x367d, 0x0f},
    {0x3690, 0x22},
    {0x3691, 0x32},
    {0x3692, 0x33},
    {0x3698, 0x89},
    {0x3699, 0x95},
    {0x369a, 0xbb},
    {0x369b, 0xbb},
    {0x369c, 0x09},
    {0x369d, 0x0f},
    {0x36a2, 0x09},
    {0x36a3, 0x0f},
    {0x36a4, 0x1f},
    {0x36d0, 0x01},
    {0x36ea, 0x09},
    {0x36eb, 0x0c},
    {0x36ec, 0x1c},
    {0x36ed, 0x28},
    {0x3722, 0xe1},
    {0x3724, 0x41},
    {0x3725, 0xc1},
    {0x3728, 0x20},
    {0x37fa, 0x09},
    {0x37fb, 0x32},
    {0x37fc, 0x11},
    {0x37fd, 0x37},
    {0x3900, 0x0d},
    {0x3905, 0x98},
    {0x391b, 0x81},
    {0x391c, 0x10},
    {0x3933, 0x81},
    {0x3934, 0xc9},
    {0x3940, 0x6d},
    {0x3942, 0x01},
    {0x3943, 0xcb},
    {0x3952, 0x02},
    {0x3953, 0x0f},
    {0x3e01, 0x4a},
    {0x3e02, 0xb0},
    {0x3e08, 0x1f},
    {0x3e1b, 0x14},
    {0x440e, 0x02},
    {0x4509, 0x38},
    {0x4819, 0x06},
    {0x481b, 0x03},
    {0x481d, 0x0b},
    {0x481f, 0x03},
    {0x4821, 0x08},
    {0x4823, 0x03},
    {0x4825, 0x03},
    {0x4827, 0x03},
    {0x4829, 0x05},
    {0x5799, 0x06},
    {0x5ae0, 0xfe},
    {0x5ae1, 0x40},
    {0x5ae2, 0x30},
    {0x5ae3, 0x28},
    {0x5ae4, 0x20},
    {0x5ae5, 0x30},
    {0x5ae6, 0x28},
    {0x5ae7, 0x20},
    {0x5ae8, 0x3c},
    {0x5ae9, 0x30},
    {0x5aea, 0x28},
    {0x5aeb, 0x3c},
    {0x5aec, 0x30},
    {0x5aed, 0x28},
    {0x5aee, 0xfe},
    {0x5aef, 0x40},
    {0x5af4, 0x30},
    {0x5af5, 0x28},
    {0x5af6, 0x20},
    {0x5af7, 0x30},
    {0x5af8, 0x28},
    {0x5af9, 0x20},
    {0x5afa, 0x3c},
    {0x5afb, 0x30},
    {0x5afc, 0x28},
    {0x5afd, 0x3c},
    {0x5afe, 0x30},
    {0x5aff, 0x28},
    {0x36e9, 0x53},
    {0x37f9, 0x53},
    {0x0100, 0x01},
    {SENSOR_REG_END, 0x00},
};

/*
 * 1280x720@60, 2 lanes x 405 Mbps RAW10, 24 MHz MCLK.  Ported from
 * espressif/esp-video-components esp_cam_sensor/sensors/sc2336/
 * private_include/sc2336_mipi_2lane_24Minput_1280x720_raw10_60fps.h
 * (Apache-2.0).  Same PLL as the 1080p table; window 0x3200-0x3207 =
 * x 308..1619, y 180..907 of the array, i.e. a centre crop, not binning.
 * HTS 0x0708 = 1800, VTS 0x02ee = 750 -> 81e6 / (1800 * 750) = 60.0 fps.
 */
static struct regval_list sensor_init_regs_1280_720_60fps_mipi[] = {
	{0x0103, 0x01},
	{0x0100, 0x00},
	{0x36e9, 0x80},
	{0x37f9, 0x80},
	{0x301f, 0x30},
	{0x3106, 0x05},
	{0x3200, 0x01},
	{0x3201, 0x34},
	{0x3202, 0x00},
	{0x3203, 0xb4},
	{0x3204, 0x06},
	{0x3205, 0x53},
	{0x3206, 0x03},
	{0x3207, 0x8b},
	{0x3208, 0x05},
	{0x3209, 0x00},
	{0x320a, 0x02},
	{0x320b, 0xd0},
	{0x320c, 0x07},
	{0x320d, 0x08},
	{0x320e, 0x02},
	{0x320f, 0xee},
	{0x3210, 0x00},
	{0x3211, 0x10},
	{0x3212, 0x00},
	{0x3213, 0x04},
	{0x3248, 0x04},
	{0x3249, 0x0b},
	{0x3253, 0x08},
	{0x3301, 0x09},
	{0x3302, 0xff},
	{0x3303, 0x10},
	{0x3306, 0x60},
	{0x3307, 0x02},
	{0x330a, 0x01},
	{0x330b, 0x10},
	{0x330c, 0x16},
	{0x330d, 0xff},
	{0x3318, 0x02},
	{0x3321, 0x0a},
	{0x3327, 0x0e},
	{0x332b, 0x12},
	{0x3333, 0x10},
	{0x3334, 0x40},
	{0x335e, 0x06},
	{0x335f, 0x0a},
	{0x3364, 0x1f},
	{0x337c, 0x02},
	{0x337d, 0x0e},
	{0x3390, 0x09},
	{0x3391, 0x0f},
	{0x3392, 0x1f},
	{0x3393, 0x20},
	{0x3394, 0x20},
	{0x3395, 0x30},
	{0x33a2, 0x04},
	{0x33b1, 0x80},
	{0x33b2, 0x68},
	{0x33b3, 0x42},
	{0x33f9, 0x70},
	{0x33fb, 0xd0},
	{0x33fc, 0x0f},
	{0x33fd, 0x1f},
	{0x349f, 0x03},
	{0x34a6, 0x0f},
	{0x34a7, 0x1f},
	{0x34a8, 0x42},
	{0x34a9, 0x06},
	{0x34aa, 0x01},
	{0x34ab, 0x23},
	{0x34ac, 0x01},
	{0x34ad, 0x84},
	{0x3630, 0xf4},
	{0x3633, 0x22},
	{0x3639, 0xf4},
	{0x363c, 0x47},
	{0x3641, 0x03},
	{0x3670, 0x09},
	{0x3674, 0xf4},
	{0x3675, 0xfb},
	{0x3676, 0xed},
	{0x367c, 0x09},
	{0x367d, 0x0f},
	{0x3690, 0x33},
	{0x3691, 0x33},
	{0x3692, 0x43},
	{0x3698, 0x89},
	{0x3699, 0x96},
	{0x369a, 0xd0},
	{0x369b, 0xd0},
	{0x369c, 0x09},
	{0x369d, 0x0f},
	{0x36a2, 0x09},
	{0x36a3, 0x0f},
	{0x36a4, 0x1f},
	{0x36d0, 0x01},
	{0x36ea, 0x09},
	{0x36eb, 0x0c},
	{0x36ec, 0x1c},
	{0x36ed, 0x28},
	{0x3722, 0xe1},
	{0x3724, 0x41},
	{0x3725, 0xc1},
	{0x3728, 0x20},
	{0x37fa, 0x09},
	{0x37fb, 0x32},
	{0x37fc, 0x11},
	{0x37fd, 0x37},
	{0x3900, 0x0d},
	{0x3905, 0x98},
	{0x391b, 0x81},
	{0x391c, 0x10},
	{0x3933, 0x81},
	{0x3934, 0xc5},
	{0x3940, 0x68},
	{0x3941, 0x00},
	{0x3942, 0x01},
	{0x3943, 0xc6},
	{0x3952, 0x02},
	{0x3953, 0x0f},
	{0x3e01, 0x37},
	{0x3e02, 0xe0},
	{0x3e08, 0x1f},
	{0x3e1b, 0x14},
	{0x440e, 0x02},
	{0x4509, 0x38},
	{0x4819, 0x06},
	{0x481b, 0x03},
	{0x481d, 0x0b},
	{0x481f, 0x03},
	{0x4821, 0x08},
	{0x4823, 0x03},
	{0x4825, 0x03},
	{0x4827, 0x03},
	{0x4829, 0x05},
	{0x5799, 0x06},
	{0x5ae0, 0xfe},
	{0x5ae1, 0x40},
	{0x5ae2, 0x30},
	{0x5ae3, 0x28},
	{0x5ae4, 0x20},
	{0x5ae5, 0x30},
	{0x5ae6, 0x28},
	{0x5ae7, 0x20},
	{0x5ae8, 0x3c},
	{0x5ae9, 0x30},
	{0x5aea, 0x28},
	{0x5aeb, 0x3c},
	{0x5aec, 0x30},
	{0x5aed, 0x28},
	{0x5aee, 0xfe},
	{0x5aef, 0x40},
	{0x5af4, 0x30},
	{0x5af5, 0x28},
	{0x5af6, 0x20},
	{0x5af7, 0x30},
	{0x5af8, 0x28},
	{0x5af9, 0x20},
	{0x5afa, 0x3c},
	{0x5afb, 0x30},
	{0x5afc, 0x28},
	{0x5afd, 0x3c},
	{0x5afe, 0x30},
	{0x5aff, 0x28},
	{0x36e9, 0x53},
	{0x37f9, 0x53},
	{0x0100, 0x01},
	{SENSOR_REG_END, 0x00},
};

static struct tx_isp_sensor_win_setting sensor_win_sizes[] = {
	{
		.width = 1920,
		.height = 1080,
		.fps = 25 << 16 | 1,
		.mbus_code = V4L2_MBUS_FMT_SBGGR10_1X10,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.regs = sensor_init_regs_1920_1080_30fps_mipi,
	},
	{
		.width = 1280,
		.height = 720,
		.fps = 60 << 16 | 1,
		.mbus_code = V4L2_MBUS_FMT_SBGGR10_1X10,
		.colorspace = V4L2_COLORSPACE_SRGB,
		.regs = sensor_init_regs_1280_720_60fps_mipi,
	},
};

/*
 * Per-mode timing.  Modes 0 and 1 are published tables used verbatim.
 * Modes >= 2 are EXPERIMENTAL derived crops: a published base table
 * (720p60 for HTS 1800 crops, 1080p for full-width strips, same PLL) is
 * written without its stream-on, then sensor_write_window() programs the
 * window, output size, HTS and VTS.  Window geometry follows the
 * published SC2336 crops: the array window is (W+32) x (H+8), centred
 * in a 1928 x 1088 array, with output offsets x=16, y=4 (x=4 for the
 * full 1920 width, which uses the whole array width).
 * No binning/skipping exists in any published SC2336 table, so every
 * mode other than 0 narrows the field of view.
 *
 * min_vts = active rows + 30: measured on a T23N, the ISP stops delivering
 * frames (or delivers noise) below roughly 23-31 lines of vertical
 * blanking; 30 is the smallest value seen working (720p60, VTS 750).
 * max_fps follows from min_vts at 81 MHz.  Mode 7's bin/skip recipe
 * produces a corrupted image on SC2336 (kept for further research).
 */
struct sensor_mode_timing {
	unsigned int width, height;
	unsigned int hts;        /* 0x320c/0x320d */
	unsigned int vts;        /* 0x320e/0x320f at the default fps */
	unsigned int min_vts;    /* active lines + minimum vertical blanking */
	unsigned int max_fps;
	unsigned int expo_margin;
	unsigned int sclk;       /* pixel clock in Hz */
	int base;                /* sensor_win_sizes[] entry holding the base table */
	int derived;             /* window/HTS/VTS programmed after the base table */
	int bin;                 /* 2x2 bin/skip: full array window, output W x H */
};

#define SC2336_SCLK 81000000
#define SC2336_ARRAY_W 1928
#define SC2336_ARRAY_H 1088

static const struct sensor_mode_timing sensor_modes[] = {
	/*
	 * 0: stock 1080p, full FOV, 25 fps default, 32 max (stock: 30).  Exposure margin 6, not the
	 * stock 4: at VTS-4 the SC2336 stretches the frame (measured 33.50 ms instead
	 * of 33.33 at 30 fps), at VTS-6 it holds the period.
	 */
	{ 1920, 1080, 2250, 1440, 1080 + 30, 32, 6, SC2336_SCLK, 0, 0 },
	/* 1: published 720p60 centre crop (2/3 FOV) */
	{ 1280, 720, 1800, 750, 720 + 30, 60, 6, SC2336_SCLK, 1, 0 },
	/* 2: EXPERIMENTAL full-width strip 1920x540: full H FOV, 1/2 V FOV, 60 fps (max 63) */
	{ 1920, 540, 2250, 600, 540 + 30, 63, 6, SC2336_SCLK, 0, 1 },
	/* 3: EXPERIMENTAL 960x540 centre crop (1/2 FOV), 75 fps (max 78) */
	{ 960, 540, 1800, 600, 540 + 30, 78, 6, SC2336_SCLK, 1, 1 },
	/* 4: EXPERIMENTAL 640x480 centre crop (1/3 x 4/9 FOV), 75 fps (max 88) */
	{ 640, 480, 1800, 600, 480 + 30, 88, 6, SC2336_SCLK, 1, 1 },
	/* 5: EXPERIMENTAL 640x360 centre crop (1/3 FOV), 90 fps (max 115) */
	{ 640, 360, 1800, 500, 360 + 30, 115, 6, SC2336_SCLK, 1, 1 },
	/* 6: EXPERIMENTAL full-width strip 1920x360: full H FOV, 1/3 V FOV, 90 fps (max 92) */
	{ 1920, 360, 2250, 400, 360 + 30, 92, 6, SC2336_SCLK, 0, 1 },
	/*
	 * 7: EXPERIMENTAL 2x2 bin/skip 960x540 over the full array (full FOV), 60 fps (max 65).
	 * Uses the SmartSens bin registers from the SC200AI 960x540@120 table
	 * (0x3215/0x3220/0x5000/0x5900/0x5901); unpublished for SC2336, may not work.
	 */
	{ 960, 540, 2250, 600, 540 + 30, 63, 6, SC2336_SCLK, 0, 1, 1 },
};

#define SENSOR_NUM_MODES ARRAY_SIZE(sensor_modes)

static const struct sensor_mode_timing *mtiming = &sensor_modes[0];

static struct tx_isp_sensor_win_setting mode_win;

struct tx_isp_sensor_win_setting *wsize = &sensor_win_sizes[0];

/* point wsize/mtiming at the selected mode and patch the static attributes */
static void sensor_apply_mode(void) {
	if (sensor_mode < 0 || sensor_mode >= SENSOR_NUM_MODES) {
		ISP_ERROR("%s: invalid sensor_mode %d, using 0\n", SENSOR_NAME, sensor_mode);
		sensor_mode = 0;
	}

	mtiming = &sensor_modes[sensor_mode];
	if (mtiming->derived) {
		mode_win = sensor_win_sizes[mtiming->base];
		mode_win.width = mtiming->width;
		mode_win.height = mtiming->height;
		mode_win.fps = (mtiming->sclk / (mtiming->hts * mtiming->vts)) << 16 | 1;
		wsize = &mode_win;
	} else {
		wsize = &sensor_win_sizes[mtiming->base];
	}

	sensor_info.width = wsize->width;
	sensor_info.height = wsize->height;
	sensor_info.max_fps = mtiming->max_fps;

	sensor_attr.mipi.image_twidth = wsize->width;
	sensor_attr.mipi.image_theight = wsize->height;
	sensor_attr.total_width = mtiming->hts;
	sensor_attr.total_height = mtiming->vts;
	sensor_attr.max_integration_time_native = mtiming->vts - mtiming->expo_margin;
	sensor_attr.integration_time_limit = mtiming->vts - mtiming->expo_margin;
	sensor_attr.max_integration_time = mtiming->vts - mtiming->expo_margin;
	sensor_attr.one_line_expr_in_us = (mtiming->hts * 1000 + mtiming->sclk / 2000) / (mtiming->sclk / 1000);

	ISP_WARNING("%s mode %d%s: %dx%d hts %u vts %u max %u fps\n", SENSOR_NAME, sensor_mode,
		mtiming->derived ? " (EXPERIMENTAL)" : "", wsize->width, wsize->height,
		mtiming->hts, mtiming->vts, mtiming->max_fps);
}

int sensor_write(struct tx_isp_subdev *sd, uint16_t reg, unsigned char value);

static int sensor_write16(struct tx_isp_subdev *sd, uint16_t reg, unsigned int value) {
	int ret = sensor_write(sd, reg, (value >> 8) & 0xff);

	return ret ? ret : sensor_write(sd, reg + 1, value & 0xff);
}

/* program window, output size and frame timing of a derived mode (stream must be off) */
static int sensor_write_window(struct tx_isp_subdev *sd) {
	unsigned int w = mtiming->width, h = mtiming->height;
	unsigned int aw, xs, xoff, ys = (SC2336_ARRAY_H - (h + 8)) / 2;
	int ret = 0;

	if (mtiming->bin) {
		/* full array in, binned 960x540 out */
		aw = SC2336_ARRAY_W;
		xs = 0;
		xoff = 2;
		ys = 0;
		h = SC2336_ARRAY_H - 8; /* window rows; output height written below */
	} else if (w + 32 > SC2336_ARRAY_W) {
		/* full width: whole array, centred output */
		aw = SC2336_ARRAY_W;
		xs = 0;
		xoff = (SC2336_ARRAY_W - w) / 2;
	} else {
		aw = w + 32;
		xs = (SC2336_ARRAY_W - aw) / 2;
		xoff = 16;
	}

	ret |= sensor_write16(sd, 0x3200, xs);
	ret |= sensor_write16(sd, 0x3202, ys);
	ret |= sensor_write16(sd, 0x3204, xs + aw - 1);
	ret |= sensor_write16(sd, 0x3206, ys + h + 8 - 1);
	ret |= sensor_write16(sd, 0x3208, w);
	ret |= sensor_write16(sd, 0x320a, mtiming->height);
	ret |= sensor_write16(sd, 0x320c, mtiming->hts);
	ret |= sensor_write16(sd, 0x320e, mtiming->vts);
	ret |= sensor_write16(sd, 0x3210, xoff);
	ret |= sensor_write16(sd, 0x3212, mtiming->bin ? 2 : 4);
	if (mtiming->bin) {
		/* SC200AI-style analog skip + digital bin (vertical and horizontal 2x) */
		ret |= sensor_write(sd, 0x3215, 0x31);
		ret |= sensor_write(sd, 0x3220, 0x17);
		ret |= sensor_write(sd, 0x5000, 0x46);
		ret |= sensor_write(sd, 0x5900, 0xf1);
		ret |= sensor_write(sd, 0x5901, 0x04);
	}
	/* initial exposure inside the short frame; AE takes over afterwards */
	ret |= sensor_write(sd, 0x3e00, 0x00);
	ret |= sensor_write16(sd, 0x3e01, ((mtiming->vts - mtiming->expo_margin) / 2) << 4);

	ISP_WARNING("%s window x %u..%u y %u..%u out %ux%u off %u hts %u vts %u%s\n", SENSOR_NAME,
		xs, xs + aw - 1, ys, ys + h + 7, w, mtiming->height, xoff, mtiming->hts, mtiming->vts,
		mtiming->bin ? " bin2x2" : "");

	return ret;
}


static struct regval_list sensor_stream_on_mipi[] = {
    {0x0100, 0x01},
    {SENSOR_REG_END, 0x00},
};

static struct regval_list sensor_stream_off_mipi[] = {
    {0x0100, 0x00},
    {SENSOR_REG_END, 0x00},
};

int sensor_read(struct tx_isp_subdev *sd, uint16_t reg, unsigned char *value)
{
    struct i2c_client *client = tx_isp_get_subdevdata(sd);
    unsigned char buf[2] = {reg >> 8, reg & 0xff};
    struct i2c_msg msg[2] = {
        [0] = {
            .addr = client->addr,
            .flags = 0,
            .len = 2,
            .buf = buf,
        },
        [1] = {
            .addr = client->addr,
            .flags = I2C_M_RD,
            .len = 1,
            .buf = value,
        }
    };
    int ret;
    ret = private_i2c_transfer(client->adapter, msg, 2);
    if (ret > 0) {
        ret = 0;
    }

    return ret;
}

int sensor_write(struct tx_isp_subdev *sd, uint16_t reg, unsigned char value)
{
    struct i2c_client *client = tx_isp_get_subdevdata(sd);
    uint8_t buf[3] = {(reg >> 8) & 0xff, reg & 0xff, value};
    struct i2c_msg msg = {
        .addr = client->addr,
        .flags = 0,
        .len = 3,
        .buf = buf,
    };
    int ret;
    ret = private_i2c_transfer(client->adapter, &msg, 1);
    if (ret > 0) {
        ret = 0;
    }

    return ret;
}

static int sensor_read_array(struct tx_isp_subdev *sd, struct regval_list *vals)
{
    int ret;
    unsigned char val;
    while (vals->reg_num != SENSOR_REG_END) {
        if (vals->reg_num == SENSOR_REG_DELAY) {
            private_msleep(vals->value);
        } else {
            ret = sensor_read(sd, vals->reg_num, &val);
            if (ret < 0) {
                return ret;
            }
        }
        vals++;
    }

    return 0;
}

static int sensor_write_array(struct tx_isp_subdev *sd, struct regval_list *vals)
{
    int ret;
    while (vals->reg_num != SENSOR_REG_END) {
        if (vals->reg_num == SENSOR_REG_DELAY) {
            private_msleep(vals->value);
        } else {
            ret = sensor_write(sd, vals->reg_num, vals->value);
            if (ret < 0) {
                return ret;
            }
        }
        vals++;
    }

    return 0;
}

static int sensor_reset(struct tx_isp_subdev *sd, int val)
{
    return 0;
}

static int sensor_detect(struct tx_isp_subdev *sd, unsigned int *ident)
{
    int ret = 0;
    unsigned char v;

    ret += sensor_read(sd, 0x3107, &v);
    ISP_WARNING("-----%s: %d ret = %d, v = 0x%02x\n", __func__, __LINE__, ret, v);
    if (ret < 0) {
        return ret;
    }

    if (v != SENSOR_CHIP_ID_H) {
        return -ENODEV;
    }

    *ident = v;

    ret += sensor_read(sd, 0x3108, &v);
    ISP_WARNING("-----%s: %d ret = %d, v = 0x%02x\n", __func__, __LINE__, ret, v);
    if (ret < 0) {
        return ret;
    }

    if (v != SENSOR_CHIP_ID_L) {
        return -ENODEV;
    }

    *ident = (*ident << 8) | v;

    return 0;
}

static int sensor_set_expo(struct tx_isp_subdev *sd, int value)
{
    int ret = 0;
    int it = (value & 0xffff);
    int again = (value & 0xffff0000) >> 16;

    //integration time
    ret += sensor_write(sd, 0x3e00, (unsigned char)((it >> 12) & 0xf));
    ret += sensor_write(sd, 0x3e01, (unsigned char)((it >> 4) & 0xff));
    ret += sensor_write(sd, 0x3e02, (unsigned char)((it & 0x0f) << 4));

    //sensor analog gain
    ret += sensor_write(sd, 0x3e09, (unsigned char)(((again >> 8) & 0xff)));
    //sensor dig fine gain
    ret += sensor_write(sd, 0x3e07, (unsigned char)(again & 0xff));
    if (ret < 0) {
        return ret;
    }

    return 0;
}

#if 0
static int sensor_set_integration_time(struct tx_isp_subdev *sd, int value)
{
    int ret = 0;

    value *= 2;
    ret += sensor_write(sd, 0x3e00, (unsigned char)((value >> 12) & 0xf));
    ret += sensor_write(sd, 0x3e01, (unsigned char)((value >> 4) & 0xff));
    ret += sensor_write(sd, 0x3e02, (unsigned char)((value & 0x0f) << 4));
    if (ret < 0)
        return ret;

    return 0;
}

static int sensor_set_analog_gain(struct tx_isp_subdev *sd, int value)
{
    int ret = 0;

    ret += sensor_write(sd, 0x3e09, (unsigned char)(value & 0xff));
    ret += sensor_write(sd, 0x3e08, (unsigned char)(((value >> 8) & 0xff)));
    if (ret < 0)
        return ret;

    gain_val = value;

    return 0;
}
#endif

static int sensor_set_logic(struct tx_isp_subdev *sd, int value)
{
    return 0;
}

static int sensor_set_digital_gain(struct tx_isp_subdev *sd, int value)
{
    return 0;
}

static int sensor_get_black_pedestal(struct tx_isp_subdev *sd, int value)
{
    return 0;
}

static int sensor_init(struct tx_isp_subdev *sd, int enable)
{
    struct tx_isp_sensor *sensor = sd_to_sensor_device(sd);
    int ret = 0;

    if (!enable) {
        return ISP_SUCCESS;
    }

    sensor->video.mbus.width = wsize->width;
    sensor->video.mbus.height = wsize->height;
    sensor->video.mbus.code = wsize->mbus_code;
    sensor->video.mbus.field = V4L2_FIELD_NONE;
    sensor->video.mbus.colorspace = wsize->colorspace;
    sensor->video.fps = wsize->fps;

    sensor_update_actual_fps((wsize->fps >> 16) & 0xffff);

    if (mtiming->derived) {
        /* base table minus its stream-on, then the derived window, then stream on */
        struct regval_list *v;

        for (v = wsize->regs; v->reg_num != SENSOR_REG_END; v++) {
            if (v->reg_num == 0x0100 && v->value == 0x01)
                continue;
            ret = sensor_write(sd, v->reg_num, v->value);
            if (ret)
                return ret;
        }
        ret = sensor_write_window(sd);
        if (!ret)
            ret = sensor_write_array(sd, sensor_stream_on_mipi);
    } else {
        ret = sensor_write_array(sd, wsize->regs);
    }
    if (ret) {
        return ret;
    }

    ret = tx_isp_call_subdev_notify(sd, TX_ISP_EVENT_SYNC_SENSOR_ATTR, &sensor->video);
    sensor->priv = wsize;

    return 0;
}

static int sensor_s_stream(struct tx_isp_subdev *sd, int enable)
{
    int ret = 0;

    if (enable) {
        if (data_interface == TX_SENSOR_DATA_INTERFACE_MIPI) {
            ret = sensor_write_array(sd, sensor_stream_on_mipi);
        } else {
            ISP_ERROR("Don't support this Sensor Data interface\n");
        }
        ISP_WARNING("%s stream on\n", SENSOR_NAME);
    } else {
        if (data_interface == TX_SENSOR_DATA_INTERFACE_MIPI) {
            ret = sensor_write_array(sd, sensor_stream_off_mipi);
        } else {
            ISP_ERROR("Don't support this Sensor Data interface\n");
        }
        ISP_WARNING("%s stream off\n", SENSOR_NAME);
    }

    return ret;
}

static int sensor_set_fps(struct tx_isp_subdev *sd, int fps)
{
    struct tx_isp_sensor *sensor = sd_to_sensor_device(sd);
    unsigned int clk = 0;
    unsigned int hts = 0;
    unsigned int vts = 0;
    unsigned char val = 0;
    unsigned int newformat = 0; //the format is 24.8
    int ret = 0;

    newformat = (((fps >> 16) / (fps & 0xffff)) << 8) + ((((fps >> 16) % (fps & 0xffff)) << 8) / (fps & 0xffff));
    if (newformat > (mtiming->max_fps << 8) || newformat < (SENSOR_OUTPUT_MIN_FPS << 8)) {
        ISP_ERROR("warn: fps(%d) not in range\n", fps);
        return -1;
    }

    clk = mtiming->sclk;
    ret = sensor_read(sd, 0x320c, &val);
    hts = val;
    ret += sensor_read(sd, 0x320d, &val);
    if (0 != ret) {
        ISP_ERROR("err: %s read err\n", SENSOR_NAME);
        return ret;
    }

    hts = ((hts << 8) + val);
    vts = clk * (fps & 0xffff) / hts / ((fps & 0xffff0000) >> 16);
    if (vts < mtiming->min_vts) {
        ISP_ERROR("warn: fps(%d) needs vts %u < minimum %u for mode %d\n", fps, vts,
            mtiming->min_vts, sensor_mode);
        return -1;
    }
    ret += sensor_write(sd, 0x320f, (unsigned char)(vts & 0xff));
    ret += sensor_write(sd, 0x320e, (unsigned char)(vts >> 8));
    if (0 != ret) {
        ISP_ERROR("Error: %s write error\n", SENSOR_NAME);
        return ret;
    }

    sensor->video.fps = fps;

    sensor_update_actual_fps((fps >> 16) & 0xffff);

    sensor->video.attr->max_integration_time_native = vts - mtiming->expo_margin;
    sensor->video.attr->integration_time_limit = vts - mtiming->expo_margin;
    sensor->video.attr->total_height = vts;
    sensor->video.attr->max_integration_time = vts - mtiming->expo_margin;
    ret = tx_isp_call_subdev_notify(sd, TX_ISP_EVENT_SYNC_SENSOR_ATTR, &sensor->video);

    return ret;
}

static int sensor_set_mode(struct tx_isp_subdev *sd, int value)
{
    struct tx_isp_sensor *sensor = sd_to_sensor_device(sd);
    int ret = ISP_SUCCESS;

    if (wsize) {
        sensor->video.mbus.width = wsize->width;
        sensor->video.mbus.height = wsize->height;
        sensor->video.mbus.code = wsize->mbus_code;
        sensor->video.mbus.field = V4L2_FIELD_NONE;
        sensor->video.mbus.colorspace = wsize->colorspace;
        sensor->video.fps = wsize->fps;

        sensor_update_actual_fps((wsize->fps >> 16) & 0xffff);

        ret = tx_isp_call_subdev_notify(sd, TX_ISP_EVENT_SYNC_SENSOR_ATTR, &sensor->video);
    }

    return ret;
}

static int sensor_g_chip_ident(struct tx_isp_subdev *sd, struct tx_isp_chip_ident *chip)
{
    struct i2c_client *client = tx_isp_get_subdevdata(sd);
    unsigned int ident = 0;
    int ret = ISP_SUCCESS;
    if (reset_gpio != -1) {
        ret = private_gpio_request(reset_gpio, "sensor_reset");
        if (!ret) {
            private_gpio_direction_output(reset_gpio, 1);
            private_msleep(5);
            private_gpio_direction_output(reset_gpio, 0);
            private_msleep(10);
            private_gpio_direction_output(reset_gpio, 1);
            private_msleep(10);
        } else {
            ISP_ERROR("gpio request fail %d\n", reset_gpio);
        }
    }
    if (pwdn_gpio != -1) {
        ret = private_gpio_request(pwdn_gpio, "sensor_pwdn");
        if (!ret) {
            private_gpio_direction_output(pwdn_gpio, 1);
            private_msleep(10);
            private_gpio_direction_output(pwdn_gpio, 0);
            private_msleep(10);
        } else {
            ISP_ERROR("gpio request fail %d\n", pwdn_gpio);
        }
    }
    ret = sensor_detect(sd, &ident);
    if (ret) {
        ISP_ERROR("chip found @ 0x%x (%s) is not an %s chip.\n",
              client->addr, client->adapter->name, SENSOR_NAME);
        return ret;
    }

    ISP_WARNING("%s chip found @ 0x%02x (%s)\n",
            SENSOR_NAME, client->addr, client->adapter->name);
    ISP_WARNING("sensor driver version %s\n", SENSOR_VERSION);
    if (chip) {
        memcpy(chip->name, SENSOR_NAME, sizeof(SENSOR_NAME));
        chip->ident = ident;
        chip->revision = SENSOR_VERSION;
    }

    return 0;
}

static int sensor_set_vflip(struct tx_isp_subdev *sd, int enable)
{
    int ret = -1;
    struct tx_isp_sensor *sensor = sd_to_sensor_device(sd);
    unsigned char val = 0x0;

    ret += sensor_read(sd, 0x3221, &val);
    if (enable & 0x2) {
        val |= 0x60;
    } else {
        val &= 0x9f;
    }
    ret += sensor_write(sd, 0x3221, val);
    if (!ret) {
        ret = tx_isp_call_subdev_notify(sd, TX_ISP_EVENT_SYNC_SENSOR_ATTR, &sensor->video);
    }

    return ret;
}

static int sensor_sensor_ops_ioctl(struct tx_isp_subdev *sd, unsigned int cmd, void *arg)
{
    long ret = 0;
    if (IS_ERR_OR_NULL(sd)) {
        ISP_ERROR("[%d]The pointer is invalid!\n", __LINE__);
        return -EINVAL;
    }

    switch (cmd) {
        case TX_ISP_EVENT_SENSOR_EXPO:
            if (arg) {
                ret = sensor_set_expo(sd, *(int *) arg);
            }
            break;
        case TX_ISP_EVENT_SENSOR_INT_TIME:
            //    if (arg)
            //        ret = sensor_set_integration_time(sd, *(int*)arg);
            break;
        case TX_ISP_EVENT_SENSOR_AGAIN:
            //    if (arg)
            //        ret = sensor_set_analog_gain(sd, *(int*)arg);
            break;
        case TX_ISP_EVENT_SENSOR_DGAIN:
            if (arg) {
                ret = sensor_set_digital_gain(sd, *(int *) arg);
            }
            break;
        case TX_ISP_EVENT_SENSOR_BLACK_LEVEL:
            if (arg) {
                ret = sensor_get_black_pedestal(sd, *(int *) arg);
            }
            break;
        case TX_ISP_EVENT_SENSOR_RESIZE:
            if (arg) {
                ret = sensor_set_mode(sd, *(int *) arg);
            }
            break;
        case TX_ISP_EVENT_SENSOR_PREPARE_CHANGE:
            if (data_interface == TX_SENSOR_DATA_INTERFACE_MIPI) {
                ret = sensor_write_array(sd, sensor_stream_off_mipi);
            } else {
                ISP_ERROR("Don't support this Sensor Data interface\n");
            }
            break;
        case TX_ISP_EVENT_SENSOR_FINISH_CHANGE:
            if (data_interface == TX_SENSOR_DATA_INTERFACE_MIPI) {
                ret = sensor_write_array(sd, sensor_stream_on_mipi);
            } else {
                ISP_ERROR("Don't support this Sensor Data interface\n");
                ret = -1;
            }
            break;
        case TX_ISP_EVENT_SENSOR_FPS:
            if (arg) {
                ret = sensor_set_fps(sd, *(int *) arg);
            }
            break;
        case TX_ISP_EVENT_SENSOR_VFLIP:
            if (arg) {
                ret = sensor_set_vflip(sd, *(int *) arg);
            }
            break;
        case TX_ISP_EVENT_SENSOR_LOGIC:
            if (arg) {
                ret = sensor_set_logic(sd, *(int *) arg);
            }
        default:
            break;
    }

    return ret;
}

static int sensor_g_register(struct tx_isp_subdev *sd, struct tx_isp_dbg_register *reg)
{
    unsigned char val = 0;
    int len = 0;
    int ret = 0;

    len = strlen(sd->chip.name);
    if (len && strncmp(sd->chip.name, reg->name, len)) {
        return -EINVAL;
    }

    if (!private_capable(CAP_SYS_ADMIN)) {
        return -EPERM;
    }

    ret = sensor_read(sd, reg->reg & 0xffff, &val);
    reg->val = val;
    reg->size = 2;

    return ret;
}

static int sensor_s_register(struct tx_isp_subdev *sd, const struct tx_isp_dbg_register *reg)
{
    int len = 0;

    len = strlen(sd->chip.name);
    if (len && strncmp(sd->chip.name, reg->name, len)) {
        return -EINVAL;
    }

    if (!private_capable(CAP_SYS_ADMIN)) {
        return -EPERM;
    }

    sensor_write(sd, reg->reg & 0xffff, reg->val & 0xff);

    return 0;
}

static struct tx_isp_subdev_core_ops sensor_core_ops = {
    .g_chip_ident = sensor_g_chip_ident,
    .reset = sensor_reset,
    .init = sensor_init,
    /*.ioctl = sensor_ops_ioctl,*/
    .g_register = sensor_g_register,
    .s_register = sensor_s_register,
};

static struct tx_isp_subdev_video_ops sensor_video_ops = {
    .s_stream = sensor_s_stream,
};

static struct tx_isp_subdev_sensor_ops sensor_sensor_ops = {
    .ioctl = sensor_sensor_ops_ioctl,
};

static struct tx_isp_subdev_ops sensor_ops = {
    .core = &sensor_core_ops,
    .video = &sensor_video_ops,
    .sensor = &sensor_sensor_ops,
};

/* It's the sensor device */
static u64 tx_isp_module_dma_mask = ~(u64) 0;

struct platform_device sensor_platform_device = {
    .name = SENSOR_NAME,
    .id = -1,
    .dev = {
        .dma_mask = &tx_isp_module_dma_mask,
        .coherent_dma_mask = 0xffffffff,
        .platform_data = NULL,
    },
    .num_resources = 0,
};

static int sensor_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct tx_isp_subdev *sd;
    struct tx_isp_video_in *video;
    struct tx_isp_sensor *sensor;

    sensor = (struct tx_isp_sensor *)kzalloc(sizeof(*sensor), GFP_KERNEL);
    if (!sensor) {
        ISP_ERROR("Failed to allocate sensor subdev.\n");
        return -ENOMEM;
    }

    memset(sensor, 0, sizeof(*sensor));

    sensor->mclk = clk_get(NULL, "cgu_cim");
    if (IS_ERR(sensor->mclk)) {
        ISP_ERROR("Cannot get sensor input clock cgu_cim\n");
        goto err_get_mclk;
    }

    private_clk_set_rate(sensor->mclk, 24000000);
    private_clk_enable(sensor->mclk);

    /* Convert sensor-gain into isp-gain, */
    sd = &sensor->sd;
    video = &sensor->video;
    sensor->video.shvflip = shvflip;
    sensor->video.attr = &sensor_attr;
    sensor->video.vi_max_width = wsize->width;
    sensor->video.vi_max_height = wsize->height;
    sensor->video.mbus.width = wsize->width;
    sensor->video.mbus.height = wsize->height;
    sensor->video.mbus.code = wsize->mbus_code;
    sensor->video.mbus.field = V4L2_FIELD_NONE;
    sensor->video.mbus.colorspace = wsize->colorspace;
    sensor->video.fps = wsize->fps;

    sensor_update_actual_fps((wsize->fps >> 16) & 0xffff);

    tx_isp_subdev_init(&sensor_platform_device, sd, &sensor_ops);
    tx_isp_set_subdevdata(sd, client);
    tx_isp_set_subdev_hostdata(sd, sensor);
    private_i2c_set_clientdata(client, sd);

    pr_debug("probe ok ------->%s\n", SENSOR_NAME);

    return 0;

err_get_mclk:
    private_clk_disable(sensor->mclk);
    private_clk_put(sensor->mclk);
    kfree(sensor);

    return -1;
}

static int sensor_remove(struct i2c_client *client)
{
    struct tx_isp_subdev *sd = private_i2c_get_clientdata(client);
    struct tx_isp_sensor *sensor = tx_isp_get_subdev_hostdata(sd);

    if (reset_gpio != -1) {
        private_gpio_free(reset_gpio);
    }

    if (pwdn_gpio != -1) {
        private_gpio_free(pwdn_gpio);
    }

    private_clk_disable(sensor->mclk);
    private_clk_put(sensor->mclk);
    tx_isp_subdev_deinit(sd);
    kfree(sensor);

    return 0;
}

static const struct i2c_device_id sensor_id[] = {
    {SENSOR_NAME, 0},
    {}
};

MODULE_DEVICE_TABLE(i2c, sensor_id);

static struct i2c_driver sensor_driver = {
    .driver = {
        .owner = THIS_MODULE,
        .name = SENSOR_NAME,
    },
    .probe = sensor_probe,
    .remove = sensor_remove,
    .id_table = sensor_id,
};

static __init int init_sensor(void)
{
    int ret = 0;

    sensor_apply_mode();
    sensor_common_init(&sensor_info);

    ret = private_driver_get_interface();
    if (ret) {
        ISP_ERROR("Failed to init %s driver.\n", SENSOR_NAME);
        return -1;
    }

    return private_i2c_add_driver(&sensor_driver);
}

static __exit void exit_sensor(void)
{
    private_i2c_del_driver(&sensor_driver);

    sensor_common_exit();
}

module_init(init_sensor);
module_exit(exit_sensor);

MODULE_DESCRIPTION("A low-level driver for "SENSOR_NAME" sensor");
MODULE_LICENSE("GPL");
