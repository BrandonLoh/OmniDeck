#include "omnideck_i2c.h"
#include <esp_log.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "OmniI2c";

OmniI2cDevice::OmniI2cDevice(i2c_master_bus_handle_t bus, uint8_t addr)
    : bus_(bus), addr_(addr) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr;
    cfg.scl_speed_hz = 100 * 1000;   // SHTC3/PCF85063 均支持 100k~400k
    cfg.flags.disable_ack_check = 0;
    if (i2c_master_bus_add_device(bus_, &cfg, &device_) != ESP_OK) {
        ESP_LOGE(TAG, "0x%02X 设备注册失败", addr_);
        device_ = nullptr;
    }
}

bool OmniI2cDevice::Recover(esp_err_t err, const char *what) {
    ESP_LOGW(TAG, "0x%02X %s 失败: %s → 复位总线并重建设备句柄", addr_, what, esp_err_to_name(err));
    i2c_master_bus_reset(bus_);
    // 失败的事务可能在设备句柄的 ops 队列中残留，导致后续全部 INVALID_STATE；
    // 重建句柄回到干净状态（remove 失败也忽略，尽量恢复）
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
        device_ = nullptr;
    }
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = addr_;
    cfg.scl_speed_hz = 100 * 1000;
    cfg.flags.disable_ack_check = 0;
    if (i2c_master_bus_add_device(bus_, &cfg, &device_) != ESP_OK) {
        device_ = nullptr;
    }
    return false;
}

bool OmniI2cDevice::WriteRaw(const uint8_t *data, size_t len) {
    if (!device_) return false;
    esp_err_t err = i2c_master_transmit(device_, data, len, 100);
    return err == ESP_OK ? true : Recover(err, "写");
}

bool OmniI2cDevice::ReadRaw(uint8_t *data, size_t len) {
    if (!device_) return false;
    esp_err_t err = i2c_master_receive(device_, data, len, 100);
    return err == ESP_OK ? true : Recover(err, "读");
}

bool OmniI2cDevice::WriteReg(uint8_t reg, uint8_t value) {
    uint8_t buf[2] = { reg, value };
    return WriteRaw(buf, sizeof(buf));
}

bool OmniI2cDevice::WriteRegs(uint8_t reg, const uint8_t *data, size_t len) {
    uint8_t *tx = (uint8_t *)malloc(len + 1);
    if (!tx) return false;
    tx[0] = reg;
    memcpy(tx + 1, data, len);
    bool ok = WriteRaw(tx, len + 1);
    free(tx);
    return ok;
}

bool OmniI2cDevice::ReadReg(uint8_t reg, uint8_t &out) {
    if (!device_) return false;
    esp_err_t err = i2c_master_transmit_receive(device_, &reg, 1, &out, 1, 100);
    return err == ESP_OK ? true : Recover(err, "读寄存器");
}

bool OmniI2cDevice::ReadRegs(uint8_t reg, uint8_t *data, size_t len) {
    if (!device_) return false;
    esp_err_t err = i2c_master_transmit_receive(device_, &reg, 1, data, len, 100);
    return err == ESP_OK ? true : Recover(err, "读连续寄存器");
}
