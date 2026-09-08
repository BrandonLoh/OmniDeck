#include "alarm_coordinator.h"
#include "omnideck_board.h"
#include "omnideck_app.h"
#include "config.h"        // RTC_INT_PIN 等引脚宏
#include "shtc3.h"         // 完整类型定义（GetShtc3().Read）
#include "pcf85063.h"      // 完整类型定义（GetRtc().IsAlarmFlagged）

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <driver/gpio.h>

#include "application.h"
#include "notify/notify_player.h"
#include "assets/lang_config.h"   // 构建时生成: Lang::Sounds::OGG_EXCLAMATION 等

#define TAG "AlarmCoord"

static AlarmCoordinator instance;
AlarmCoordinator& AlarmCoordinator::GetInstance() { return instance; }

/* NotifyPlayer 单例: 挂在 xiaozhi Application 的 AudioService 上 */
static NotifyPlayer *s_notify_player = nullptr;
NotifyPlayer& AlarmCoordinatorGetNotifyPlayer() {
    if (!s_notify_player) {
        s_notify_player = new NotifyPlayer(Application::GetInstance().GetAudioService());
    }
    return *s_notify_player;
}

/* ---------------- GPIO14 (RTC INT) 下降沿中断 ---------------- */

void AlarmCoordinator::IsrHandler(void *arg) {
    auto *self = static_cast<AlarmCoordinator *>(arg);
    if (self->sem_) xSemaphoreGive((SemaphoreHandle_t)self->sem_);  // ISR 安全释放
}

void AlarmCoordinator::Start() {
    if (started_) return;
    started_ = true;

    sem_ = xSemaphoreCreateBinary();

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << RTC_INT_PIN;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;       // INT 开漏输出，需要上拉
    io.intr_type = GPIO_INTR_NEGEDGE;         // 下降沿触发
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(RTC_INT_PIN, IsrHandler, this);

    xTaskCreate(TaskEntry, "alarm_coord", 8192, this, 5, nullptr);
    ESP_LOGI(TAG, "alarm coordinator started (INT=GPIO%d)", RTC_INT_PIN);
}

/* ---------------- 协调器任务 ---------------- */

void AlarmCoordinator::TaskEntry(void *arg) {
    static_cast<AlarmCoordinator *>(arg)->Run();
}

void AlarmCoordinator::Run() {
    while (true) {
        // 等待 RTC 硬件中断（闹钟时间到）
        xSemaphoreTake((SemaphoreHandle_t)sem_, portMAX_DELAY);

        auto &rtc = OmniDeck().GetRtc();
        // 确认 TF 标志确实是 RTC 闹钟触发的（防误触发），并释放 INT
        if (rtc.IsAlarmFlagged(true)) {
            ESP_LOGI(TAG, "RTC INT → 闹钟触发");
            Trigger();
        }
    }
}

/*
 * 闹钟触发 (PRD 3.4):
 *   1. 读 SHTC3 温湿度
 *   2. 播放本地铃声（可被按键打断）
 *   3. 上报 {"event":"alarm_triggered", temp, humi} 给 NAS 服务端
 *   4. 服务端异步返回 tts_play → 由 PlayTts() 直接拉流播报
 */
void AlarmCoordinator::Trigger() {
    // 幂等保护: 响铃/播报期间重复触发直接忽略
    if (ringing_ || broadcasting_) return;
    ringing_ = true;
    abort_requested_ = false;

    // 开启扬声器输出（平时为省电处于关闭状态）
    auto *codec = OmniDeck().GetAudioCodec();
    if (codec != nullptr) codec->EnableOutput(true);
    output_open_ = true;
    window_start_us_ = esp_timer_get_time();

    float temp = 0, humi = 0;
    OmniDeck().GetShtc3().Read(temp, humi);

    // 1. 响铃（本地铃声，不依赖网络；按键可打断）
    PlayRing();
    ringing_ = false;
    if (abort_requested_) {
        ESP_LOGI(TAG, "铃声被按键打断，跳过播报");
        CloseOutput();
        return;
    }

    // 2. 上报服务端: 触发 AI 生成"日期+温湿度+今日日程"播报
    char buf[96];
    snprintf(buf, sizeof(buf), "{\"event\":\"alarm_triggered\",\"temp\":%.1f,\"humi\":%.1f}", temp, humi);
    OmniDeckApp::GetInstance().SendJsonToServer(buf);
    // TTS 音频由服务端异步返回，到达后 PlayTts() 立即播放
    // 若服务端无响应，MaybeCloseOutput() 会在 60 秒后关闭输出
}

/* 关闭扬声器输出（省电: ES8311 DAC/功放待机电流） */
void AlarmCoordinator::CloseOutput() {
    auto *codec = OmniDeck().GetAudioCodec();
    if (codec != nullptr) codec->EnableOutput(false);
    output_open_ = false;
}

/* 分钟任务调用: 播报窗口结束且无 TTS 到达时，超时关闭输出 */
void AlarmCoordinator::MaybeCloseOutput() {
    if (!output_open_ || ringing_ || broadcasting_) return;
    if (esp_timer_get_time() - window_start_us_ > 60 * 1000 * 1000) {
        ESP_LOGI(TAG, "播报超时，关闭扬声器待机电流");
        CloseOutput();
    }
}

void AlarmCoordinator::PlayRing() {
    // 播放框架内置提示音三声作为闹铃
    auto &audio = Application::GetInstance().GetAudioService();
    for (int i = 0; i < 3 && !abort_requested_; i++) {
        audio.PlaySound(Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(900));
    }
}

/*
 * 服务端 tts_play 到达时由 WS 线程调用:
 * 直接用 NotifyPlayer 拉流播放（自带 worker 任务，可在任意线程调用）。
 * Web 端"语音测试"按钮触发的播报同样走这条路，无需经过 Trigger()。
 */
void AlarmCoordinator::PlayTts(const std::string &url, const std::string &text) {
    if (url.empty()) {
        ESP_LOGI(TAG, "服务端未返回音频（未配置 TTS），仅文案: %s", text.c_str());
        CloseOutput();
        return;
    }
    ESP_LOGI(TAG, "播放 TTS: %s", url.c_str());

    auto *codec = OmniDeck().GetAudioCodec();
    if (codec != nullptr) codec->EnableOutput(true);
    output_open_ = true;

    broadcasting_ = true;
    abort_requested_ = false;
    auto &player = AlarmCoordinatorGetNotifyPlayer();
    std::vector<NotifySubtitle> subs;
    if (!player.Start(url, subs, 0x0D15, nullptr,
                      [this](uint32_t, bool ok) {
                          broadcasting_ = false;
                          ESP_LOGI(TAG, "TTS 播报结束 (%s)", ok ? "ok" : "failed");
                          CloseOutput();
                      })) {
        broadcasting_ = false;
        CloseOutput();
    }
}

void AlarmCoordinator::Abort() {
    abort_requested_ = true;
    if (broadcasting_) AlarmCoordinatorGetNotifyPlayer().Stop();
    ESP_LOGI(TAG, "按键打断播报");
}
