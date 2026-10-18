// main/demo_rps.c —— 002 剪子包袱锤 (Rock Paper Scissors with Multi-Difficulty)
#include "demo.h"
#include "ui_pixel.h"
#include "font_chinese_14.h"
#include "lvgl.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdbool.h>

typedef enum {
    CHOICE_SCISSORS = 0, // 剪刀
    CHOICE_ROCK     = 1, // 石头
    CHOICE_PAPER    = 2, // 布
    CHOICE_COUNT    = 3
} rps_choice_t;

typedef enum {
    DIFF_EASY = 0,   // 简单模式：电脑偶发放水，胜率高
    DIFF_NORMAL = 1, // 普通模式：经典公平，1/3 纯随机盲猜
    DIFF_HARD = 2,   // 困难模式：心理预判与习惯克制
    DIFF_COUNT = 3
} rps_difficulty_t;

static const char *DIFF_NAMES[] = { "简单", "普通", "困难" };
static const char *DIFF_TAGS[]  = { "【简单难度】", "【普通难度】", "【困难难度】" };
static const char *CHOICE_NAMES[] = { "剪刀", "石头", "布" };

typedef enum {
    RPS_STATE_CHOOSING = 0, // 玩家挑选阶段，电脑在暗中已选好
    RPS_STATE_REVEALED       // 双方开牌亮出，判定胜负
} rps_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_panel;
static lv_obj_t *s_top_score_label;
static lv_obj_t *s_header_label;
static lv_obj_t *s_cards[CHOICE_COUNT];
static lv_obj_t *s_mascot;

// 结果区域
static lv_obj_t *s_vs_label;
static lv_obj_t *s_outcome_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_hint_label;

static rps_state_t s_state = RPS_STATE_CHOOSING;
static rps_difficulty_t s_diff = DIFF_NORMAL; // 默认普通模式
static int s_user_choice = 1; // 默认选中石头
static int s_ai_choice = 0;   // 电脑暗中选好的结果

// 玩家历史统计（用于困难模式智能分析）
static int s_history_counts[CHOICE_COUNT] = { 0 };
static int s_last_user_choice = -1;
static int s_last_ai_choice = -1;
static int s_last_result = 0; // 1: user win, -1: user lose, 0: draw

static int s_win_count = 0;
static int s_lose_count = 0;
static int s_draw_count = 0;

static lv_obj_t *pixel_rect(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    return obj;
}

// 绘制像素风格图形：剪刀（包围盒 30x34，ox, oy 为包围盒左上角）
static void draw_scissors_icon(lv_obj_t *parent, int ox, int oy) {
    // 刀把两环
    pixel_rect(parent, ox + 0, oy + 22, 12, 12, UI_RED);
    pixel_rect(parent, ox + 3, oy + 25, 6, 6, UI_PAPER);

    pixel_rect(parent, ox + 18, oy + 22, 12, 12, UI_RED);
    pixel_rect(parent, ox + 21, oy + 25, 6, 6, UI_PAPER);

    // 交叉轴
    pixel_rect(parent, ox + 12, oy + 16, 6, 6, UI_INK);

    // 左右两片银色刀刃
    pixel_rect(parent, ox + 4, oy + 0, 6, 18, 0x8899A6);
    pixel_rect(parent, ox + 2, oy + 2, 4, 12, 0xCCD6DD);

    pixel_rect(parent, ox + 20, oy + 0, 6, 18, 0x8899A6);
    pixel_rect(parent, ox + 24, oy + 2, 4, 12, 0xCCD6DD);
}

// 绘制像素风格图形：石头（包围盒 28x30，ox, oy 为包围盒左上角）
static void draw_rock_icon(lv_obj_t *parent, int ox, int oy) {
    pixel_rect(parent, ox + 0, oy + 2, 28, 26, UI_INK);
    pixel_rect(parent, ox + 2, oy + 0, 24, 30, UI_INK);

    pixel_rect(parent, ox + 3, oy + 3, 22, 24, 0x7E8B92);
    pixel_rect(parent, ox + 5, oy + 3, 12, 10, 0xBAC3C7);
    pixel_rect(parent, ox + 12, oy + 13, 10, 11, 0x5C686E);
    pixel_rect(parent, ox + 7, oy + 16, 6, 4, 0x4B555A);
}

