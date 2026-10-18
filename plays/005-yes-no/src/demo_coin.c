// main/demo_coin.c —— 005 决策硬币 (Yes or No with 3D Pixel Flip Animation)
#include "demo.h"
#include "ui_pixel.h"
#include "font_chinese_14.h"
#include "lvgl.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdbool.h>

typedef enum {
    COIN_STATE_IDLE = 0,    // 待命状态：等待用户按 OK 投掷
    COIN_STATE_FLIPPING,    // 翻转中：3D 像素立体纵向翻转动效
    COIN_STATE_RESULT       // 结果展示：定格在 YES 或 NO
} coin_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_top_info;
static lv_obj_t *s_coin_box;
static lv_obj_t *s_coin_shadow;
static lv_obj_t *s_coin_rim;
static lv_obj_t *s_coin_body;
static lv_obj_t *s_coin_inner;
static lv_obj_t *s_coin_text;

static lv_obj_t *s_result_label;
static lv_obj_t *s_comment_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_mascot;

static lv_timer_t *s_flip_timer = NULL;
static coin_state_t s_state = COIN_STATE_IDLE;

// 统计数据
static int s_total_spins = 0;
static int s_yes_count = 0;
static int s_no_count = 0;

// 动画变量
static int s_current_result = 1; // 1 = YES, 0 = NO
static int s_anim_step = 0;
static int s_total_steps = 0;
static int s_flip_phase = 0;     // 0..7 翻转相位

// 趣味评语库
static const char *YES_COMMENTS[] = {
    "命运说：去做吧！",
    "毫无疑问，冲！",
    "吉兆已现，立刻行动！",
    "答案很明确：YES！",
    "顺从内心的渴望吧！"
};

static const char *NO_COMMENTS[] = {
    "命运建议：再想想...",
    "时机未到，暂缓行动",
    "果断拒绝，换条路走",
    "不要冒险，稳住！",
    "神明示意：NO！"
};

