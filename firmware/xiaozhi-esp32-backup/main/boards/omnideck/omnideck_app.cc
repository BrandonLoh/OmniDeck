#include "omnideck_app.h"
#include "omnideck_ui.h"
#include "alarm_coordinator.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <esp_websocket_client.h>
#include <esp_wifi.h>
#include <esp_littlefs.h>   // joltwallet/littlefs: esp_vfs_littlefs_register / esp_littlefs_info
#include <esp_timer.h>
#include <freertos/event_groups.h>
#include <sys/time.h>       // settimeofday（服务器校时后修正系统时钟）

#include <algorithm>
#include <memory>
#include <vector>

#include "board.h"
#include "application.h"
#include "config.h"
#include "omnideck_board.h"
#include "shtc3.h"
#include "pcf85063.h"

#define TAG "OmniDeckApp"

/* ---------------- UTC 时间换算（Howard Hinnant civil 算法，无时区依赖） ----------------
 * PCF85063 存 UTC，显示时加上服务器下发的时区偏移；这些函数避免依赖 localtime_r/
 * timegm 的时区环境变量（设备上的 TZ 可能未配置），保证任意时区下结果一致。 */

int64_t OmniDeckApp::DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);              // [0, 399]
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1; // [0, 365]
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;  // [0, 146096]
    return era * 146097 + (int64_t)doe - 719468;                 // 距 1970-01-01 的天数
}

time_t OmniDeckApp::UtcTimeToTimeT(const struct tm &t) {
    return DaysFromCivil(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday) * 86400
           + t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
}

void OmniDeckApp::TimeTToUtcTm(time_t t, struct tm &out) {
    int64_t days = t / 86400 + (t % 86400 < 0 ? -1 : 0);
    int64_t secs = t - days * 86400;   // [0, 86400)
    int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);           // [0, 146096]
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    const unsigned mp = (5 * doy + 2) / 153;                      // [0, 11]
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;              // [1, 31]
    const unsigned m = mp + (mp < 10 ? 3 : -9);                   // [1, 12]
    out.tm_year = (int)(y + (m <= 2)) - 1900;
    out.tm_mon = (int)m - 1;
    out.tm_mday = (int)d;
    out.tm_hour = (int)(secs / 3600);
    out.tm_min = (int)((secs / 60) % 60);
    out.tm_sec = (int)(secs % 60);
    // 1970-01-01 是周四（wday=4）；结果 0=周日
    out.tm_wday = (int)(((days % 7) + 4) % 7 + 7) % 7;
    out.tm_isdst = 0;
}

static const char *CACHE_PATH = "/littlefs/schedule_cache.json";
static const char *EVENTS_PATH = "/littlefs/events.json";

/* 转义 JSON 字符串中的引号/反斜杠并截断到 out_sz（缓存写入用） */
static void JsonEscape(const std::string &in, char *out, size_t out_sz) {
    size_t j = 0;
    for (char c : in) {
        if ((c == '"' || c == '\\') && j + 1 < out_sz) out[j++] = '\\';
        if (j + 1 < out_sz) out[j++] = c;
    }
    if (j < out_sz) out[j] = '\0';
}

/* 星期中文名（状态栏显示） */
static const char *WEEK_CN[] = { "周一", "周二", "周三", "周四", "周五", "周六", "周日" };

/* 深度休眠毛刺计数器（RTC 内存, 跨休眠保持） */
RTC_DATA_ATTR static int s_glitch_wakes = 0;

static OmniDeckApp instance;

OmniDeckApp& OmniDeckApp::GetInstance() { return instance; }

/* 从 ws://192.168.1.10:8080/ws 提取 http://192.168.1.10:8080，用于 TTS 音频下载 */
static std::string HttpBaseUrl() {
    std::string url = CONFIG_OMNIDECK_SERVER_URL;
    size_t host = url.find("://");
    if (host == std::string::npos) return "";
    return "http://" + url.substr(host + 3, url.find('/', host + 3) - host - 3);
}