// 绘制像素风格图形：布（包围盒 28x28，ox, oy 为包围盒左上角）
static void draw_paper_icon(lv_obj_t *parent, int ox, int oy) {
    pixel_rect(parent, ox + 0, oy + 0, 28, 28, UI_INK);
    pixel_rect(parent, ox + 2, oy + 2, 24, 24, 0xFFE082);
    pixel_rect(parent, ox + 2, oy + 2, 24, 4, 0xFFF9C4);
    pixel_rect(parent, ox + 13, oy + 2, 2, 24, 0xF57F17);
    pixel_rect(parent, ox + 2, oy + 13, 24, 2, 0xF57F17);
    pixel_rect(parent, ox + 20, oy + 20, 6, 6, 0xE65100);
}

// 根据当前难度与历史，生成电脑暗中出拳
static int generate_ai_choice(void) {
    uint32_t r = esp_random() % 100;

    if (s_diff == DIFF_EASY) {
        // 简单模式：电脑温和放水（60% 出玩家克制的牌，20% 平，20% 赢）
        // 预判玩家当前停留的牌
        int target = s_user_choice;
        if (r < 60) {
            // 出被 target 克制的牌让玩家赢
            if (target == CHOICE_SCISSORS) return CHOICE_PAPER;
            if (target == CHOICE_ROCK)     return CHOICE_SCISSORS;
            return CHOICE_ROCK;
        } else if (r < 80) {
            // 平局
            return target;
        } else {
            // 电脑赢
            if (target == CHOICE_SCISSORS) return CHOICE_ROCK;
            if (target == CHOICE_ROCK)     return CHOICE_PAPER;
            return CHOICE_SCISSORS;
        }
    } else if (s_diff == DIFF_NORMAL) {
        // 普通模式：完全公平的 1/3 随机
        return (int)(esp_random() % CHOICE_COUNT);
    } else {
        // 困难模式：心理博弈与统计克制
        // 1. 如果有历史局，利用胜负心理学：
        // 赢家倾向继续出同一手；输家倾向变阵（换成克制赢家上一把的手）
        if (s_last_user_choice >= 0 && (r < 70)) {
            int predicted_user_move = -1;
            if (s_last_result == 1) {
                // 上把玩家赢了，大概率会继续出同一手
                predicted_user_move = s_last_user_choice;
            } else if (s_last_result == -1) {
                // 上把玩家输了，会换出能克制电脑上一把的牌
                if (s_last_ai_choice == CHOICE_SCISSORS) predicted_user_move = CHOICE_ROCK;
                else if (s_last_ai_choice == CHOICE_ROCK) predicted_user_move = CHOICE_PAPER;
                else predicted_user_move = CHOICE_SCISSORS;
            } else {
                // 上把平局，玩家倾向换牌
                predicted_user_move = (s_last_user_choice + 1) % CHOICE_COUNT;
            }

            // 电脑克制该预判
            if (predicted_user_move == CHOICE_SCISSORS) return CHOICE_ROCK;
            if (predicted_user_move == CHOICE_ROCK)     return CHOICE_PAPER;
            return CHOICE_SCISSORS;
        }

        // 2. 根据玩家历史频率最高的出拳进行针对克制
        int max_c = 0;
        int favored = 0;
        for (int i = 0; i < CHOICE_COUNT; i++) {
            if (s_history_counts[i] > max_c) {
                max_c = s_history_counts[i];
                favored = i;
            }
        }
        if (max_c > 0 && (r < 50)) {
            if (favored == CHOICE_SCISSORS) return CHOICE_ROCK;
            if (favored == CHOICE_ROCK)     return CHOICE_PAPER;
            return CHOICE_SCISSORS;
        }

        return (int)(esp_random() % CHOICE_COUNT);
    }
}

