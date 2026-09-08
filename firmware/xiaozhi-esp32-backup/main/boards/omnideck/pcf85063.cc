#include "pcf85063.h"
#include <esp_log.h>

static const char *TAG = "PCF85063";

/* 寄存器地址 —— PCF85063A 布局（与旧版 PCF85063TP 不同!）:
 * 0x02 是偏移校准, 0x03 是 RAM, 时间寄存器从 0x04 开始。
 * 参照 Waveshare 官方示例 (SensorLib PCF85063Constants.h) */
static constexpr uint8_t REG_CONTROL_1 = 0x00;
static constexpr uint8_t REG_CONTROL_2 = 0x01;
static constexpr uint8_t REG_SECONDS   = 0x04;
static constexpr uint8_t REG_ALRM_SEC  = 0x0B;
static constexpr uint8_t REG_ALARM_MIN = 0x0C;

/* Control_1 bit 定义 */
static constexpr uint8_t CTL1_STOP = 0x20; // 时钟停止
static constexpr uint8_t CTL1_12H  = 0x02; // 12 小时制（0 = 24 小时制）

/* Control_2 bit 定义（PCF85063A 真实语义, 参照 Waveshare SensorLib）:
 *   bit7 = AIE 闹钟中断使能（enableAlarm = set bit7）
 *   bit6 = AF  闹钟标志（触发后置 1, 写 0 清除; resetAlarm = clr bit6） */
static constexpr uint8_t CTL2_AIE = 0x80;
static constexpr uint8_t CTL2_AF  = 0x40;

Pcf85063::Pcf85063(i2c_master_bus_handle_t bus) : OmniI2cDevice(bus, 0x51) {}

bool Pcf85063::Init() {
    uint8_t c1 = 0;
    if (!ReadReg(REG_CONTROL_1, c1)) {
        ESP_LOGE(TAG, "初始化失败 — 检查 I2C 接线/地址(0x51)/上拉电阻");
        return false;
    }
    ESP_LOGI(TAG, "Control_1=0x%02X (STOP=%d, 12H=%d)", c1, (c1 >> 5) & 1, (c1 >> 1) & 1);
    // 24 小时制 + 时钟运行（同时清 12H 与 STOP 位）
    if (!WriteReg(REG_CONTROL_1, c1 & ~(CTL1_STOP | CTL1_12H))) return false;

    uint8_t c2 = 0;
    if (!ReadReg(REG_CONTROL_2, c2)) return false;
    ESP_LOGI(TAG, "Control_2=0x%02X (AF=%d)", c2, (c2 >> 6) & 1);
    // 开启闹钟中断（bit7=AIE），触发时 INT 拉低直到 AF 被清除
    if (!WriteReg(REG_CONTROL_2, c2 | CTL2_AIE)) return false;

    // 读一次时间验证通信正常
    struct tm t;
    bool vl;
    ready_ = GetTime(t, vl);
    if (ready_) {
        if (vl) ESP_LOGW(TAG, "RTC 电压低标志置位，时间可能不准，需要联网校时");
        ESP_LOGI(TAG, "PCF85063 ready, current %04d-%02d-%02d %02d:%02d:%02d",
                 t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    }
    return ready_;
}

bool Pcf85063::GetTime(struct tm &out, bool &voltage_low) {
    // PCF85063A: 从 0x04(秒) 连续读 7 个寄存器
    // 顺序: 秒/分/时/日/星期/月/年（与 Waveshare 官方示例一致）
    uint8_t buf[7];
    if (!ReadRegs(REG_SECONDS, buf, sizeof(buf))) return false;
    voltage_low = buf[0] & 0x80;
    out.tm_sec   = FromBcd(buf[0] & 0x7F);
    out.tm_min   = FromBcd(buf[1] & 0x7F);
    out.tm_hour  = FromBcd(buf[2] & 0x3F);   // 24 小时制（Init 已确保）
    out.tm_mday  = FromBcd(buf[3] & 0x3F);
    out.tm_wday  = FromBcd(buf[4] & 0x07);   // 0~6，芯片自带星期计数
    out.tm_mon   = FromBcd(buf[5] & 0x1F) - 1;      // tm_mon 0~11
    out.tm_year  = FromBcd(buf[6]) + 100;           // 2000 起 → tm_year 基于 1900
    return true;
}

bool Pcf85063::SetTime(const struct tm &t) {
    // 防御性确保时钟运行: 若 STOP/12H 意外置位（掉电/干扰），先清除再写时间
    uint8_t c1 = 0;
    if (ReadReg(REG_CONTROL_1, c1)) {
        WriteReg(REG_CONTROL_1, c1 & ~(CTL1_STOP | CTL1_12H));
    }
    uint8_t buf[7] = {
        (uint8_t)ToBcd(t.tm_sec),
        (uint8_t)ToBcd(t.tm_min),
        (uint8_t)ToBcd(t.tm_hour),                  // 24h 制
        (uint8_t)ToBcd(t.tm_mday),
        (uint8_t)ToBcd(t.tm_wday),
        (uint8_t)ToBcd(t.tm_mon + 1),
        (uint8_t)ToBcd((t.tm_year + 1900) % 100),
    };
    if (!WriteRegs(REG_SECONDS, buf, sizeof(buf))) return false;
    ESP_LOGI(TAG, "time set to %04d-%02d-%02d %02d:%02d:%02d",
             t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    return true;
}

bool Pcf85063::SetDailyAlarm(int hour, int minute) {
    // bit7=1 表示"不比较该项"；秒/日期/星期均跳过 → 每日 hh:mm 触发
    if (!WriteReg(REG_ALRM_SEC, 0x80)) return false;   // 不比较秒
    if (!WriteReg(REG_ALARM_MIN, ToBcd(minute))) return false;
    if (!WriteReg(REG_ALARM_MIN + 1, ToBcd(hour))) return false;
    if (!WriteReg(REG_ALARM_MIN + 2, 0x80)) return false; // 日期不比较 → 每日
    if (!WriteReg(REG_ALARM_MIN + 3, 0x80)) return false; // 星期不比较
    ClearAlarmFlag();
    ESP_LOGI(TAG, "daily alarm set %02d:%02d", hour, minute);
    return true;
}

bool Pcf85063::IsAlarmFlagged(bool clear) {
    uint8_t c2 = 0;
    if (!ReadReg(REG_CONTROL_2, c2)) return false;
    if (!(c2 & CTL2_AF)) return false;
    if (clear) ClearAlarmFlag();
    return true;
}

bool Pcf85063::ClearAlarmFlag() {
    uint8_t c2 = 0;
    if (!ReadReg(REG_CONTROL_2, c2)) return false;
    return WriteReg(REG_CONTROL_2, c2 & ~CTL2_AF);  // 写 0 清除 AF，INT 随之释放
}