static void update_coin_geometry(int phase, int is_yes) {
    if (!s_coin_body) return;

    int cur_side = is_yes;
    int h_scale = 100;

    switch (phase % 8) {
        case 0: h_scale = 100; cur_side = is_yes;  break;
        case 1: h_scale = 65;  cur_side = is_yes;  break;
        case 2: h_scale = 30;  cur_side = is_yes;  break;
        case 3: h_scale = 8;   cur_side = is_yes;  break;
        case 4: h_scale = 8;   cur_side = !is_yes; break;
        case 5: h_scale = 30;  cur_side = !is_yes; break;
        case 6: h_scale = 65;  cur_side = !is_yes; break;
        case 7: h_scale = 100; cur_side = !is_yes; break;
    }

    int coin_w = 84;
    int coin_h = (84 * h_scale) / 100;
    if (coin_h < 6) coin_h = 6;

    // 阴影
    lv_obj_set_size(s_coin_shadow, coin_w, coin_h);
    lv_obj_set_style_radius(s_coin_shadow, coin_h / 2, 0);
    lv_obj_set_pos(s_coin_shadow, (140 - coin_w) / 2, (100 - coin_h) / 2 + 5);

    // 边框立体厚度
    lv_obj_set_size(s_coin_rim, coin_w, coin_h);
    lv_obj_set_style_radius(s_coin_rim, coin_h / 2, 0);
    lv_obj_set_pos(s_coin_rim, (140 - coin_w) / 2, (100 - coin_h) / 2 + 2);

    // 金币主体
    lv_obj_set_size(s_coin_body, coin_w, coin_h);
    lv_obj_set_style_radius(s_coin_body, coin_h / 2, 0);
    lv_obj_set_pos(s_coin_body, (140 - coin_w) / 2, (100 - coin_h) / 2);
    lv_obj_set_style_bg_color(s_coin_body, cur_side ? lv_color_hex(0xFFD700) : lv_color_hex(0xF4A460), 0);

    // 金币内环
    if (coin_h >= 24) {
        lv_obj_remove_flag(s_coin_inner, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_coin_inner, coin_w - 14, coin_h - 10);
        lv_obj_set_style_radius(s_coin_inner, (coin_h - 10) / 2, 0);
        lv_obj_set_style_bg_color(s_coin_inner, cur_side ? lv_color_hex(0xFFE040) : lv_color_hex(0xFFA07A), 0);
    } else {
        lv_obj_add_flag(s_coin_inner, LV_OBJ_FLAG_HIDDEN);
    }

    // 金币文字 (仅在展开时显示)
    if (coin_h >= 36) {
        lv_obj_remove_flag(s_coin_text, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_coin_text, cur_side ? "YES" : "NO");
        lv_obj_set_style_text_color(s_coin_text, cur_side ? lv_color_hex(0x8B6508) : lv_color_hex(0x8B2500), 0);
    } else {
        lv_obj_add_flag(s_coin_text, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_top_stats(void) {
    if (!s_top_info) return;
    lv_label_set_text_fmt(s_top_info, "总掷:%d | YES:%d | NO:%d",
                          s_total_spins, s_yes_count, s_no_count);
}

static void flip_timer_cb(lv_timer_t *timer) {
    (void)timer;
    s_anim_step++;
    s_flip_phase = (s_flip_phase + 1) % 8;

    update_coin_geometry(s_flip_phase, s_current_result);

    // 倒计时减速并定格
    if (s_anim_step >= s_total_steps && (s_flip_phase == 0)) {
        lv_timer_del(s_flip_timer);
        s_flip_timer = NULL;
        s_state = COIN_STATE_RESULT;

        s_total_spins++;
        if (s_current_result) s_yes_count++;
        else s_no_count++;
        update_top_stats();

        update_coin_geometry(0, s_current_result);

        // 最终大字与评语
        if (s_current_result) {
            lv_label_set_text(s_result_label, "【 YES 】");
            lv_obj_set_style_text_color(s_result_label, lv_color_hex(0x1B8A2B), 0);
            int idx = esp_random() % 5;
            lv_label_set_text(s_comment_label, YES_COMMENTS[idx]);
        } else {
            lv_label_set_text(s_result_label, "【 NO 】");
            lv_obj_set_style_text_color(s_result_label, lv_color_hex(0xD32F2F), 0);
            int idx = esp_random() % 5;
            lv_label_set_text(s_comment_label, NO_COMMENTS[idx]);
        }

        ui_pixel_mascot_jump(s_mascot);
        lv_label_set_text(s_hint_label, "按 OK 键再次抛掷，长按返回");
    }
}

static void start_flip(void) {
    if (s_state == COIN_STATE_FLIPPING) return;

    s_state = COIN_STATE_FLIPPING;
    s_current_result = (esp_random() % 2 == 0) ? 1 : 0; // 纯随机决定最终结果

    s_anim_step = 0;
    // 旋转 18~24 步（约 1.5~2 秒流畅翻转）
    s_total_steps = 18 + (esp_random() % 7);
    s_flip_phase = 0;

    lv_label_set_text(s_result_label, "命运旋转中...");
    lv_obj_set_style_text_color(s_result_label, lv_color_hex(UI_INK), 0);
    lv_label_set_text(s_comment_label, "正在聆听宇宙的回答...");
    lv_label_set_text(s_hint_label, "硬币正在翻转...");

    ui_pixel_mascot_jump(s_mascot);

    if (s_flip_timer) {
        lv_timer_del(s_flip_timer);
    }
    s_flip_timer = lv_timer_create(flip_timer_cb, 60, NULL);
}

void demo_coin_enter(void) {
    s_state = COIN_STATE_IDLE;
    s_scr = ui_pixel_screen_create("决策硬币");

    // 顶部统计栏
    lv_obj_t *top_card = ui_pixel_panel_create(s_scr, 10, 42, 220, 26, UI_PAPER);
    s_top_info = lv_label_create(top_card);
    lv_obj_set_style_text_font(s_top_info, &font_chinese_14, 0);
    lv_obj_center(s_top_info);
    update_top_stats();

    // 中间 3D 硬币展示框
    lv_obj_t *center_panel = ui_pixel_panel_create(s_scr, 10, 72, 220, 116, UI_PAPER);
    s_coin_box = lv_obj_create(center_panel);
    lv_obj_remove_flag(s_coin_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_coin_box, 140, 100);
    lv_obj_set_style_radius(s_coin_box, 0, 0);
    lv_obj_set_style_border_width(s_coin_box, 0, 0);
    lv_obj_set_style_pad_all(s_coin_box, 0, 0);
    lv_obj_set_style_bg_opa(s_coin_box, LV_OPA_TRANSP, 0);
    lv_obj_center(s_coin_box);

    // 阴影
    s_coin_shadow = lv_obj_create(s_coin_box);
    lv_obj_remove_flag(s_coin_shadow, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_coin_shadow, 0, 0);
    lv_obj_set_style_pad_all(s_coin_shadow, 0, 0);
    lv_obj_set_style_bg_color(s_coin_shadow, lv_color_hex(0xD8D8C8), 0);

    // 金币外圈立体厚度边
    s_coin_rim = lv_obj_create(s_coin_box);
    lv_obj_remove_flag(s_coin_rim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_coin_rim, 0, 0);
    lv_obj_set_style_pad_all(s_coin_rim, 0, 0);
    lv_obj_set_style_bg_color(s_coin_rim, lv_color_hex(0xC99700), 0);

    // 金币主体
    s_coin_body = lv_obj_create(s_coin_box);
    lv_obj_remove_flag(s_coin_body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_coin_body, 2, 0);
    lv_obj_set_style_border_color(s_coin_body, lv_color_hex(0xB8860B), 0);
    lv_obj_set_style_pad_all(s_coin_body, 0, 0);

    // 金币内环
    s_coin_inner = lv_obj_create(s_coin_body);
    lv_obj_remove_flag(s_coin_inner, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(s_coin_inner, 1, 0);
    lv_obj_set_style_border_color(s_coin_inner, lv_color_hex(0xDAA520), 0);
    lv_obj_set_style_pad_all(s_coin_inner, 0, 0);
    lv_obj_center(s_coin_inner);

    // 金币文字
    s_coin_text = lv_label_create(s_coin_inner);
    lv_obj_set_style_text_font(s_coin_text, &lv_font_montserrat_20, 0);
    lv_obj_center(s_coin_text);

    // 初始静止画面：满圆正面 YES
    update_coin_geometry(0, 1);

    // 下方结果显示面板
    lv_obj_t *result_panel = ui_pixel_panel_create(s_scr, 10, 192, 220, 68, UI_PAPER);

    s_result_label = lv_label_create(result_panel);
    lv_obj_set_style_text_font(s_result_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_result_label, lv_color_hex(UI_INK), 0);
    lv_label_set_text(s_result_label, "YES / NO");
    lv_obj_align(s_result_label, LV_ALIGN_TOP_MID, 0, 4);

    s_comment_label = lv_label_create(result_panel);
    lv_obj_set_style_text_font(s_comment_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_comment_label, lv_color_hex(UI_INK), 0);
    lv_label_set_text(s_comment_label, "心中默念问题，按下 OK 键");
    lv_obj_align(s_comment_label, LV_ALIGN_BOTTOM_MID, 0, -4);

    // 底部提示
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xFFFFFF), 0);
    lv_label_set_text(s_hint_label, "按 OK 键抛掷 | 长按返回");
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    // 吉祥物
    s_mascot = ui_pixel_mascot_create(s_scr, 101, 262);

    lv_screen_load(s_scr);
}

void demo_coin_exit(void) {
    if (s_flip_timer) {
        lv_timer_del(s_flip_timer);
        s_flip_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_coin_box = NULL;
        s_mascot = NULL;
    }
}

void demo_coin_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        if (s_state != COIN_STATE_FLIPPING) {
            start_flip();
        }
    } else if (btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
        if (s_state != COIN_STATE_FLIPPING) {
            ui_pixel_mascot_jump(s_mascot);
        }
    }
}
