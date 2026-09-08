#include "shtc3.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "SHTC3";

/* SHTC3 命令字（大端 16-bit） */
static constexpr uint16_t CMD_WAKEUP   = 0x3517; // 退出休眠
static constexpr uint16_t CMD_SLEEP    = 0xB098; // 进入休眠
static constexpr uint16_t CMD_MEAS_TFH = 0x7866; // 测量: 无时钟拉伸, 温度在前
static constexpr uint16_t CMD_READ_ID  = 0xEFC8; // 读芯片 ID

Shtc3::Shtc3(i2c_master_bus_handle_t bus) : OmniI2cDevice(bus, 0x70) {}

uint8_t Shtc3::Crc8(const uint8_t *data, int len) {
    // Sensirion 规定: CRC-8, 多项式 0x31, 初值 0xFF, 无反射, 无终值异或
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
        }
    }
    return crc;
}

bool Shtc3::WriteCommand(uint16_t cmd) {
    // SHTC3 是命令流协议: 先发命令高字节再低字节（无寄存器地址）
    uint8_t buf[2] = { (uint8_t)(cmd >> 8), (uint8_t)(cmd & 0xFF) };
    return WriteRaw(buf, sizeof(buf));
}

bool Shtc3::Init() {
    if (!WriteCommand(CMD_WAKEUP)) {
        ESP_LOGE(TAG, "wakeup failed — 检查 I2C 接线/地址(0x70)/上拉电阻");
        return false;
    }
    awake_ = true;
    vTaskDelay(pdMS_TO_TICKS(1));

    // 读 ID 并校验: 低 6 bit 应为 0x07 (SHTC3 产品编码)
    if (!WriteCommand(CMD_READ_ID)) return false;
    uint8_t id[3];
    if (!ReadRaw(id, 3)) return false;
    uint16_t chip_id = ((uint16_t)id[0] << 8) | id[1];
    if ((chip_id & 0x083F) != 0x0807) {
        ESP_LOGW(TAG, "unexpected chip id 0x%04X", chip_id);
    } else {
        ESP_LOGI(TAG, "SHTC3 detected (id=0x%04X)", chip_id);
    }
    return true;
}

bool Shtc3::Read(float &temp_c, float &humi_rh) {
    // 最多尝试两次: I2C 总线偶发异常时第一次失败后 Recover 已重建句柄，重试通常成功
    for (int attempt = 0; attempt < 2; attempt++) {
        if (!awake_ && !WriteCommand(CMD_WAKEUP)) break;
        awake_ = true;

        if (!WriteCommand(CMD_MEAS_TFH)) break;
        // 无时钟拉伸模式需要等待转换完成（典型 ~12ms，取 15ms 余量）
        vTaskDelay(pdMS_TO_TICKS(15));

        uint8_t raw[6];
        if (!ReadRaw(raw, sizeof(raw))) {
            awake_ = false; // 总线异常，重试前重新唤醒
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // CRC 校验，防止在总线上读到坏数据导致 UI 显示乱跳
        if (Crc8(raw, 2) != raw[2] || Crc8(raw + 3, 2) != raw[5]) {
            ESP_LOGW(TAG, "CRC mismatch");
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        uint16_t t = ((uint16_t)raw[0] << 8) | raw[1];
        uint16_t h = ((uint16_t)raw[3] << 8) | raw[4];
        temp_ = 175.0f * t / 65536.0f - 45.0f;   // 数据手册换算公式
        humi_ = 100.0f * h / 65536.0f;
        temp_c = temp_;
        humi_rh = humi_;

        // 测完立即进入休眠省电（下次读取会自动唤醒）
        WriteCommand(CMD_SLEEP);
        awake_ = false;
        return true;
    }
    return false;
}
