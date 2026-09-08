#ifndef __SHTC3_H__
#define __SHTC3_H__

#include "omnideck_i2c.h"

/**
 * SHTC3 温湿度传感器驱动 (I2C 地址 0x70)
 *
 * Sensirion SHTC3 协议要点：
 *  - 命令均为 16-bit，大端发送；
 *  - 测量前需 WAKE(0x3517)，测完可 SLEEP(0xB098) 省电；
 *  - 本驱动使用"无时钟拉伸、先测温度"模式 0x7866，一次连续读 6 字节：
 *    [T_MSB, T_LSB, CRC, RH_MSB, RH_LSB, CRC]
 *  - 所有操作容错：失败返回 false 并自动复位总线，绝不导致系统崩溃。
 */
class Shtc3 : public OmniI2cDevice {
public:
    explicit Shtc3(i2c_master_bus_handle_t bus);

    /* 上电后调用一次；成功返回 true（会读到芯片 ID 并校验） */
    bool Init();

    /* 触发一次测量并返回温湿度；失败返回 false（数据保持上次值） */
    bool Read(float &temp_c, float &humi_rh);

private:
    bool WriteCommand(uint16_t cmd);
    uint8_t Crc8(const uint8_t *data, int len);   // Sensirion CRC-8: poly 0x31, init 0xFF
    bool awake_ = false;
    float temp_ = 0.0f;
    float humi_ = 0.0f;
};

#endif // __SHTC3_H__
