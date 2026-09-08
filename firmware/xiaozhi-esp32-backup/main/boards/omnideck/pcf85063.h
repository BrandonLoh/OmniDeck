#ifndef __PCF85063_H__
#define __PCF85063_H__

#include "omnideck_i2c.h"
#include <time.h>

/**
 * PCF85063 RTC 实时时钟驱动 (I2C 地址 0x51, INT 脚 GPIO14)
 *
 * 寄存器布局（数据手册 §8）：
 *   0x00 Control_1   0x01 Control_2 (TF 标志/中断使能)
 *   0x03 Seconds(bit7=VL 电压低标志)  0x04 Minutes  0x05 Hours(bit6=0 时为 24h 制)
 *   0x06 Weekdays  0x07 Days  0x08 Months  0x09 Years
 *   0x0B~0x0E 闹钟: second/minute/hour/day，各寄存器 bit7 置 1 表示"不比较该项"
 *
 * 闹钟触发时 INT 脚拉低，TF 标志置位；读时间/清除标志由 alarm_coordinator 完成。
 * 所有操作容错：失败返回 false 并自动复位总线，绝不导致系统崩溃。
 */
class Pcf85063 : public OmniI2cDevice {
public:
    explicit Pcf85063(i2c_master_bus_handle_t bus);

    /* 初始化: 检测芯片 + 确保 24 小时制 + 开启闹钟中断输出 */
    bool Init();

    /* 读取当前时间（本地时间，含 VL 掉电标志检查） */
    bool GetTime(struct tm &out, bool &voltage_low);

    /* 设置时间（写入秒/分/时/日/月/年/星期，24 小时制） */
    bool SetTime(const struct tm &t);

    /* 设置每日闹钟（hour/minute，24h 制），生效后 INT 将在该时刻拉低 */
    bool SetDailyAlarm(int hour, int minute);

    /* 是否有待处理的闹钟标志 (Control_2.TF)；clear=true 时同时清除并释放 INT */
    bool IsAlarmFlagged(bool clear = true);

    /* 闹钟标志未触发时直接释放 INT（例如播报结束后） */
    bool ClearAlarmFlag();

private:
    static uint8_t ToBcd(int v)  { return ((v / 10) << 4) | (v % 10); }
    static int FromBcd(uint8_t b){ return ((b >> 4) & 0x0F) * 10 + (b & 0x0F); }
    bool ready_ = false;
};

#endif // __PCF85063_H__