void OmniDeckApp::Start() {
    if (started_) return;
    started_ = true;

    // 记录唤醒原因（深度休眠周期中每次唤醒都是一次全新启动）
    wake_cause_ = esp_sleep_get_wakeup_cause();
    if (wake_cause_ == ESP_SLEEP_WAKEUP_TIMER) {
        s_glitch_wakes = 0;
        ESP_LOGI(TAG, "唤醒原因: 定时器（周期刷新）");
    } else if (wake_cause_ == ESP_SLEEP_WAKEUP_EXT0) {
        // ext0 = RTC 闹钟 INT (GPIO15)。若 AF 未置位 → 休眠入口毛刺，立即回睡
        if (OmniDeck().GetRtc().IsAlarmFlagged(false)) {
            s_glitch_wakes = 0;
            alarm_wake_ = true;
            ESP_LOGI(TAG, "唤醒原因: RTC 闹钟");
        } else if (++s_glitch_wakes <= 3) {
            ESP_LOGI(TAG, "ext0 毛刺唤醒 #%d（闹钟未触发），立即回睡", (int)s_glitch_wakes);
            vTaskDelay(pdMS_TO_TICKS(300));
            EnterDeepSleep();
        } else {
            ESP_LOGW(TAG, "毛刺连续 %d 次，按正常流程执行", (int)s_glitch_wakes);
            s_glitch_wakes = 0;
        }
    } else if (wake_cause_ == ESP_SLEEP_WAKEUP_EXT1) {
        s_glitch_wakes = 0;
        ESP_LOGI(TAG, "唤醒原因: 侧边按键");
    } else {
        s_glitch_wakes = 0;
        power_on_boot_ = true;   // 无唤醒原因 = 冷启动（USB 插电/上电）
        ESP_LOGI(TAG, "唤醒原因: 上电冷启动");
    }

    // 开发模式: 插电瞬间按住侧边按键 → 本次开机完全不休眠（便于烧录/调试）
    if (gpio_get_level(KEY_BUTTON_GPIO) == 0) {
        stay_awake_ = true;
        ESP_LOGI(TAG, "检测到按键按住: 开发模式，本次开机不休眠");
    }

    MountStorage();
    LoadCache();

    ConnectServer();

    // UI 不在此初始化：配网模式下屏幕由框架的配网界面占用，
    // 由分钟任务在退出配网模式后接管渲染（见 MinuteTick）

    // 启动 RTC 闹钟中断监听（RTC_INT_PIN 下降沿）
    AlarmCoordinator::GetInstance().Start();

    // Core 1 上跑分钟轮询任务（PRD 3.3：每 60s 读取 RTC 并比对缓存）
    xTaskCreatePinnedToCore(MinuteTaskEntry, "omni_minute", 6144, this, 4, nullptr, 1);

    ESP_LOGI(TAG, "OmniDeckApp started");
}

/* ==================== LittleFS 本地缓存 ==================== */

void OmniDeckApp::MountStorage() {
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/littlefs";
    conf.partition_label = "storage";
    conf.format_if_mount_failed = true;
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS 挂载失败: %s", esp_err_to_name(err));
        return;
    }
    size_t total = 0, used = 0;
    esp_littlefs_info("storage", &total, &used);
    ESP_LOGI(TAG, "LittleFS: %d KB used / %d KB", (int)used / 1024, (int)total / 1024);
}

void OmniDeckApp::LoadCache() {
    static char buf[8192];   // 静态缓冲（BSS），不占任务栈

    // 1. 周日程缓存
    FILE *f = fopen(CACHE_PATH, "r");
    if (f) {
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            ApplySyncPayload(root);
            cJSON_Delete(root);
            ESP_LOGI(TAG, "已从 LittleFS 恢复 %d 条日程", (int)schedules_.size());
        }
    } else {
        ESP_LOGW(TAG, "无本地缓存，等待服务端同步");
    }

    // 2. 倒计时缓存（events.json 只含 countdown 字段）
    FILE *f2 = fopen(EVENTS_PATH, "r");
    if (f2) {
        size_t n2 = fread(buf, 1, sizeof(buf) - 1, f2);
        fclose(f2);
        buf[n2] = '\0';
        cJSON *root2 = cJSON_Parse(buf);
        if (root2) {
            ApplySyncPayload(root2);
            cJSON_Delete(root2);
            if (countdown_) {
                ESP_LOGI(TAG, "已从 LittleFS 恢复倒计时: %s", countdown_->title.c_str());
            }
        }
    }
}

void OmniDeckApp::SaveScheduleCache() {
    // 流式写入: 不在堆上构建整份 JSON（MQTT 连接后内部堆仅 ~5KB，
    // 大块 cJSON_Print 会把堆耗尽并导致 fopen 的锁初始化 abort）
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 4 * 1024) return;
    FILE *f = fopen(CACHE_PATH, "w");
    if (!f) return;
    fputs("{\"schedules\":[", f);
    bool first = true;
    for (auto &s : schedules_) {
        char t[128], l[96], st[16], en[16];
        JsonEscape(s.title, t, sizeof(t));
        JsonEscape(s.location, l, sizeof(l));
        JsonEscape(s.start_time, st, sizeof(st));
        JsonEscape(s.end_time, en, sizeof(en));
        char line[420];
        int n = snprintf(line, sizeof(line),
            "%s{\"day_of_week\":%d,\"start_time\":\"%s\",\"end_time\":\"%s\",\"title\":\"%s\",\"location\":\"%s\"}",
            first ? "" : ",", s.day_of_week, st, en, t, l);
        if (n > 0 && n < (int)sizeof(line)) {
            fwrite(line, 1, n, f);
            first = false;
        }
    }
    fputs("]}", f);
    fclose(f);
}

