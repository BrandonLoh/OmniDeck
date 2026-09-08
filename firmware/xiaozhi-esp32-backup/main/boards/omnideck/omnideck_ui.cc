#include "omnideck_ui.h"
#include "omnideck_app.h"

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <lvgl.h>

#include "assets.h"
#include "board.h"
#include "display/lvgl_display/lvgl_theme.h"
#include "display/lvgl_display/lvgl_font.h"
#include "material_symbols.h"

#define TAG "OmniDeckUi"

/* 内置图标字体（14px / 20px material symbols, 随固件编译） */
LV_FONT_DECLARE(font_material_symbols_14_1);
LV_FONT_DECLARE(font_material_symbols_20_4);

/* 主题全集字体（20px, 用于一级标题; 兜底） */
LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

static std::vector<std::shared_ptr<LvglFont>> s_keep_fonts;

static const lv_font_t *GetTextFont() {
    static const lv_font_t *last_font = nullptr;
    const lv_font_t *font = &BUILTIN_TEXT_FONT;
    auto *display = Board::GetInstance().GetDisplay();
    if (display != nullptr && display->GetTheme() != nullptr) {
        auto *theme = LvglThemeManager::GetInstance().GetTheme(display->GetTheme()->name());
        if (theme != nullptr && theme->text_font() && theme->text_font()->font()) {
            font = theme->text_font()->font();
            if (font != last_font) s_keep_fonts.push_back(theme->text_font());
        }
    }
    last_font = font;
    return font;
}

/*
 * 字号层级（对照设计稿, 全部使用原生字号, 不做缩放以保证清晰度）:
 *   - 顶栏/标签/徽章: 14px（font_noto_sans_common_14_1, 1bpp 点阵, 1-bit 屏最清晰）
 *   - 正文(日程/待办): 16px（font_noto_sans_common_16_4, 4bpp）
 *   - 一级标题: 20px（主题 common_20_4）
 *   - 倒计时数字/时钟页: 30px（font_noto_sans_common_30_4）
 * 14/16/30 均打包在 assets 分区，运行时按名加载；失败回退主题 20px。
 */
static std::shared_ptr<LvglCBinFont> s_font14b;
static std::shared_ptr<LvglCBinFont> s_font16b;
static std::shared_ptr<LvglCBinFont> s_font20b;
static std::shared_ptr<LvglCBinFont> s_font42;
static std::shared_ptr<LvglCBinFont> s_font14;    // 常规字重 14px (顶栏小字)

static const lv_font_t *LoadAssetFont(const char *name, std::shared_ptr<LvglCBinFont> &holder,
                                      const char *label) {
    if (holder != nullptr && holder->font() != nullptr) return holder->font();
    void *ptr = nullptr;
    size_t size = 0;
    if (Assets::GetInstance().GetAssetData(name, ptr, size)) {
        holder = std::make_shared<LvglCBinFont>(ptr);
        if (holder->font() != nullptr) {
            ESP_LOGI(TAG, "已加载 %s", label);
            return holder->font();
        }
        holder.reset();
    }
    return GetTextFont();
}

/* 思源黑体 Bold 字重（gen_bold_fonts.py 生成, 打包于 assets 分区） */
static const lv_font_t *GetBoldFont14() {
    return LoadAssetFont("font_omni_14_bold.bin", s_font14b, "14px 粗体");
}
/* 官方常规字重 14px (顶栏小字: 1-bit 屏上粗体笔画粘连, 常规更清晰) */
static const lv_font_t *GetStandardFont14() {
    return LoadAssetFont("font_noto_sans_common_14_1.bin", s_font14, "14px 常规");
}
static const lv_font_t *GetBoldFont16() {
    return LoadAssetFont("font_omni_16_bold.bin", s_font16b, "16px 粗体");
}
static const lv_font_t *GetBoldFont20() {
    return LoadAssetFont("font_omni_20_bold.bin", s_font20b, "20px 粗体");
}
static const lv_font_t *GetDigitsFont() {
    return LoadAssetFont("font_omni_42_digits.bin", s_font42, "42px 数字");
}

/* 设计配色: RLCD 1-bit 输出，浅底深字 */
static const lv_color_t COL_BG      = lv_color_hex(0xC3C9C1);
static const lv_color_t COL_TEXT    = lv_color_hex(0x111111);
static const lv_color_t COL_BADGE   = lv_color_hex(0x111111);
static const lv_color_t COL_ONBADGE = lv_color_hex(0xFFFFFF);