static void update_display(void) {
    for (int i = 0; i < CHOICE_COUNT; i++) {
        if (s_state == RPS_STATE_CHOOSING) {
            ui_pixel_set_selected(s_cards[i], i == s_user_choice, true);
        } else {
            bool is_user = (i == s_user_choice);
            bool is_ai   = (i == s_ai_choice);
            ui_pixel_set_selected(s_cards[i], is_user || is_ai, true);
        }
    }

    // 更新顶部比分栏与难度
    char top_buf[64];
    snprintf(top_buf, sizeof(top_buf), "比分: 你 %d - %d 电脑 (%d平) %s",
             s_win_count, s_lose_count, s_draw_count, DIFF_NAMES[s_diff]);
    lv_label_set_text(s_top_score_label, top_buf);

    char header_buf[64];
    if (s_state == RPS_STATE_CHOOSING) {
        snprintf(header_buf, sizeof(header_buf), "%s 长按上键换难度", DIFF_TAGS[s_diff]);
        lv_label_set_text(s_header_label, header_buf);
        lv_label_set_text(s_vs_label, "对决准备中...");
        lv_label_set_text(s_outcome_label, "电脑已在暗中秘密就绪");
        lv_obj_set_style_text_color(s_outcome_label, lv_color_hex(UI_INK), 0);
        lv_label_set_text(s_detail_label, "不要被偷看哦，请选好后按确定");
        lv_label_set_text(s_hint_label, "UP/DOWN 选牌  ·  OK 确定开牌");
    } else {
        snprintf(header_buf, sizeof(header_buf), "%s 按确定再来一局", DIFF_TAGS[s_diff]);
        lv_label_set_text(s_header_label, header_buf);

        char vs_buf[64];
        snprintf(vs_buf, sizeof(vs_buf), "你出: %s   VS   电脑: %s",
                 CHOICE_NAMES[s_user_choice], CHOICE_NAMES[s_ai_choice]);
        lv_label_set_text(s_vs_label, vs_buf);

        if (s_user_choice == s_ai_choice) {
            lv_label_set_text(s_outcome_label, "【双方平手】 势均力敌！");
            lv_obj_set_style_text_color(s_outcome_label, lv_color_hex(0x0872C9), 0);
            lv_label_set_text(s_detail_label, "心有灵犀，双方出拳一致");
        } else if ((s_user_choice == CHOICE_SCISSORS && s_ai_choice == CHOICE_PAPER) ||
                   (s_user_choice == CHOICE_ROCK && s_ai_choice == CHOICE_SCISSORS) ||
                   (s_user_choice == CHOICE_PAPER && s_ai_choice == CHOICE_ROCK)) {
            lv_label_set_text(s_outcome_label, "【恭喜你赢了】 获胜！");
            lv_obj_set_style_text_color(s_outcome_label, lv_color_hex(0x1B8A2B), 0);
            if (s_user_choice == CHOICE_SCISSORS) {
                lv_label_set_text(s_detail_label, "剪刀剪断了布！");
            } else if (s_user_choice == CHOICE_ROCK) {
                lv_label_set_text(s_detail_label, "石头把剪刀打碎了！");
            } else {
                lv_label_set_text(s_detail_label, "布紧紧包住了石头！");
            }
            ui_pixel_mascot_jump(s_mascot);
        } else {
            lv_label_set_text(s_outcome_label, "【很遗憾你输了】 惜败！");
            lv_obj_set_style_text_color(s_outcome_label, lv_color_hex(UI_RED), 0);
            if (s_ai_choice == CHOICE_SCISSORS) {
                lv_label_set_text(s_detail_label, "电脑剪刀剪断了你的布！");
            } else if (s_ai_choice == CHOICE_ROCK) {
                lv_label_set_text(s_detail_label, "电脑石头把你的剪刀打碎了！");
            } else {
                lv_label_set_text(s_detail_label, "电脑布紧紧包住了你的石头！");
            }
        }
        lv_label_set_text(s_hint_label, "按确定键开启下一局！");
    }
}

static void start_new_round(void) {
    s_state = RPS_STATE_CHOOSING;
    s_ai_choice = generate_ai_choice();
    update_display();
}

