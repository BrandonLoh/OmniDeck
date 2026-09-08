#ifndef __OMNIDECK_I2C_H__
#define __OMNIDECK_I2C_H__

#include <driver/i2c_master.h>
#include <stddef.h>
#include <stdint.h>

/**
 * OmniI2cDevice — 容错 I2C 设备封装
 *
 * 与框架自带的 I2cDevice 的区别：
 *   - 所有操作返回 bool，失败绝不 ESP_ERROR_CHECK abort（框架版本任何
 *     一次总线错误都会让整个固件崩溃重启）；
 *   - 失败后自动 i2c_master_bus_reset() 恢复总线，下次操作仍可继续；
 *   - 外设不在线（接线错/地址错）时只打日志，系统照常运行。
 */
class OmniI2cDevice {
public:
    OmniI2cDevice(i2c_master_bus_handle_t bus, uint8_t addr);

    /* 设备句柄是否创建成功 */
    bool Ok() const { return device_ != nullptr; }

    /* ---- 基础读写（SHTC3 等命令流器件用 Raw 版本） ---- */
    bool WriteRaw(const uint8_t *data, size_t len);
    bool ReadRaw(uint8_t *data, size_t len);

    /* ---- 寄存器风格读写（PCF85063 用） ---- */
    bool WriteReg(uint8_t reg, uint8_t value);
    bool WriteRegs(uint8_t reg, const uint8_t *data, size_t len);
    bool ReadReg(uint8_t reg, uint8_t &out);
    bool ReadRegs(uint8_t reg, uint8_t *data, size_t len);

protected:
    i2c_master_bus_handle_t bus_ = nullptr;
    i2c_master_dev_handle_t device_ = nullptr;
    uint8_t addr_ = 0;

    /* 失败处理：打日志 + 复位总线（返回 false 供调用方短路） */
    bool Recover(esp_err_t err, const char *what);
};

#endif // __OMNIDECK_I2C_H__