static const char *WEEK_CN[] = { "周一", "周二", "周三", "周四", "周五", "周六", "周日" };

static OmniDeckUi instance;
OmniDeckUi& OmniDeckUi::GetInstance() { return instance; }

/* ---------------- 工具 ---------------- */

static lv_obj_t *CreateLabel(lv_obj_t *parent, const char *text, lv_color_t color,
                             const lv_font_t *font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, lv_pct(100));
    return l;
}

/* 图标标签: material symbols */
static lv_obj_t *CreateIcon(lv_obj_t *parent, const char *symbol, lv_color_t color,
                            const lv_font_t *icon_font) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, symbol);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, icon_font, 0);
    return l;
}

/* 时间徽章: solid=true 黑底白字, false 描边（14px 字体） */
static lv_obj_t *CreateBadge(lv_obj_t *parent, int x, int y, int w, int h,
                             lv_color_t text_color, bool solid) {
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_set_pos(badge, x, y);
    lv_obj_set_size(badge, w, h);
    lv_obj_set_style_bg_color(badge, COL_BADGE, 0);
    lv_obj_set_style_bg_opa(badge, solid ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(badge, solid ? 0 : 1, 0);
    lv_obj_set_style_border_color(badge, COL_BADGE, 0);
    lv_obj_set_style_radius(badge, 3, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *l = lv_label_create(badge);
    lv_obj_set_style_text_color(l, text_color, 0);
    lv_obj_set_style_text_font(l, GetStandardFont14(), 0);
    lv_obj_center(l);
    return l;
}

/* UTF-8 文本渲染宽度(px), 用于描述行三段文字首尾相接排布 */
static uint32_t TextWidthPx(const lv_font_t *font, const char *txt) {
    uint32_t w = 0;
    const uint8_t *p = (const uint8_t *)txt;
    while (*p) {
        uint32_t c;
        if (*p < 0x80) { c = *p; p += 1; }
        else if ((*p & 0xE0) == 0xC0) { c = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0) { c = ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F); p += 3; }
        else { c = ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F); p += 4; }
        lv_font_glyph_dsc_t g;
        if (lv_font_get_glyph_dsc(font, &g, c, 0)) w += g.adv_w;
    }
    return w;
}

void OmniDeckUi::Init() {
    if (inited_) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    renderOmniDeckUI();
    lvgl_port_unlock();
    inited_ = true;
    ESP_LOGI(TAG, "renderOmniDeckUI 完成");
}

/* 若小号字体在控件创建时尚未就绪（assets 未加载）而回退过, 补应用一次 */
void OmniDeckUi::ApplyFontsAll() {
    lv_obj_set_style_text_font(date_label_, GetStandardFont14(), 0);
    lv_obj_set_style_text_font(battery_pct_, GetStandardFont14(), 0);
    lv_obj_set_style_text_font(now_time_, GetStandardFont14(), 0);
    lv_obj_set_style_text_font(next_time_, GetStandardFont14(), 0);
    lv_obj_set_style_text_font(now_title_, GetBoldFont16(), 0);
    lv_obj_set_style_text_font(next_title_, GetBoldFont16(), 0);
    lv_obj_set_style_text_font(cd_pre_, GetStandardFont14(), 0);
    lv_obj_set_style_text_font(cd_title_, GetBoldFont16(), 0);
    lv_obj_set_style_text_font(cd_post_, GetStandardFont14(), 0);
    lv_obj_set_style_text_font(cd_days_, GetDigitsFont(), 0);
    for (int i = 0; i < 4; i++) lv_obj_set_style_text_font(todo_text_[i], GetBoldFont16(), 0);
}

