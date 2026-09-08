/*
 * omnideck.cc — OmniDeck 板级定义 (基于 xiaozhi-esp32 WifiBoard)
 *
 * 硬件组装顺序:
 *   I2C 总线 (GPIO8/9) → 按键 → LCD → SHTC3/PCF85063 → OmniDeckApp 启动
 *
 * OmniDeckApp 是本板的业务扩展（WS 数据同步 / 本地日程 / 闹钟 / 屏幕 UI），
 * 通过独立的 FreeRTOS 任务延迟启动，避免阻塞 xiaozhi Application 初始化。
 */
#include <esp_lcd_panel_vendor.h>
#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_log.h>
#include <esp_pm.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "custom_lcd_display.h"
#include "wifi_board.h"
#include "application.h"
#include "button.h"
#include "codecs/box_audio_codec.h"
#include "config.h"
#include "shtc3.h"
#include "pcf85063.h"
#include "omnideck_board.h"
#include "omnideck_app.h"

#define TAG "OmniDeckBoard"

class OmniDeckBoard : public OmniDeckBoardBase {
private:
    i2c_master_bus_handle_t i2c_bus_ = nullptr;
    Button key_button_;                 // GPIO0 侧边按键
    CustomLcdDisplay *display_ = nullptr;
    Shtc3 *shtc3_ = nullptr;
    Pcf85063 *pcf85063_ = nullptr;

    /* 共享 I2C 总线: ES8311 / SHTC3(0x70) / PCF85063(0x51) 挂在同一条上 */
    void InitializeI2c() {
        i2c_master_bus_config_t cfg = {};
        cfg.i2c_port = ESP32_I2C_HOST;
        cfg.sda_io_num = I2C_SDA_PIN;
        cfg.scl_io_num = I2C_SCL_PIN;
        cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        cfg.glitch_ignore_cnt = 7;
        cfg.intr_priority = 0;
        cfg.trans_queue_depth = 0;
        cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&cfg, &i2c_bus_));
    }

    /*
     * 侧边按键 KEY (GPIO0):
     *   短按 → OmniDeck 页面切换 / 事件确认（OmniDeckApp::OnButtonShort）
     *   长按 → 重置 Wi-Fi 进入配网模式
     */
    void InitializeButtons() {
        key_button_.OnClick([this]() {
            OmniDeckApp::GetInstance().OnButtonShort();
        });
        key_button_.OnLongPress([this]() {
            ESP_LOGI(TAG, "长按按键 → 重置 Wi-Fi");
            EnterWifiConfigMode();
        });
    }

    /* ST7305 全反射屏 400x300 横屏 */
    void InitializeLcdDisplay() {
        spi_display_config_t spi_config = {};
        spi_config.mosi = RLCD_MOSI_PIN;
        spi_config.scl = RLCD_SCK_PIN;
        spi_config.dc = RLCD_DC_PIN;
        spi_config.cs = RLCD_CS_PIN;
        spi_config.rst = RLCD_RST_PIN;
        // CustomLcdDisplay 内部检测 width==400 时使用横屏像素 LUT
        display_ = new CustomLcdDisplay(NULL, NULL, RLCD_WIDTH, RLCD_HEIGHT,
                                        DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                        DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y,
                                        DISPLAY_SWAP_XY, spi_config);
    }

    /* 启动时扫描 I2C 总线，打印哪些器件在线（帮助排查接线问题）
     * 注意: 不含 ES8311——它上电后需经编解码器复位序列才会应答，
     * 裸探会得到误导性的"无应答"，其状态以 BoxAudioCodec 初始化日志为准 */
    void ProbeI2cBus() {
        struct ProbeItem { uint8_t addr; const char *name; };
        const ProbeItem items[] = {
            { ES7210_CODEC_DEFAULT_ADDR, "ES7210 麦克风阵列" },
            { 0x51, "PCF85063 RTC" },
            { 0x70, "SHTC3 温湿度" },
        };
        for (auto &it : items) {
            esp_err_t err = i2c_master_probe(i2c_bus_, it.addr, 50);
            if (err == ESP_OK) ESP_LOGI(TAG, "I2C 0x%02X: %s ✓", it.addr, it.name);
            else ESP_LOGW(TAG, "I2C 0x%02X: %s 无应答 (%s)", it.addr, it.name, esp_err_to_name(err));
        }
    }