void OmniDeckApp::SaveEvents() {
    if (!countdown_ && todos_.empty()) return;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 4 * 1024) return;
    FILE *f = fopen(EVENTS_PATH, "w");
    if (!f) return;

    fputc('{', f);
    if (countdown_) {
        char t[128], d[16];
        JsonEscape(countdown_->title, t, sizeof(t));
        JsonEscape(countdown_->target_date, d, sizeof(d));
        fprintf(f, "\"countdown\":{\"title\":\"%s\",\"target_date\":\"%s\"},", t, d);
    }
    char q[128];
    JsonEscape(quote_, q, sizeof(q));
    fprintf(f, "\"quote\":\"%s\",", q);

    fputs("\"todos\":[", f);
    bool first = true;
    for (auto &td : todos_) {
        char tt[128];
        JsonEscape(td.text, tt, sizeof(tt));
        fprintf(f, "%s{\"text\":\"%s\",\"done\":%d}", first ? "" : ",", tt, td.done ? 1 : 0);
        first = false;
    }
    fputs("]}", f);
    fclose(f);
}

/* WS 线程置脏的缓存统一由分钟任务在此落盘（避开框架激活期的堆峰值）。
 * 落盘前检查内部堆余量: MQTT 连接后稳态空闲仅 ~10KB，门限取 4KB；
 * 内存不足时推迟到下一分钟（JSON 分配失败也会安全跳过，不崩溃）。 */
void OmniDeckApp::FlushCacheIfDirty() {
    if (!cache_dirty_) return;
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < 4 * 1024) {
        ESP_LOGW(TAG, "内部内存紧张，缓存落盘推迟到下一分钟");
        return;
    }
    cache_dirty_ = false;
    SaveScheduleCache();
    SaveEvents();
    ESP_LOGI(TAG, "缓存已写入 LittleFS");
}

/* ==================== WebSocket 客户端 ==================== */

void OmniDeckApp::ConnectServer() {
    esp_websocket_client_config_t cfg = {};
    cfg.uri = CONFIG_OMNIDECK_SERVER_URL;
    cfg.reconnect_timeout_ms = 10000;   // 断线自动重连（NAS 重启后自动恢复）
    cfg.buffer_size = 4096;
    ws_ = esp_websocket_client_init(&cfg);
    esp_websocket_register_events(ws_, WEBSOCKET_EVENT_ANY, WsEventHandler, this);
    esp_websocket_client_start(ws_);
}

void OmniDeckApp::WsEventHandler(void *handler_args, esp_event_base_t, int32_t event_id, void *event_data) {
    auto *self = static_cast<OmniDeckApp *>(handler_args);
    auto *event = static_cast<esp_websocket_event_data_t *>(event_data);
    switch (event_id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "已连接 OmniDeck 服务端");
            // 无需立即请求校时: 服务端连接后立刻下发的 FULL_SYNC 自带 server_time
            break;
        case WEBSOCKET_EVENT_DATA:
            // 只处理文本帧，且要排除心跳帧 (event->data_len==0)
            if (event->op_code == 0x1 && event->data_len > 0) {
                self->HandleWsMessage(event->data_ptr, event->data_len);
            }
            break;
        case WEBSOCKET_EVENT_DISCONNECTED: {
            // 仅在网络已连通后才计入失败次数: Wi-Fi 未连上时的失败
            // 是必然的，不应消耗"尝试几次后入睡"的重试预算，
            // 否则慢速路由器会把预算在同步成功前烧光。
            auto state = Application::GetInstance().GetDeviceState();
            if (state == kDeviceStateIdle || state == kDeviceStateListening ||
                state == kDeviceStateSpeaking || state == kDeviceStateNotifying) {
                self->ws_attempts_++;
            }
            ESP_LOGW(TAG, "服务端连接断开(有效失败 %d)，将自动重连（离线模式：本地缓存继续显示）",
                     (int)self->ws_attempts_);
            break;
        }
        default:
            break;
    }
}