void OmniDeckUi::renderOmniDeckUI() {
    const int TOP_H = 32;     // 顶栏（14px 字体, 与设计稿一致）
    const int DIV_X = 211;    // 左右分栏线
    const int RX = 215, RW = 185;   // 右栏

    lv_obj_t *root = lv_obj_create(lv_screen_active());
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 400, 300);
    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 顶栏 (14px 字体, 无缩放) ---- */
    zones_[0] = lv_obj_create(root);
    lv_obj_set_pos(zones_[0], 0, 0);
    lv_obj_set_size(zones_[0], 400, TOP_H);
    lv_obj_set_style_bg_opa(zones_[0], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zones_[0], 0, 0);
    lv_obj_set_style_border_side(zones_[0], LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(zones_[0], 2, 0);
    lv_obj_set_style_border_color(zones_[0], COL_BADGE, 0);
    lv_obj_set_style_pad_all(zones_[0], 0, 0);
    lv_obj_clear_flag(zones_[0], LV_OBJ_FLAG_SCROLLABLE);

    date_label_ = CreateLabel(zones_[0], "----", COL_TEXT, GetStandardFont14());
    lv_obj_set_width(date_label_, 220);
    lv_obj_align(date_label_, LV_ALIGN_LEFT_MID, 10, 0);

    wifi_icon_ = CreateIcon(zones_[0], MATERIAL_SYMBOLS_WIFI, COL_TEXT,
                            &font_material_symbols_20_4);
    lv_obj_align(wifi_icon_, LV_ALIGN_RIGHT_MID, -66, 1);

    // 先创建百分比, 后创建电池图标 → 图标永远绘制在百分比之上, 不会被覆盖
    battery_pct_ = CreateLabel(zones_[0], "--%", COL_TEXT, GetStandardFont14());
    lv_obj_set_style_text_align(battery_pct_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_width(battery_pct_, 32);
    lv_obj_align(battery_pct_, LV_ALIGN_RIGHT_MID, -4, 0);

    battery_icon_ = CreateIcon(zones_[0], MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL, COL_TEXT,
                               &font_material_symbols_20_4);
    lv_obj_align(battery_icon_, LV_ALIGN_RIGHT_MID, -42, 1);

    /* ---- 左右分栏垂直分隔线 ---- */
    static lv_point_precise_t v_line_pts[] = { {DIV_X, TOP_H + 7}, {DIV_X, 294} };
    lv_obj_t *v_line = lv_line_create(root);
    lv_line_set_points(v_line, v_line_pts, 2);
    lv_obj_set_style_line_width(v_line, 2, 0);
    lv_obj_set_style_line_color(v_line, lv_color_hex(0x444444), 0);

    /* ---- 左栏: 双日程聚焦 ---- */
    zones_[1] = lv_obj_create(root);
    lv_obj_set_pos(zones_[1], 0, TOP_H);
    lv_obj_set_size(zones_[1], 211, 268);
    lv_obj_set_style_bg_opa(zones_[1], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zones_[1], 0, 0);
    lv_obj_set_style_pad_all(zones_[1], 0, 0);
    lv_obj_clear_flag(zones_[1], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cal = CreateIcon(zones_[1], MATERIAL_SYMBOLS_CALENDAR_MONTH, COL_TEXT,
                               &font_material_symbols_20_4);
    lv_obj_align(cal, LV_ALIGN_TOP_LEFT, 10, 13);
    lv_obj_t *sched_title = CreateLabel(zones_[1], "日程安排", COL_TEXT, GetBoldFont20());
    lv_obj_set_width(sched_title, 160);
    lv_obj_align(sched_title, LV_ALIGN_TOP_LEFT, 36, 9);

    // [A] 当前日程: 图标+标签+时间徽章同一行, 标题下一行
    lv_obj_t *play = CreateIcon(zones_[1], MATERIAL_SYMBOLS_PLAY_ARROW, COL_TEXT,
                                &font_material_symbols_20_4);
    lv_obj_align(play, LV_ALIGN_TOP_LEFT, 12, 42);
    lv_obj_t *now_tag = CreateLabel(zones_[1], "当前日程", COL_TEXT, GetStandardFont14());
    lv_obj_set_width(now_tag, 72);
    lv_obj_align(now_tag, LV_ALIGN_TOP_LEFT, 32, 44);
    now_time_ = CreateBadge(zones_[1], 106, 44, 98, 18, COL_ONBADGE, true);

    now_title_ = CreateLabel(zones_[1], "暂无日程", COL_TEXT, GetBoldFont16());
    lv_obj_align(now_title_, LV_ALIGN_TOP_LEFT, 12, 70);

    // [B] 下一日程
    lv_obj_t *clock_icon = CreateIcon(zones_[1], MATERIAL_SYMBOLS_SCHEDULE, COL_TEXT,
                                      &font_material_symbols_20_4);
    lv_obj_align(clock_icon, LV_ALIGN_TOP_LEFT, 12, 106);
    lv_obj_t *next_tag = CreateLabel(zones_[1], "下一日程", COL_TEXT, GetStandardFont14());
    lv_obj_set_width(next_tag, 72);
    lv_obj_align(next_tag, LV_ALIGN_TOP_LEFT, 32, 108);
    next_time_ = CreateBadge(zones_[1], 106, 108, 98, 18, COL_TEXT, false);

    next_title_ = CreateLabel(zones_[1], "", COL_TEXT, GetBoldFont16());
    lv_obj_align(next_title_, LV_ALIGN_TOP_LEFT, 12, 134);

    /* ---- 右栏: 倒计时 + 待办 ---- */
    zones_[2] = lv_obj_create(root);
    lv_obj_set_pos(zones_[2], RX, TOP_H);
    lv_obj_set_size(zones_[2], RW, 268);
    lv_obj_set_style_bg_opa(zones_[2], LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zones_[2], 0, 0);
    lv_obj_set_style_pad_all(zones_[2], 0, 0);
    lv_obj_clear_flag(zones_[2], LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bell = CreateIcon(zones_[2], MATERIAL_SYMBOLS_NOTIFICATIONS, COL_TEXT,
                                &font_material_symbols_20_4);
    lv_obj_align(bell, LV_ALIGN_TOP_LEFT, 6, 13);
    lv_obj_t *cd_title = CreateLabel(zones_[2], "倒计时", COL_TEXT, GetBoldFont20());
    lv_obj_set_width(cd_title, 130);
    lv_obj_align(cd_title, LV_ALIGN_TOP_LEFT, 32, 9);

    // 倒计时描述行: "距离" + 【标题】(16px 粗体突出) + "还剩"
    // 三段文字按实测宽度首尾相接 (绝对定位), 连续靠左
    cd_pre_ = lv_label_create(zones_[2]);
    lv_obj_set_style_text_color(cd_pre_, COL_TEXT, 0);
    lv_obj_set_style_text_font(cd_pre_, GetStandardFont14(), 0);
    lv_label_set_long_mode(cd_pre_, LV_LABEL_LONG_WRAP);

    cd_title_ = lv_label_create(zones_[2]);
    lv_obj_set_style_text_color(cd_title_, COL_TEXT, 0);
    lv_obj_set_style_text_font(cd_title_, GetBoldFont16(), 0);

    cd_post_ = lv_label_create(zones_[2]);
    lv_obj_set_style_text_color(cd_post_, COL_TEXT, 0);
    lv_obj_set_style_text_font(cd_post_, GetStandardFont14(), 0);
    lv_label_set_long_mode(cd_post_, LV_LABEL_LONG_WRAP);

    // 倒计时数字: 42px 大字体, 与"还剩"行拉开间距
    cd_days_ = CreateLabel(zones_[2], "", COL_TEXT, GetDigitsFont());
    lv_obj_align(cd_days_, LV_ALIGN_TOP_LEFT, 8, 68);

    // 单位"DAYS"（16px 粗体, 与 42px 数字底边对齐:
    //   数字墨底 = 100+(64-12)-ofs_y(1) = 151; DAYS 墨底 = 标签顶+(21-4)-ofs_y(0) = 标签顶+17
    //   → 标签顶 = 134 (zone y=102)）
    cd_unit_ = CreateLabel(zones_[2], "DAYS", COL_TEXT, GetBoldFont16());
    lv_obj_set_width(cd_unit_, 70);
    lv_obj_align(cd_unit_, LV_ALIGN_TOP_LEFT, 96, 102);

    // 右栏中部分隔横线
    static lv_point_precise_t h_line_pts[] = { {4, 132}, {RW - 4, 132} };
    lv_obj_t *h_line = lv_line_create(zones_[2]);
    lv_line_set_points(h_line, h_line_pts, 2);
    lv_obj_set_style_line_width(h_line, 1, 0);
    lv_obj_set_style_line_color(h_line, lv_color_hex(0x666666), 0);

    lv_obj_t *chk = CreateIcon(zones_[2], MATERIAL_SYMBOLS_CHECK_CIRCLE, COL_TEXT,
                               &font_material_symbols_20_4);
    lv_obj_align(chk, LV_ALIGN_TOP_LEFT, 6, 144);
    lv_obj_t *todo_title = CreateLabel(zones_[2], "待办事项", COL_TEXT, GetBoldFont20());
    lv_obj_set_width(todo_title, 130);
    lv_obj_align(todo_title, LV_ALIGN_TOP_LEFT, 32, 140);

    // 待办条目: 方框复选框 + 文本（16px, 行距 20px 与标题拉开间距）
    for (int i = 0; i < 4; i++) {
        int y = 174 + i * 20;
        todo_box_[i] = lv_obj_create(zones_[2]);
        lv_obj_set_pos(todo_box_[i], 10, y + 2);
        lv_obj_set_size(todo_box_[i], 15, 15);
        lv_obj_set_style_bg_opa(todo_box_[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(todo_box_[i], 2, 0);
        lv_obj_set_style_border_color(todo_box_[i], COL_BADGE, 0);
        lv_obj_set_style_radius(todo_box_[i], 2, 0);
        lv_obj_set_style_pad_all(todo_box_[i], 0, 0);
        lv_obj_clear_flag(todo_box_[i], LV_OBJ_FLAG_SCROLLABLE);

        todo_icon_[i] = CreateIcon(todo_box_[i], "", COL_TEXT, &font_material_symbols_20_4);
        lv_obj_center(todo_icon_[i]);

        todo_text_[i] = CreateLabel(zones_[2], "", COL_TEXT, GetBoldFont16());
        lv_obj_set_width(todo_text_[i], RW - 34);
        lv_obj_align(todo_text_[i], LV_ALIGN_TOP_LEFT, 32, y - 2);
    }

    /* ---- 页面 2: 时钟大字页（30px 大字体） ---- */
    clock_label_ = CreateLabel(root, "--:--", COL_TEXT, GetDigitsFont());
    lv_obj_set_style_text_align(clock_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(clock_label_, LV_ALIGN_CENTER, 0, 0);
    lv_obj_add_flag(clock_label_, LV_OBJ_FLAG_HIDDEN);
}

void OmniDeckUi::ShowPage(int page) {
    page_ = page;
    if (!inited_) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    for (auto *z : zones_) {
        if (page == 0) lv_obj_clear_flag(z, LV_OBJ_FLAG_HIDDEN);
        else           lv_obj_add_flag(z, LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

/* 顶栏 = 日期(YYYY-MM-DD 周几) */
void OmniDeckUi::RefreshStatus() {
    char buf[40];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %s",
             clock_.tm_year + 1900, clock_.tm_mon + 1, clock_.tm_mday,
             WEEK_CN[((clock_.tm_wday + 6) % 7)]);
    lv_label_set_text(date_label_, buf);
}

void OmniDeckUi::UpdateClock(const struct tm &now) {
    clock_ = now;
    if (!inited_) return;
    // 功耗优化: 文本只在分钟变化时才需要刷新
    if (now.tm_min == last_minute_) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    // 若小号字体此前因 assets 未就绪而回退，此时补应用
    if (s_font14b == nullptr || s_font16b == nullptr || s_font20b == nullptr) {
        ApplyFontsAll();
    }
    RefreshStatus();
    char buf[16];
    snprintf(buf, sizeof(buf), "%02d:%02d", now.tm_hour, now.tm_min);
    lv_label_set_text(clock_label_, buf);
    last_minute_ = now.tm_min;
    lvgl_port_unlock();
}

void OmniDeckUi::UpdateBattery(int level_percent) {
    battery_level_ = level_percent;
    if (!inited_) return;
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    // 8 档电量图标: 0 / 1-6 档 / 满电（与框架同款映射, 索引安全不越界）
    static const char *levels[] = {
        MATERIAL_SYMBOLS_BATTERY_ANDROID_0,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_1,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_2,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_3,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_4,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_5,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_6,
        MATERIAL_SYMBOLS_BATTERY_ANDROID_FRAME_FULL,
    };
    int idx = level_percent <= 0 ? 0
              : (level_percent >= 100 ? 7 : 1 + (level_percent - 1) * 6 / 99);
    lv_label_set_text(battery_icon_, levels[idx]);
    char buf[12];
    snprintf(buf, sizeof(buf), "%d%%", level_percent);
    lv_label_set_text(battery_pct_, buf);
    lvgl_port_unlock();
}

/* 依据 OmniDeckApp 缓存重算日程/倒计时/待办区域 */
void OmniDeckUi::Refresh() {
    if (!inited_) return;

    auto &app = OmniDeckApp::GetInstance();
    struct tm now;
    if (!app.GetDisplayTime(now)) return;
    int dow = now.tm_wday == 0 ? 7 : now.tm_wday;
    int mins = now.tm_hour * 60 + now.tm_min;

    // ---- 当前日程 / 下一日程 ----
    std::string cur_time = "", cur_title = "暂无日程";
    std::string next_time = "", next_title = "";
    const OmniDeckApp::ScheduleItem *next = nullptr;
    for (auto &s : app.schedules()) {
        if (s.day_of_week != dow) continue;
        int st = atoi(s.start_time.c_str()) * 60 + atoi(s.start_time.c_str() + 3);
        int en = atoi(s.end_time.c_str()) * 60 + atoi(s.end_time.c_str() + 3);
        if (mins >= st && mins < en) {
            cur_time = s.start_time + "-" + s.end_time;
            cur_title = s.title + (s.location.empty() ? "" : "  " + s.location);
        } else if (st > mins && !next) {
            next = &s;
        }
    }
    if (next) {
        next_time = next->start_time + "-" + next->end_time;
        next_title = next->title;
    }

    // ---- 倒计时（描述行 = "距离" + 【标题】16px 粗体 + "还剩"） ----
    std::string cd_pre = "", cd_title = "未设置置顶倒计时", cd_post = "", cd_days = "";
    if (app.countdown()) {
        struct tm target = {};
        strptime(app.countdown()->target_date.c_str(), "%Y-%m-%d", &target);
        int64_t target_days = OmniDeckApp::DaysFromCivil(target.tm_year + 1900, target.tm_mon + 1, target.tm_mday);
        int64_t today_days = OmniDeckApp::DaysFromCivil(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday);
        int64_t diff = target_days - today_days;
        cd_pre = "距离";
        cd_title = "【" + app.countdown()->title + "】";
        cd_post = "还剩";
        cd_days = std::to_string(diff >= 0 ? diff : -diff);
    }

    // ---- 待办事项（最多 4 条, 方框复选框: 完成打勾） ----
    const char *CHECK_SYM = MATERIAL_SYMBOLS_CHECK;
    int n = std::min((int)app.todos().size(), 4);
    std::string todo_syms[4], todo_texts[4];
    for (int i = 0; i < 4; i++) {
        todo_syms[i] = "";
        todo_texts[i] = "";
    }
    for (int i = 0; i < n; i++) {
        if (app.todos()[i].done) todo_syms[i] = CHECK_SYM;
        todo_texts[i] = app.todos()[i].text;
    }

    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;
    // 空态: 无进行中/下一日程时徽章显示 "--:--"（避免纯黑/纯空徽章）
    lv_label_set_text(now_time_, cur_time.empty() ? "--:--" : cur_time.c_str());
    lv_label_set_text(now_title_, cur_title.c_str());
    lv_label_set_text(next_time_, next_time.empty() ? "--:--" : next_time.c_str());
    lv_label_set_text(next_title_, next_title.c_str());
    lv_label_set_text(cd_pre_, cd_pre.c_str());
    lv_label_set_text(cd_title_, cd_title.c_str());
    lv_label_set_text(cd_post_, cd_post.c_str());
    // 按实测宽度首尾相接: 距离 → 【标题】 → 还剩（连续靠左）
    uint32_t x = 8;
    uint32_t pre_w = TextWidthPx(GetStandardFont14(), cd_pre.c_str());
    if (pre_w > 0) {
        lv_obj_set_pos(cd_pre_, (lv_coord_t)x, 44);
        x += pre_w;
    }
    uint32_t title_w = TextWidthPx(GetBoldFont16(), cd_title.c_str());
    uint32_t max_w = app.countdown() ? 104 : 160;
    uint32_t w = title_w < max_w ? title_w : max_w;
    lv_obj_set_pos(cd_title_, (lv_coord_t)x, 42);
    lv_obj_set_width(cd_title_, (lv_coord_t)w);
    lv_label_set_long_mode(cd_title_, title_w > max_w ? LV_LABEL_LONG_DOT : LV_LABEL_LONG_WRAP);
    x += w;
    if (!cd_post.empty()) {
        lv_obj_set_pos(cd_post_, (lv_coord_t)x, 44);
    }
    lv_label_set_text(cd_days_, cd_days.c_str());
    lv_label_set_text(cd_unit_, app.countdown() ? "DAYS" : "");
    for (int i = 0; i < 4; i++) {
        lv_label_set_text(todo_icon_[i], todo_syms[i].c_str());
        lv_label_set_text(todo_text_[i], todo_texts[i].c_str());
    }
    lvgl_port_unlock();
}
