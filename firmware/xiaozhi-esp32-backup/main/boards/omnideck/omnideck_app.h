#ifndef __OMNIDECK_APP_H__
#define __OMNIDECK_APP_H__

#include <string>
#include <vector>
#include <memory>

#include <cJSON.h>
#include <esp_sleep.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_websocket_client.h>
#include <time.h>

/**
 * OmniDeckApp — OmniDeck 业务层单例
 *
 * 职责:
 *  1. WebSocket 客户端: 连接 NAS 服务端 (Kconfig OMNIDECK_SERVER_URL)，
 *     处理 FULL_SYNC / SYNC_UPDATE / tts_play 下行，上报 env_report / alarm_triggered / mirror
 *  2. LittleFS 本地缓存: /littlefs/schedule_cache.json + /littlefs/events.json，
 *     Wi-Fi/NAS 断开时屏幕仍可离线显示
 *  3. 60 秒轮询: 读 PCF85063 RTC → 比对缓存日程 → 刷新 UI → 上报环境数据
 *  4. 按键: 短按切换页面（时钟页 / 日程页）
 */
class OmniDeckApp {
public:
    OmniDeckApp() = default;
    ~OmniDeckApp() = default;
    static OmniDeckApp& GetInstance();

    /* 延迟调用（board 构造后由 boot 任务触发），只执行一次 */
    void Start();

    /* 侧边按键短按：切页 / 停止闹钟播报 */
    void OnButtonShort();

    /* 向 NAS 服务端发送一条 JSON 事件（alarm_coordinator 上报用） */
    void SendJsonToServer(const std::string &json) { SendJson(json); }

    /* alarm_coordinator 在 RTC INT 中触发时调用：检查并处理今日闹钟 */
    void HandleAlarmTick();

    /* 收到服务端 tts_play 时由 WS 客户端转发给闹钟协调器播放 */
    void OnTtsReady(const std::string &url, const std::string &text);

    /* 数据结构（与 server/src/db.js 三张表一一对应） */
    struct ScheduleItem {
        int day_of_week;      // 1=周一 ... 7=周日
        std::string start_time; // "09:00"
        std::string end_time;
        std::string title;
        std::string location;
    };
    struct AlarmItem {
        std::string time;
        std::vector<int> days; // 1-7
        bool enabled;
    };
    struct CountdownItem {
        std::string title;
        std::string target_date;
    };
    struct TodoItem {
        std::string text;
        bool done;
    };

    /* 数据同步 */
    std::vector<ScheduleItem>& schedules() { return schedules_; }
    std::vector<AlarmItem>& alarms() { return alarms_; }
    const CountdownItem* countdown() const { return countdown_.get(); }
    const std::string& quote() const { return quote_; }   // 每日励志名言（服务器下发）
    std::vector<TodoItem>& todos() { return todos_; }     // 待办事项
    const std::string& last_sync() const { return last_sync_; }  // 最近同步时刻 "HH:MM"

    /* ---- 时间（以 NAS 服务器为权威） ---- */
    /* 本地显示时间 = UTC + 服务器下发的时区偏移；离线时退回 RTC/系统时间 */
    bool GetDisplayTime(struct tm &out);
    bool HaveServerTime() const { return server_epoch_ms_ > 0; }
    int64_t ServerNowMs() const;   // 服务器时间戳 + 本地单调时钟推算

    /* UTC 换算工具（Howard Hinnant civil 算法，无时区依赖，供 UI/镜像等复用） */
    static int64_t DaysFromCivil(int y, unsigned m, unsigned d);
    static time_t UtcTimeToTimeT(const struct tm &t);
    static void TimeTToUtcTm(time_t t, struct tm &out);

private:
    /* ---- LittleFS 缓存 ---- */
    void MountStorage();
    void LoadCache();
    void SaveScheduleCache();   // → /littlefs/schedule_cache.json
    void SaveEvents();          // → /littlefs/events.json (倒计时等)
    /* WS 线程只置脏标记，由分钟任务统一落盘（避免激活期堆峰值时 fopen 崩溃） */
    volatile bool cache_dirty_ = false;
    void FlushCacheIfDirty();

    /* ---- WebSocket ---- */
    void ConnectServer();
    static void WsEventHandler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
    void HandleWsMessage(const char *data, int len);
    void SendJson(const std::string &json);

    /* ---- 周期任务 ---- */
    static void MinuteTaskEntry(void *arg);
    void MinuteTick();          // 每 60s: 校时/UI/上报

    /* ---- 业务 ---- */
    void ApplySyncPayload(const cJSON *root);           // FULL_SYNC / SYNC_UPDATE 共用
    void ReportEnvironment();                            // 上报 SHTC3 温湿度
    void ReportMirror();                                 // 上报屏幕镜像快照（Web 预览用）
    void ArmNextAlarm();                                 // 把最近一个启用的闹钟写入 RTC

    /* ---- 深度休眠 ---- */
    void EnterDeepSleep();               // 配置唤醒源并进入深度休眠
    esp_sleep_wakeup_cause_t wake_cause_ = ESP_SLEEP_WAKEUP_UNDEFINED;
    bool sleep_prep_done_ = false;       // 本轮唤醒的睡前准备工作是否已执行
    bool alarm_wake_ = false;            // 本次唤醒是否来自 RTC 闹钟
    bool synced_once_ = false;           // 本轮已成功收到过同步包
    bool stay_awake_ = false;            // 开发模式: 本次开机不休眠（按键按住插电）
    bool power_on_boot_ = false;         // 冷启动（USB 插电/上电）: 首次入睡延迟 5 分钟
    int ws_attempts_ = 0;                // 本轮 WS 连接失败次数（休眠门控用）
    int64_t sleep_at_us_ = 0;            // 闹钟播报完成后入睡的时间点

    /* ---- 数据 ---- */
    std::vector<ScheduleItem> schedules_;
    std::vector<AlarmItem> alarms_;
    std::unique_ptr<CountdownItem> countdown_;           // 仅保留置顶项
    std::vector<TodoItem> todos_;                        // 待办事项
    std::string quote_ = "千里之行，始于足下";            // 每日一言（离线默认）
    std::string last_sync_ = "--:--";                    // 最近同步时刻（顶栏显示）

    bool started_ = false;
    bool ui_initialized_ = false;                        // 退出配网模式后才渲染 UI
    int page_ = 0;                                       // 0=日程页 1=时钟大字页
    int tick_count_ = 0;                                 // 分钟计数（周期性请求校时用）

    /* 服务器时间锚点: server_epoch_ms_ 为最近一次收到的服务器时间戳，
     * server_anchor_us_ 为收到时刻的 esp_timer 值，两者联合推算当前服务器时间 */
    int64_t server_epoch_ms_ = 0;
    int64_t server_anchor_us_ = 0;
    int tz_offset_min_ = 8 * 60;                         // 时区偏移（默认东八区），由服务器下发
    time_t last_min_ = 0;                                // 分钟去抖
    int last_alarm_min_ = -1;                            // 同一分钟内只触发一次

    esp_websocket_client_handle_t ws_ = nullptr;
    float last_temp_ = 0, last_humi_ = 0;
    int last_batt_ = -1;             // 最近一次电池百分比（随 env_report 上报）
    bool env_valid_ = false;
};

#endif // __OMNIDECK_APP_H__
