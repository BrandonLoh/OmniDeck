#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/*
 * OmniDeck 板级硬件引脚映射 (ESP32-S3, 16MB Flash, 8MB PSRAM)
 *
 * 硬件基于 Waveshare ESP32-S3-RLCD-4.2 布局（与模板板一致）：
 *  - 显示屏 : 4.2" RLCD 全反射屏, ST7305 驱动, 400x300 (横置安装), SPI
 *  - I2C    : SDA=GPIO13, SCL=GPIO14（ES8311/ES7210 音频 + SHTC3 + PCF85063 共用）
 *  - RTC    : PCF85063 @ 0x51, INT 中断脚 GPIO15（⚠️ 需按实际 PCB 核对）
 *  - 传感器 : SHTC3 @ 0x70 (温湿度)
 *  - 音频   : ES8311 @ 0x30 (播放) + ES7210 @ 0x40 (双麦阵列), NS 系列功放 (PA 引脚)
 *  - 按键   : KEY=GPIO0 (短按切页, 长按重置 Wi-Fi)
 *
 * 注意: LCD/音频/I2C 引脚与 waveshare/esp32-s3-rlcd-4.2 模板板保持完全一致。
 */

#define ESP32_I2C_HOST      I2C_NUM_0
#define ESP32_LCD_HOST      SPI3_HOST

/* ------- I2C 总线（与 waveshare 模板板一致：SDA=13, SCL=14） ------- */
#define I2C_SDA_PIN         GPIO_NUM_13
#define I2C_SCL_PIN         GPIO_NUM_14

/* ------- PCF85063 RTC ------- */
#define RTC_ADDR            0x51
#define RTC_INT_PIN         GPIO_NUM_15   /* ⚠️ PRD 原为 14，但 14 已被 I2C SCL 占用；按实际 PCB 核对 */

/* ------- SHTC3 温湿度传感器 ------- */
#define SHTC3_ADDR          0x70

/* ------- 电池电量检测（参照 Waveshare 官方示例 03_ADC_Test） ------- */
#define BATTERY_ADC_CHANNEL  ADC_CHANNEL_3   /* ADC1 CH3 = GPIO4 */
#define BATTERY_DIVIDER      3.0f            /* 板载分压比 */
#define BATTERY_VOLT_MIN     3.00f           /* 0% 电压 */
#define BATTERY_VOLT_MAX     4.12f           /* 100% 电压 */

/* ------- 音频 (ES8311 + ES7210 + NS 系列功放，引脚与 waveshare 一致) ------- */
#define AUDIO_INPUT_SAMPLE_RATE  24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_INPUT_REFERENCE    true   /* ES7210 参考通道（AEC 用，模板板同款） */

#define AUDIO_I2S_GPIO_MCLK  GPIO_NUM_16
#define AUDIO_I2S_GPIO_WS    GPIO_NUM_45
#define AUDIO_I2S_GPIO_BCLK  GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN   GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT  GPIO_NUM_8

#define AUDIO_CODEC_PA_PIN       GPIO_NUM_46
#define AUDIO_CODEC_ES8311_ADDR  ES8311_CODEC_DEFAULT_ADDR   /* 0x30 */
#define AUDIO_CODEC_ES7210_ADDR  ES7210_CODEC_DEFAULT_ADDR   /* 0x40 */

/* ------- 侧边按键 ------- */
#define KEY_BUTTON_GPIO     GPIO_NUM_0

/* ------- ST7305 RLCD 显示屏 SPI ------- */
#define RLCD_DC_PIN    GPIO_NUM_5
#define RLCD_CS_PIN    GPIO_NUM_40
#define RLCD_SCK_PIN   GPIO_NUM_11
#define RLCD_MOSI_PIN  GPIO_NUM_12
#define RLCD_RST_PIN   GPIO_NUM_41

/* 横屏 400x300：CustomLcdDisplay 内部按 width==400 走横屏像素 LUT */
#define RLCD_WIDTH   400
#define RLCD_HEIGHT  300

#define DISPLAY_MIRROR_X false
#define DISPLAY_MIRROR_Y false
#define DISPLAY_SWAP_XY  false

#define DISPLAY_OFFSET_X  0
#define DISPLAY_OFFSET_Y  0

#endif /* _BOARD_CONFIG_H_ */