void OmniDeckApp::HandleWsMessage(const char *data, int len) {
    std::string text(data, len);
    cJSON *root = cJSON_Parse(text.c_str());
    if (!root) return;

    auto type = cJSON_GetObjectItem(root, "type");
    auto event = cJSON_GetObjectItem(root, "event");

    if (cJSON_IsString(type) &&
        (strcmp(type->valuestring, "FULL_SYNC") == 0 || strcmp(type->valuestring, "SYNC_UPDATE") == 0)) {
        ApplySyncPayload(root);
        synced_once_ = true;
        // ⚠️ 不在 WS 事件线程做文件 I/O: 此时框架可能正在做 TLS 激活、
        // 内部堆吃紧，fopen 的 newlib 锁初始化会因 OOM 直接 abort。
        // 只置脏标记，由分钟任务在堆稳定的时机统一落盘。
        cache_dirty_ = true;
        ArmNextAlarm();
        OmniDeckUi::GetInstance().Refresh();   // 立即刷新屏幕
        ESP_LOGI(TAG, "收到同步包（缓存待落盘）");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "tts_play") == 0) {
        auto url = cJSON_GetObjectItem(root, "tts_url");
        auto txt = cJSON_GetObjectItem(root, "text");
        OnTtsReady(url && cJSON_IsString(url) ? url->valuestring : "",
                   txt && cJSON_IsString(txt) ? txt->valuestring : "");
    } else if (cJSON_IsString(event) && strcmp(event->valuestring, "time_sync") == 0) {
        // 服务器时间权威: 记录锚点（毫秒时间戳 + 本地单调时钟），供推算与校时
        auto epoch = cJSON_GetObjectItem(root, "epoch_ms");
        auto tz = cJSON_GetObjectItem(root, "tz_offset_min");
        if (cJSON_IsNumber(epoch)) {
            server_epoch_ms_ = (int64_t)epoch->valuedouble;
            server_anchor_us_ = esp_timer_get_time();
            ESP_LOGI(TAG, "已同步服务器时间, tz=%d min", tz_offset_min_);
        }
        if (cJSON_IsNumber(tz)) tz_offset_min_ = (int)tz->valuedouble;
    }
    cJSON_Delete(root);
}

void OmniDeckApp::SendJson(const std::string &json) {
    if (ws_ && esp_websocket_client_is_connected(ws_)) {
        esp_websocket_client_send_text(ws_, json.c_str(), json.length(), pdMS_TO_TICKS(2000));
    }
}

/* FULL_SYNC / SYNC_UPDATE / 本地缓存解析共用此函数。
 * 注意: 只对"出现在 JSON 中的字段"做整组替换——本地缓存文件只包含部分字段
 * （schedule_cache.json 只有 schedules，events.json 只有 countdown），
 * 全量包则包含全部字段；countdown 为 null 时表示服务器已取消置顶。 */