public:
    OmniDeckBoard() : key_button_(KEY_BUTTON_GPIO) {
        // 自动轻睡眠: 空闲时芯片进入 light sleep，仅按 Wi-Fi DTIM 周期短暂唤醒。
        // 这是桌面长待机场景最大的省电项（基态电流可从 ~20-30mA 降到几 mA）。
        // 需配合 sdkconfig: CONFIG_PM_ENABLE=y + CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
        esp_pm_config_esp32s3_t pm_cfg = {
            .max_freq_mhz = 240,
            .min_freq_mhz = 80,
            .light_sleep_enable = true,
        };
        ESP_ERROR_CHECK(esp_pm_configure(&pm_cfg));

        InitializeI2c();
        InitializeButtons();

        // 深度休眠唤醒后释放 LCD/唤醒引脚的保持状态（休眠期间 hold 以保留屏显图像与上拉）
        gpio_hold_dis(RLCD_RST_PIN);
        gpio_hold_dis(RLCD_DC_PIN);
        gpio_hold_dis(RLCD_CS_PIN);
        gpio_hold_dis(KEY_BUTTON_GPIO);
        gpio_hold_dis(RTC_INT_PIN);
        gpio_deep_sleep_hold_dis();

        InitializeLcdDisplay();

        ProbeI2cBus();

        // 外设驱动：初始化失败不致命，系统照常运行（温湿度/时间显示占位，
        // RTC 不可用时日程比对退化为使用系统时钟）
        shtc3_ = new Shtc3(i2c_bus_);
        if (!shtc3_->Init()) {
            ESP_LOGW(TAG, "SHTC3 初始化失败 — 请检查 0x70 器件接线/上拉");
        }
        pcf85063_ = new Pcf85063(i2c_bus_);
        if (!pcf85063_->Init()) {
            ESP_LOGW(TAG, "PCF85063 初始化失败 — 请检查 0x51 器件接线/上拉");
        }

        // 延迟启动 OmniDeck 业务层（等 LVGL/xiaozhi 主循环就绪）
        xTaskCreate([](void *arg) {
            vTaskDelay(pdMS_TO_TICKS(3000));
            OmniDeckApp::GetInstance().Start();
            vTaskDelete(nullptr);
        }, "omnideck_boot", 4096, this, 3, nullptr);
    }

    virtual ~OmniDeckBoard() = default;

    virtual AudioCodec* GetAudioCodec() override {
        // 与 waveshare/esp32-s3-rlcd-4.2 模板板一致:
        // ES8311(0x30) 播放 + ES7210(0x40) 双麦阵列输入
        static BoxAudioCodec audio_codec(
            i2c_bus_,
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_MCLK,
            AUDIO_I2S_GPIO_BCLK,
            AUDIO_I2S_GPIO_WS,
            AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN,
            AUDIO_CODEC_PA_PIN,
            AUDIO_CODEC_ES8311_ADDR,
            AUDIO_CODEC_ES7210_ADDR,
            AUDIO_INPUT_REFERENCE);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    /* xiaozhi 系统信息会调用此接口上报板载温度 → 直接返回 SHTC3 实测值 */
    virtual bool GetTemperature(float &temperature) override {
        float humi;
        return shtc3_->Read(temperature, humi);
    }

    /*
     * 电池电量: ADC1 CH3 (GPIO4), 板载 3 倍分压。
     * 电压→百分比公式取自 Waveshare 官方示例 (03_ADC_Test):
     *   3.00V = 0%, 4.12V = 100%, 线性插值
     */
    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool initialized = false;
        static adc_oneshot_unit_handle_t adc_handle = nullptr;
        static adc_cali_handle_t cali_handle = nullptr;
        if (!initialized) {
            adc_oneshot_unit_init_cfg_t unit_cfg = { .unit_id = ADC_UNIT_1 };
            if (adc_oneshot_new_unit(&unit_cfg, &adc_handle) != ESP_OK) return false;
            adc_oneshot_chan_cfg_t chan_cfg = {
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_cfg);
            adc_cali_curve_fitting_config_t cali_cfg = {
                .unit_id = ADC_UNIT_1,
                .atten = ADC_ATTEN_DB_12,
                .bitwidth = ADC_BITWIDTH_12,
            };
            adc_cali_create_scheme_curve_fitting(&cali_cfg, &cali_handle);
            initialized = true;
        }

        // 连续采样 8 次取平均（与 Waveshare 官方示例思路一致），
        // 消除 ADC 噪声导致的百分比跳动
        float voltage_sum = 0;
        int count = 0;
        for (int i = 0; i < 8; i++) {
            int raw = 0, voltage_mv = 0;
            if (adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &raw) == ESP_OK &&
                adc_cali_raw_to_voltage(cali_handle, raw, &voltage_mv) == ESP_OK) {
                voltage_sum += 0.001f * voltage_mv * BATTERY_DIVIDER;
                count++;
            }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (count == 0) return false;
        float voltage = voltage_sum / count;
        if (voltage >= BATTERY_VOLT_MAX) level = 100;
        else if (voltage <= BATTERY_VOLT_MIN) level = 0;
        else level = (int)((voltage - BATTERY_VOLT_MIN) / (BATTERY_VOLT_MAX - BATTERY_VOLT_MIN) * 100);
        charging = false;
        discharging = true;
        return true;
    }

    /* 供 OmniDeckApp / alarm_coordinator 访问外设 */
    Shtc3& GetShtc3() { return *shtc3_; }
    Pcf85063& GetRtc() { return *pcf85063_; }
};

DECLARE_BOARD(OmniDeckBoard);

/* 业务层通过该引用访问 OmniDeck 特有外设（SHTC3 / PCF85063） */
OmniDeckBoardBase& OmniDeck() {
    return static_cast<OmniDeckBoardBase &>(Board::GetInstance());
}
