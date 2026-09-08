#ifndef __OMNIDECK_UI_H__
#define __OMNIDECK_UI_H__

#include <string>
#include <time.h>
#include <lvgl.h>   // 提供 lv_obj_t 完整类型

/**
 * OmniDeckUi — 400x300 RLCD 屏幕 UI 渲染引擎 (LVGL)
 *
 * 布局（双栏设计, 参照产品设计文档）:
 *   ┌──────────────────────────────────────┐
 *   │ 09-08 周二      同步 10:30 📶 🔋99% │ ← 顶栏 (30px, 底部描边)
 *   ├──────────────────────────┬───────────┤
 *   │ 📅 日程安排              │ 🔔 倒计时  │ ← 左栏 200px / 右栏 180px
 *   │ ▶ 当前 [14:00-15:30]    │ 距离【一模】│    (x=210 处垂直分隔线)
 *   │   车载 UI 架构评审       │ 119 天     │
 *   │ 🕐 下一 [19:30-20:30]   ├───────────┤
 *   │   学校作业，英语笔记     │ ✅ 待办事项│
 *   │                          │ [ ] 整理… │
 *   │                          │ [✓] 打卡… │
 *   └──────────────────────────┴───────────┘
 */
class OmniDeckUi {
public:
    OmniDeckUi() = default;
    ~OmniDeckUi() = default;
    static OmniDeckUi& GetInstance();

    /* 创建 LVGL 控件树（renderOmniDeckUI），在 OmniDeckApp::Start 中调用 */
    void Init();

    /* 每分钟 / 数据同步后调用，根据 OmniDeckApp 缓存重新计算各区域文本 */
    void Refresh();

    /* 更新顶栏日期/同步时刻（每秒调用，内部按分钟变化门控） */
    void UpdateClock(const struct tm &now);

    /* 更新电池图标与百分比 */
    void UpdateBattery(int level_percent);

    /* 页面切换: 0=主界面 1=时钟大字页 */
    void ShowPage(int page);

private:
    void renderOmniDeckUI();   // PRD 命名的核心渲染函数
    void RefreshStatus();      // 顶栏日期与同步时刻刷新
    void ApplyFontsAll();      // 14px/30px 字体补应用（assets 延迟加载时）

    bool inited_ = false;
    int page_ = 0;
    struct tm clock_ = {};     // 最近一次显示时间
    int last_minute_ = -1;     // 已显示的分钟（无变化时跳过刷新）
    int battery_level_ = -1;   // 电池百分比（-1 = 尚无有效读数）

    // 三个区域容器（ShowPage 切到时钟页时整组隐藏）
    lv_obj_t *zones_[3] = {nullptr, nullptr, nullptr};

    // 顶栏
    lv_obj_t *date_label_ = nullptr;      // "2026-09-08 周二"
    lv_obj_t *wifi_icon_ = nullptr;       // WiFi 图标（图标字体）
    lv_obj_t *battery_icon_ = nullptr;    // 电池图标（图标字体）
    lv_obj_t *battery_pct_ = nullptr;     // 电池百分比

    // 左栏: 当前/下一日程
    lv_obj_t *now_time_ = nullptr;        // 当前日程时间徽章（黑底白字）
    lv_obj_t *now_title_ = nullptr;       // 当前日程标题
    lv_obj_t *next_time_ = nullptr;       // 下一日程时间徽章（描边）
    lv_obj_t *next_title_ = nullptr;      // 下一日程标题

    // 右栏: 倒计时（描述行 = "距离" + 【标题】16px 粗体突出 + "还剩"）
    lv_obj_t *cd_pre_ = nullptr;           // "距离"（14px 常规）
    lv_obj_t *cd_title_ = nullptr;         // "【一模】"（16px 粗体突出）
    lv_obj_t *cd_post_ = nullptr;          // "还剩"（14px 常规）
    lv_obj_t *cd_days_ = nullptr;         // "119"（42px Arial Black 大数字）
    lv_obj_t *cd_unit_ = nullptr;         // "DAYS"（16px, 底边对齐）

    // 右栏: 待办事项（最多 4 条: 方框复选框 + 文本）
    lv_obj_t *todo_box_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t *todo_icon_[4] = {nullptr, nullptr, nullptr, nullptr};
    lv_obj_t *todo_text_[4] = {nullptr, nullptr, nullptr, nullptr};

    lv_obj_t *clock_label_ = nullptr;     // 时钟大字页
};

#endif // __OMNIDECK_UI_H__