void OmniDeckApp::ApplySyncPayload(const cJSON *root) {
    // ---- 时区偏移（先于 server_time 解析, 供同步时刻换算使用） ----
    auto tz = cJSON_GetObjectItem(root, "tz_offset_min");
    if (cJSON_IsNumber(tz)) tz_offset_min_ = (int)tz->valuedouble;

    // ---- 服务器时间锚点（同步包自带，省一次 time_request 往返） ----
    auto st = cJSON_GetObjectItem(root, "server_time");
    if (cJSON_IsNumber(st)) {
        server_epoch_ms_ = (int64_t)st->valuedouble;
        server_anchor_us_ = esp_timer_get_time();
        // 记录"最近同步时刻"（顶栏显示）
        struct tm lt;
        TimeTToUtcTm((time_t)(server_epoch_ms_ / 1000) + tz_offset_min_ * 60, lt);
        char sync_buf[8];
        snprintf(sync_buf, sizeof(sync_buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
        last_sync_ = sync_buf;
    }

    // ---- 待办事项（仅当字段存在时整组替换） ----
    auto td = cJSON_GetObjectItem(root, "todos");
    if (cJSON_IsArray(td)) {
        todos_.clear();
        cJSON *it;
        cJSON_ArrayForEach(it, td) {
            auto *t = cJSON_GetObjectItem(it, "text");
            auto *d = cJSON_GetObjectItem(it, "done");
            todos_.push_back({
                t && cJSON_IsString(t) ? t->valuestring : "",
                d && cJSON_IsTrue(d),
            });
        }
    }

    // ---- 周日程（仅当字段存在时整组替换） ----
    auto sch = cJSON_GetObjectItem(root, "schedules");
    if (cJSON_IsArray(sch)) {
        schedules_.clear();
        cJSON *it;
        cJSON_ArrayForEach(it, sch) {
            auto get = [&](const char *k) -> const char * {
                auto *v = cJSON_GetObjectItem(it, k);
                return v && cJSON_IsString(v) ? v->valuestring : "";
            };
            auto *d = cJSON_GetObjectItem(it, "day_of_week");
            schedules_.push_back({
                d && cJSON_IsNumber(d) ? (int)d->valuedouble : 1,
                get("start_time"), get("end_time"), get("title"), get("location"),
            });
        }
    }

    // ---- 置顶倒计时（null = 已取消置顶，同样需要清除本地状态） ----
    auto cd = cJSON_GetObjectItem(root, "countdown");
    if (cJSON_IsObject(cd) || cJSON_IsNull(cd)) {
        countdown_.reset();
        if (cJSON_IsObject(cd)) {
            auto *t = cJSON_GetObjectItem(cd, "title");
            auto *date = cJSON_GetObjectItem(cd, "target_date");
            if (cJSON_IsString(t) && cJSON_IsString(date)) {
                countdown_ = std::make_unique<CountdownItem>(CountdownItem{ t->valuestring, date->valuestring });
            }
        }
    }

    // ---- 每日一言（仅当字段存在时更新） ----
    auto q = cJSON_GetObjectItem(root, "quote");
    if (cJSON_IsString(q)) quote_ = q->valuestring;

    // ---- 闹钟（仅启用的；仅当字段存在时整组替换） ----
    auto al = cJSON_GetObjectItem(root, "alarms");
    if (cJSON_IsArray(al)) {
        alarms_.clear();
        cJSON *it;
        cJSON_ArrayForEach(it, al) {
            auto *en = cJSON_GetObjectItem(it, "enabled");
            auto *tm = cJSON_GetObjectItem(it, "time");
            if (!cJSON_IsString(tm) || (en && !cJSON_IsTrue(en))) continue;
            AlarmItem a{ tm->valuestring, {}, true };
            auto *mask = cJSON_GetObjectItem(it, "days_mask");
            if (cJSON_IsString(mask)) {
                cJSON *days = cJSON_Parse(mask->valuestring);
                if (days) {
                    cJSON *d;
                    cJSON_ArrayForEach(d, days) a.days.push_back((int)d->valuedouble);
                    cJSON_Delete(days);
                }
            }
            alarms_.push_back(std::move(a));
        }
    }
}

/* ==================== 时间同步（NAS 服务器为权威） ==================== */

int64_t OmniDeckApp::ServerNowMs() const {
    return server_epoch_ms_ + (esp_timer_get_time() - server_anchor_us_) / 1000;
}

bool OmniDeckApp::GetDisplayTime(struct tm &out) {
    // 优先级: 服务器推算时间 > 已同步的系统时钟 > RTC
    time_t utc = 0;
    bool have_utc = false;
    if (HaveServerTime()) {
        utc = (time_t)(ServerNowMs() / 1000);
        have_utc = true;
    } else {
        time_t sys_t = time(nullptr);
        if (sys_t > 1700000000) {   // 系统时钟已被 SNTP/服务器校时过
            utc = sys_t;
            have_utc = true;
        } else {
            struct tm rtc_tm;
            bool vl;
            if (OmniDeck().GetRtc().GetTime(rtc_tm, vl)) {
                utc = UtcTimeToTimeT(rtc_tm);
                have_utc = true;
            }
        }
    }
    if (!have_utc) return false;
    TimeTToUtcTm(utc + tz_offset_min_ * 60, out);   // 本地显示时间
    return true;
}

/* ==================== 深度休眠 ==================== */

void OmniDeckApp::EnterDeepSleep() {
#ifdef CONFIG_OMNIDECK_DEEP_SLEEP
    // 释放闹钟 INT 线（清除 TF），确保休眠期间引脚电平可预测
    OmniDeck().GetRtc().ClearAlarmFlag();

    // 保持 LCD 引脚电平: MIP 全反射屏在深度休眠期间保留最后画面，
    // 若 RST 脚浮空导致面板复位，唤醒后会白屏
    gpio_hold_en(RLCD_RST_PIN);
    gpio_hold_en(RLCD_DC_PIN);
    gpio_hold_en(RLCD_CS_PIN);
    // 保持唤醒引脚的上拉: ESP32-S3 深度休眠会关闭未 hold 引脚的上拉电阻，
    // RTC INT 是开漏输出，失去上拉会悬空跌落误触发 ext0
    gpio_hold_en(KEY_BUTTON_GPIO);
    gpio_hold_en(RTC_INT_PIN);
    gpio_deep_sleep_hold_en();

    // 唤醒源 1: RTC 闹钟 INT (GPIO15) 拉低 → ext0（单引脚电平唤醒）
    esp_sleep_enable_ext0_wakeup(RTC_INT_PIN, 0);
    // 唤醒源 2: 侧边按键 (GPIO0) 拉低 → ext1
    esp_sleep_enable_ext1_wakeup((1ULL << KEY_BUTTON_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
    // 唤醒源 3: 定时器（周期刷新屏幕与同步）
    esp_sleep_enable_timer_wakeup((uint64_t)CONFIG_OMNIDECK_WAKE_INTERVAL_SEC * 1000000ULL);

    ESP_LOGI(TAG, "进入深度休眠 (KEY=%d INT=%d, %d 秒后自动唤醒)",
             gpio_get_level(KEY_BUTTON_GPIO), gpio_get_level(RTC_INT_PIN),
             CONFIG_OMNIDECK_WAKE_INTERVAL_SEC);
    esp_deep_sleep_start();
#endif
}

void OmniDeckApp::MinuteTaskEntry(void *arg) {
    auto *self = static_cast<OmniDeckApp *>(arg);
    int tick = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(5000));   // 5 秒轻量循环（省电），重活每分钟一次

        // 退出配网模式后接管屏幕。要求: 开机 ≥10 秒（避开启动期堆尖峰）
        // 且内部堆充足；深度休眠周期中每次唤醒都走此路径，尽早渲染后入睡。
        if (!self->ui_initialized_ && !OmniDeck().IsInWifiConfigMode() &&
            esp_timer_get_time() > 10 * 1000 * 1000 &&
            heap_caps_get_free_size(MALLOC_CAP_INTERNAL) > 8 * 1024) {
            OmniDeckUi::GetInstance().Init();
            OmniDeckUi::GetInstance().ShowPage(self->page_);
            self->ui_initialized_ = true;
            ESP_LOGI(TAG, "OmniDeck UI 接管屏幕");
        }

#ifdef CONFIG_OMNIDECK_DEEP_SLEEP
        // 深度休眠调度: 等待"首次同步成功"后才入睡，确保数据新鲜;
        // 若 NAS 不可达，连续 3 次有效失败（仅统计网络连通后的失败）或
        // 120s 硬超时后也入睡兜底，避免服务端挂掉时设备永不休眠。
        bool sleep_ready = (self->synced_once_ || self->ws_attempts_ >= 3 ||
                            esp_timer_get_time() > 120 * 1000 * 1000);
        if (self->power_on_boot_ && !self->stay_awake_) {
            sleep_ready = sleep_ready && esp_timer_get_time() > 300 * 1000 * 1000;
        }
        if (sleep_ready && !self->stay_awake_) {
        if (!self->sleep_prep_done_ && self->ui_initialized_) {
            self->sleep_prep_done_ = true;
            self->MinuteTick();   // 含省电配置/传感器/缓存落盘/同步上报
            if (self->alarm_wake_) {
                // 闹钟唤醒: 先响铃播报，播报结束后再入睡
                AlarmCoordinator::GetInstance().Trigger();
                self->sleep_at_us_ = esp_timer_get_time() + 150 * 1000 * 1000;
            } else {
                self->EnterDeepSleep();
            }
        }
        }
        // 闹钟播报完成后入睡
        if (self->sleep_at_us_ != 0 &&
            esp_timer_get_time() > self->sleep_at_us_ &&
            !AlarmCoordinator::GetInstance().IsBroadcasting()) {
            self->EnterDeepSleep();
        }
#endif

        // 每秒刷新时钟显示（时间走系统/服务器时钟，不依赖分钟级 RTC 读取）
        struct tm disp;
        if (self->GetDisplayTime(disp)) {
            OmniDeckUi::GetInstance().UpdateClock(disp);
        }

        if (++tick >= 12) {   // 12 × 5 秒 = 每分钟重活
            tick = 0;
            self->MinuteTick();
        }
    }
}

void OmniDeckApp::MinuteTick() {
    tick_count_++;
    FlushCacheIfDirty();   // WS 线程置脏的同步数据在此统一落盘

    // ---- 0. 独立模式省电（首个分钟周期应用一次） ----
    if (tick_count_ == 1) {
        auto *codec = OmniDeck().GetAudioCodec();
        if (codec != nullptr) {
            codec->EnableInput(false);    // ES7210 麦克风停转（无语音功能）
            codec->EnableOutput(false);   // ES8311 DAC/功放待机关闭（闹钟响铃前重新开启）
        }
        // Wi-Fi 保持框架默认 MAX_MODEM: 自动轻睡眠（esp_pm）开启后，
        // IDF 会按 DTIM 周期协调芯片休眠，无需手动改 listen_interval
        ESP_LOGI(TAG, "低功耗配置已应用: 麦克风/扬声器关闭");
    }

    // 播报结束后若超过 60 秒没有新的播报，关闭扬声器待机电流
    AlarmCoordinator::GetInstance().MaybeCloseOutput();

    auto &board = OmniDeck();
    struct tm utc_now;
    bool vl;
    bool rtc_ok = board.GetRtc().GetTime(utc_now, vl);

    // ---- 1. 时间同步: 以 NAS 服务器时间为权威 ----
    if (HaveServerTime()) {
        int64_t srv_sec = ServerNowMs() / 1000;
        if (rtc_ok && llabs(srv_sec - (int64_t)UtcTimeToTimeT(utc_now)) > 5) {
            struct tm utc;
            TimeTToUtcTm((time_t)srv_sec, utc);
            board.GetRtc().SetTime(utc);          // RTC 偏差 >5s 时回写
        }
        time_t sys_t = time(nullptr);
        if (llabs((long long)(srv_sec - (long long)sys_t)) > 120) {
            struct timeval tv = { (time_t)srv_sec, 0 };
            settimeofday(&tv, nullptr);           // 系统时钟偏差过大时同步（影响框架自身）
            ESP_LOGI(TAG, "系统时钟已同步至服务器时间");
        }
        // 每 15 分钟重新请求一次校时，抵消长时间运行的时钟漂移
        if (tick_count_ % 15 == 0) SendJson("{\"event\":\"time_request\"}");
    } else if (rtc_ok) {
        // 无服务器时间: 退回 SNTP 系统时间回写 RTC（仅当系统时间有效）
        time_t sys_now = time(nullptr);
        if (sys_now > 1700000000 &&
            llabs((long long)(sys_now - (long long)UtcTimeToTimeT(utc_now))) > 120) {
            struct tm utc;
            TimeTToUtcTm(sys_now, utc);
            board.GetRtc().SetTime(utc);
        }
    }

    // ---- 2. 闹钟匹配: INT 中断 + 分钟轮询双保险（使用本地显示时间） ----
    struct tm disp;
    if (GetDisplayTime(disp)) {
        int min_of_day = disp.tm_hour * 60 + disp.tm_min;
        if (min_of_day != last_alarm_min_) {
            HandleAlarmTick();
            last_alarm_min_ = min_of_day;
        }
    }

    // ---- 3. 温湿度采样（SHTC3） ----
    float t, h;
    if (board.GetShtc3().Read(t, h)) {
        last_temp_ = t; last_humi_ = h; env_valid_ = true;
    }

    // ---- 4. UI 刷新 + 状态上报 ----
    int battery_level = -1;
    bool charging = false, discharging = false;
    if (board.GetBatteryLevel(battery_level, charging, discharging)) {
        last_batt_ = battery_level;
        OmniDeckUi::GetInstance().UpdateBattery(battery_level);
    }
    OmniDeckUi::GetInstance().Refresh();
    // 网络上报每 15 分钟一次（省电: 减少 Wi-Fi 发送/唤醒）；开机首个分钟先报一次
    if (tick_count_ % 15 == 0 || tick_count_ == 1) {
        ReportEnvironment();
        ReportMirror();
    }
}

/* ==================== 数据上报 ==================== */

void OmniDeckApp::ReportEnvironment() {
    if (!env_valid_) return;
    // batt 随温湿度一起上报，Web 管理端可实时查看电池
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"event\":\"env_report\",\"temp\":%.1f,\"humi\":%.1f,\"batt\":%d}",
             last_temp_, last_humi_, last_batt_);
    SendJson(buf);
}

void OmniDeckApp::ReportMirror() {
    // 屏幕镜像快照: 把当前 UI 各区域的文本上报给 Web 端 1:1 渲染
    struct tm now;
    if (!GetDisplayTime(now)) return;
    char status[64], sched[128];
    snprintf(status, sizeof(status), "%02d-%02d %s  同步 %s",
             now.tm_mon + 1, now.tm_mday,
             WEEK_CN[((now.tm_wday + 6) % 7)], last_sync_.c_str());

    // 找当前/下一个日程
    auto find = [&](bool current) -> std::string {
        int dow = ((now.tm_wday + 6) % 7) + 1;
        int mins = now.tm_hour * 60 + now.tm_min;
        std::string title = "暂无日程", range = "", extra = "";
        for (auto &s : schedules_) {
            if (s.day_of_week != dow) continue;
            int st = atoi(s.start_time.c_str()) * 60 + atoi(s.start_time.c_str() + 3);
            int en = atoi(s.end_time.c_str()) * 60 + atoi(s.end_time.c_str() + 3);
            if (current ? (mins >= st && mins < en) : (st > mins)) {
                title = s.title; range = s.start_time + " - " + s.end_time;
                extra = current && s.location.size() ? " @" + s.location : "";
                break;
            }
        }
        return range.size() ? title + "|" + range + extra : title;
    };

    std::string cur = find(true);
    std::string nxt = find(false);
    snprintf(sched, sizeof(sched), "%s|%s|下一日程: %s", cur.c_str(), "", nxt.c_str());

    // 倒计时剩余天数：纯日期差值（DaysFromCivil 无时区依赖，不会因夏令时/时区差一天）
    std::string cd_title = "未设置置顶倒计时", cd_days = "";
    if (countdown_) {
        struct tm cd_tm = {};
        strptime(countdown_->target_date.c_str(), "%Y-%m-%d", &cd_tm);
        int64_t target_days = DaysFromCivil(cd_tm.tm_year + 1900, cd_tm.tm_mon + 1, cd_tm.tm_mday);
        int64_t today_days = DaysFromCivil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
        int64_t diff = target_days - today_days;
        cd_title = "距离【" + countdown_->title + "】";
        cd_days = diff >= 0 ? "还剩 " + std::to_string(diff) + " 天"
                            : "已过去 " + std::to_string(-diff) + " 天";
    }

    // JSON 字符串转义引号（日程标题可能包含特殊字符时保持 JSON 合法）
    auto esc = [](std::string s) {
        std::string out;
        for (char c : s) { if (c == '"' || c == '\\') out += '\\'; out += c; }
        return out;
    };

    // 待办摘要（前 4 条, "✓" 前缀表示已完成）
    std::string todo_summary = "";
    int tn = std::min((int)todos_.size(), 4);
    for (int i = 0; i < tn; i++) {
        if (i) todo_summary += "|";
        todo_summary += todos_[i].done ? "✓" : "·";
        todo_summary += todos_[i].text;
    }

    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\"event\":\"mirror\",\"data\":{\"status_bar\":\"%s\","
        "\"schedule\":{\"title\":\"%s\",\"next\":\"%s\"},"
        "\"countdown\":{\"title\":\"%s\",\"days\":\"%s\"},"
        "\"quote\":\"%s\",\"todos\":\"%s\"}}",
        esc(status).c_str(),
        esc(cur).c_str(), esc(nxt).c_str(),
        esc(cd_title).c_str(), esc(cd_days).c_str(),
        esc(quote_).c_str(),
        esc(todo_summary).c_str());
    SendJson(buf);
}

