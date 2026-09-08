#ifndef __ALARM_COORDINATOR_H__
#define __ALARM_COORDINATOR_H__

#include <string>

/* NotifyPlayer 单例获取（定义在 alarm_coordinator.cc，挂在 xiaozhi AudioService 上） */
class NotifyPlayer;
NotifyPlayer& AlarmCoordinatorGetNotifyPlayer();

/**
 * AlarmCoordinator — 闹钟与语音播报联动 (PRD 3.4)
 *
 * 流程:
 *   1. PCF85063 INT (RTC_INT_PIN) 下降沿中断 → 释放信号量
 *   2. 协调器任务: 读温湿度 → 播放本地铃声 → 上报 {"event":"alarm_triggered", temp, humi}
 *   3. 收到 tts_play 后用 NotifyPlayer 拉流播放 TTS（日期+温湿度+今日日程）
 *      —— 无论闹钟触发还是 Web 端手动"语音测试"，tts_play 到达即直接播放
 *   4. 播报期间按 GPIO0 → Abort() 立即打断
 *
 * 铃声说明: xiaozhi 框架的音频输出统一走 assets 里的 opus 音效，
 * 本板直接复用内置 OGG_EXCLAMATION 作为闹铃；若需自定义 alarm_ring.pcm，
 * 可用 main/CMakeLists 的 assets 管线替换 Lang::Sounds::OGG_EXCLAMATION。
 */
class AlarmCoordinator {
public:
    AlarmCoordinator() = default;
    ~AlarmCoordinator() = default;
    static AlarmCoordinator& GetInstance();

    /* 启动: 注册 RTC INT 中断 + 创建协调器任务（幂等） */
    void Start();

    /* 由 OmniDeckApp 分钟轮询确认闹钟时间到时调用 */
    void Trigger();

    /* OmniDeckApp 收到 tts_play 后调用（可在任意线程：NotifyPlayer 自带任务） */
    void PlayTts(const std::string &url, const std::string &text);

    /* 播报中按侧边键 → 打断铃声/播报 */
    void Abort();

    /* 由分钟任务周期调用: 播报结束超时后关闭扬声器待机电流（省电） */
    void MaybeCloseOutput();

    bool IsBroadcasting() const { return broadcasting_ || ringing_; }

private:
    static void IsrHandler(void *arg);
    static void TaskEntry(void *arg);
    void Run();
    void PlayRing();        // 播放本地铃声（三声提示音）
    void CloseOutput();     // 关闭扬声器输出（省电）

    bool started_ = false;
    volatile bool ringing_ = false;        // 正在响铃
    volatile bool broadcasting_ = false;   // 正在播 TTS
    volatile bool abort_requested_ = false;
    bool output_open_ = false;             // 扬声器输出是否已开启（响铃/播报期间）
    int64_t window_start_us_ = 0;          // 本次播报窗口起始（超时后关闭输出）
    void *sem_ = nullptr;                  // SemaphoreHandle_t
};

#endif // __ALARM_COORDINATOR_H__