void demo_rps_enter(void) {
    s_state = RPS_STATE_CHOOSING;
    s_user_choice = 1; // 默认停留石头
    s_ai_choice = generate_ai_choice();

    s_scr = ui_pixel_screen_create("剪子包袱锤");

    // 屏幕顶部比分横幅（常驻在标题下方，醒目展示对局积分）
    lv_obj_t *score_bar = lv_obj_create(s_scr);
    lv_obj_remove_flag(score_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(score_bar, 220, 24);
    lv_obj_set_pos(score_bar, 10, 44);
    lv_obj_set_style_radius(score_bar, 4, 0);
    lv_obj_set_style_bg_color(score_bar, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_border_color(score_bar, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(score_bar, 2, 0);
    lv_obj_set_style_pad_all(score_bar, 0, 0);

    s_top_score_label = lv_label_create(score_bar);
    lv_obj_set_style_text_font(s_top_score_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_top_score_label, lv_color_hex(UI_INK), 0);
    lv_obj_center(s_top_score_label);

    // 状态提示标语
    s_header_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_header_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_header_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_header_label, LV_ALIGN_TOP_MID, 0, 70);

    // 中部：三张像素图形卡片（剪刀、石头、布）—— 纯图形展示，取消文字标签
    int card_w = 64;
    int card_h = 60;
    int start_x = 16;
    int gap = 8;
    int card_y = 96;

    for (int i = 0; i < CHOICE_COUNT; i++) {
        int cx = start_x + i * (card_w + gap);
        s_cards[i] = ui_pixel_panel_create(s_scr, cx, card_y, card_w, card_h, UI_PAPER);
        // 清除内边距，使内部像素坐标以卡片内沿严格对齐
        lv_obj_set_style_pad_all(s_cards[i], 0, 0);

        if (i == CHOICE_SCISSORS) {
            draw_scissors_icon(s_cards[i], 13, 9);
        } else if (i == CHOICE_ROCK) {
            draw_rock_icon(s_cards[i], 14, 11);
        } else {
            draw_paper_icon(s_cards[i], 14, 12);
        }
    }

    // 下方对决信息底板
    s_panel = ui_pixel_panel_create(s_scr, 12, 168, 216, 76, UI_PAPER);

    s_vs_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_vs_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_vs_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(s_vs_label, LV_ALIGN_TOP_MID, 0, 4);

    s_outcome_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_outcome_label, &font_chinese_14, 0);
    lv_obj_align(s_outcome_label, LV_ALIGN_TOP_MID, 0, 24);

    s_detail_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_detail_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x666666), 0);
    lv_obj_align(s_detail_label, LV_ALIGN_TOP_MID, 0, 44);

    // 底部操作提示
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -22);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 246);

    update_display();
    lv_screen_load(s_scr);
}

void demo_rps_exit(void) {
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_mascot = NULL;
    }
}

void demo_rps_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    // 切换难度：支持长按 UP，或者双击 UP / DOWN
    if ((btn == BSP_BTN_UP && (ev == BSP_BTN_LONG || ev == BSP_BTN_DOUBLE)) ||
        (btn == BSP_BTN_DOWN && ev == BSP_BTN_DOUBLE)) {
        s_diff = (s_diff + 1) % DIFF_COUNT;
        ui_pixel_mascot_jump(s_mascot);
        // 难度变动后，重新暗中洗牌
        s_ai_choice = generate_ai_choice();
        update_display();
        return;
    }

    if (ev != BSP_BTN_CLICK) return;

    if (s_state == RPS_STATE_CHOOSING) {
        if (btn == BSP_BTN_UP) {
            // 向左选牌
            s_user_choice = (s_user_choice + CHOICE_COUNT - 1) % CHOICE_COUNT;
            ui_pixel_mascot_jump(s_mascot);
            // 简单模式下根据当前停留动态适配放水
            if (s_diff == DIFF_EASY) {
                s_ai_choice = generate_ai_choice();
            }
            update_display();
        } else if (btn == BSP_BTN_DOWN) {
            // 向右选牌
            s_user_choice = (s_user_choice + 1) % CHOICE_COUNT;
            ui_pixel_mascot_jump(s_mascot);
            if (s_diff == DIFF_EASY) {
                s_ai_choice = generate_ai_choice();
            }
            update_display();
        } else if (btn == BSP_BTN_OK) {
            // 开牌
            s_state = RPS_STATE_REVEALED;
            s_history_counts[s_user_choice]++;
            s_last_user_choice = s_user_choice;
            s_last_ai_choice = s_ai_choice;

            if (s_user_choice == s_ai_choice) {
                s_draw_count++;
                s_last_result = 0;
            } else if ((s_user_choice == CHOICE_SCISSORS && s_ai_choice == CHOICE_PAPER) ||
                       (s_user_choice == CHOICE_ROCK && s_ai_choice == CHOICE_SCISSORS) ||
                       (s_user_choice == CHOICE_PAPER && s_ai_choice == CHOICE_ROCK)) {
                s_win_count++;
                s_last_result = 1;
            } else {
                s_lose_count++;
                s_last_result = -1;
            }
            update_display();
        }
    } else if (s_state == RPS_STATE_REVEALED) {
        if (btn == BSP_BTN_OK || btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            start_new_round();
            ui_pixel_mascot_jump(s_mascot);
        }
    }
}