/* ==================== 闹钟 ==================== */

/* 找到当前时刻(hh:mm)匹配的闹钟 → 交给协调器响铃/播报 */
void OmniDeckApp::HandleAlarmTick() {
    // 用本地显示时间（服务器时间 + 时区偏移）做匹配，与屏幕显示一致
    struct tm lt;
    if (!GetDisplayTime(lt)) return;
    int dow = lt.tm_wday == 0 ? 7 : lt.tm_wday;   // 1=周一 ... 7=周日
    char hhmm[8];
    snprintf(hhmm, sizeof(hhmm), "%02d:%02d", lt.tm_hour, lt.tm_min);

    for (auto &a : alarms_) {
        if (a.enabled && a.time == hhmm &&
            std::find(a.days.begin(), a.days.end(), dow) != a.days.end()) {
            ESP_LOGI(TAG, "闹钟时间到 %s", hhmm);
            AlarmCoordinator::GetInstance().Trigger();
            break;
        }
    }
}

/* 把缓存中下一个启用的闹钟写入 PCF85063（INT 硬件加速触发） */
void OmniDeckApp::ArmNextAlarm() {
    if (alarms_.empty()) return;
    // 简化策略: 取第一个启用闹钟写入 RTC 每日闹钟; 分钟轮询仍负责按 days_mask 精确匹配
    auto &a = alarms_.front();
    int h = atoi(a.time.c_str()), m = atoi(a.time.c_str() + 3);
    auto &board = OmniDeck();
    board.GetRtc().SetDailyAlarm(h, m);
}

/* ==================== 按键 / TTS ==================== */

void OmniDeckApp::OnButtonShort() {
    if (AlarmCoordinator::GetInstance().IsBroadcasting()) {
        AlarmCoordinator::GetInstance().Abort();   // 播报中按键 → 立即打断
        return;
    }
    page_ = (page_ + 1) % 2;
    OmniDeckUi::GetInstance().ShowPage(page_);
}

void OmniDeckApp::OnTtsReady(const std::string &url, const std::string &text) {
    // 服务端未配置 TTS 时只返回文案（本地模板），无音频可播
    if (url.empty()) {
        ESP_LOGI(TAG, "收到播报文案（服务端无音频）: %s", text.c_str());
        return;
    }
    // 服务端返回相对路径时拼接 http 基址
    std::string full = url.rfind("http", 0) == 0 ? url : HttpBaseUrl() + url;
    ESP_LOGI(TAG, "收到 TTS: %s (%s)", text.c_str(), full.c_str());
    AlarmCoordinator::GetInstance().PlayTts(full, text);
}
