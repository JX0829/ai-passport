// main/demo_ttt.c —— 003 井字棋 (Tic-Tac-Toe: 3/4 Pieces FIFO, BO3, 2min Round Timer)
#include "demo.h"
#include "ui_pixel.h"
#include "font_chinese_14.h"
#include "lvgl.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdbool.h>

#define BOARD_SIZE 9
#define ROUND_MAX_SECONDS 120 // 每局 2 分钟

typedef enum {
    CELL_EMPTY = 0,
    CELL_PLAYER = 1, // 玩家棋子
    CELL_AI     = 2  // 电脑棋子
} ttt_cell_t;

typedef enum {
    STAGE_SETUP = 0,    // 初始配置页
    STAGE_PLAYING,      // 对战阶段
    STAGE_ROUND_OVER,   // 单局结束
    STAGE_MATCH_OVER    // 三局两胜最终决胜结束
} ttt_stage_t;

typedef enum {
    TTT_DIFF_EASY   = 0, // 简单
    TTT_DIFF_NORMAL = 1, // 普通
    TTT_DIFF_HARD   = 2, // 不败
    TTT_DIFF_COUNT  = 3
} ttt_diff_t;

typedef enum {
    SIDE_BLUE = 0, // 蓝旗 (限 3 颗子)
    SIDE_RED  = 1, // 红旗 (限 4 颗子)
    SIDE_COUNT = 2
} ttt_side_t;

static const char *DIFF_NAMES[] = { "简单", "普通", "不败" };
static const char *SIDE_NAMES[] = { "蓝旗 (限3子)", "红旗 (限4子)" };

static lv_obj_t *s_scr;
static lv_obj_t *s_setup_root = NULL;
static lv_obj_t *s_play_root  = NULL;

// 初始配置页控件
static lv_obj_t *s_setup_box;
static lv_obj_t *s_setup_diff_card;
static lv_obj_t *s_setup_diff_label;
static lv_obj_t *s_setup_side_card;
static lv_obj_t *s_setup_side_label;
static lv_obj_t *s_setup_start_card;
static lv_obj_t *s_setup_start_label;
static lv_obj_t *s_setup_hint_label;
static int s_setup_item = 0; // 0: 难度, 1: 阵营, 2: 开始游戏

// 游戏页控件
static lv_obj_t *s_top_score_label;
static lv_obj_t *s_cells[BOARD_SIZE];
static lv_obj_t *s_panel;
static lv_obj_t *s_status_label;
static lv_obj_t *s_timer_label;
static lv_obj_t *s_detail_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_mascot;

static lv_timer_t *s_sec_timer = NULL;

static ttt_stage_t s_stage = STAGE_SETUP;
static ttt_diff_t  s_diff  = TTT_DIFF_NORMAL;
static ttt_side_t  s_side  = SIDE_BLUE;

static ttt_cell_t s_board[BOARD_SIZE];
static int s_cursor = 4; // 光标位置

// 先进先出队列记录棋子历史：满额后再下，最早的自动消除
static int s_player_queue[5];
static int s_player_count = 0;
static int s_player_max = 3;

static int s_ai_queue[5];
static int s_ai_count = 0;
static int s_ai_max = 4;

// 三局两胜制（BO3）数据
static int s_current_round = 1;     // 当前第几局 (1, 2, 3)
static int s_player_round_wins = 0; // 玩家赢得局数
static int s_ai_round_wins = 0;     // 电脑赢得局数
static int s_round_draws = 0;       // 平局数

// 时间控制
static int s_round_time_left = ROUND_MAX_SECONDS; // 每局 2 分钟 (120秒)
static int s_step_countdown = 10;                 // 玩家单步 10 秒；电脑思考 5 秒
static bool s_is_ai_turn = false;                 // 是否轮到电脑
static int s_round_winner = 0;                    // 0: 游戏中, 1: 玩家胜, 2: 电脑胜, 3: 平局

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

// 绘制像素风格图案：X (蓝)
static void draw_x_icon(lv_obj_t *parent, uint32_t color) {
    for (int i = 0; i < 22; i++) {
        pixel_rect(parent, 11 + i, 9 + i, 3, 3, color);
        pixel_rect(parent, 32 - i, 9 + i, 3, 3, color);
    }
}

// 绘制像素风格图案：O (红)
static void draw_o_icon(lv_obj_t *parent, uint32_t color) {
    pixel_rect(parent, 15, 9, 16, 4, color);
    pixel_rect(parent, 15, 29, 16, 4, color);
    pixel_rect(parent, 10, 14, 4, 14, color);
    pixel_rect(parent, 32, 14, 4, 14, color);

    pixel_rect(parent, 12, 11, 4, 4, color);
    pixel_rect(parent, 30, 11, 4, 4, color);
    pixel_rect(parent, 12, 27, 4, 4, color);
    pixel_rect(parent, 30, 27, 4, 4, color);
}

// 胜负检查
static int check_winner(const ttt_cell_t *b) {
    static const int LINES[8][3] = {
        {0,1,2}, {3,4,5}, {6,7,8}, // 横
        {0,3,6}, {1,4,7}, {2,5,8}, // 竖
        {0,4,8}, {2,4,6}           // 斜
    };
    for (int i = 0; i < 8; i++) {
        int c0 = LINES[i][0], c1 = LINES[i][1], c2 = LINES[i][2];
        if (b[c0] != CELL_EMPTY && b[c0] == b[c1] && b[c1] == b[c2]) {
            return (b[c0] == CELL_PLAYER) ? 1 : 2;
        }
    }
    return 0; // 未分出三子一线
}

// 玩家落子并维护 3/4 颗子先进先出消除
static void place_player_piece(int pos) {
    if (s_player_count >= s_player_max) {
        // 最早下的棋子消除
        int oldest = s_player_queue[0];
        s_board[oldest] = CELL_EMPTY;
        for (int i = 0; i < s_player_count - 1; i++) {
            s_player_queue[i] = s_player_queue[i + 1];
        }
        s_player_count--;
    }
    s_board[pos] = CELL_PLAYER;
    s_player_queue[s_player_count++] = pos;
}

// 电脑落子并维护 3/4 颗子先进先出消除
static void place_ai_piece(int pos) {
    if (s_ai_count >= s_ai_max) {
        // 最早下的棋子消除
        int oldest = s_ai_queue[0];
        s_board[oldest] = CELL_EMPTY;
        for (int i = 0; i < s_ai_count - 1; i++) {
            s_ai_queue[i] = s_ai_queue[i + 1];
        }
        s_ai_count--;
    }
    s_board[pos] = CELL_AI;
    s_ai_queue[s_ai_count++] = pos;
}

// 电脑选点算法（结合消除机制）
static int ai_choose_move(void) {
    // 1. 尝试找一步制胜点
    for (int i = 0; i < BOARD_SIZE; i++) {
        if (s_board[i] == CELL_EMPTY) {
            // 模拟假设
            ttt_cell_t temp_board[BOARD_SIZE];
            for (int k = 0; k < BOARD_SIZE; k++) temp_board[k] = s_board[k];
            if (s_ai_count >= s_ai_max) {
                temp_board[s_ai_queue[0]] = CELL_EMPTY;
            }
            temp_board[i] = CELL_AI;
            if (check_winner(temp_board) == 2) return i;
        }
    }

    // 2. 尝试堵住玩家制胜点
    if (s_diff >= TTT_DIFF_NORMAL) {
        for (int i = 0; i < BOARD_SIZE; i++) {
            if (s_board[i] == CELL_EMPTY) {
                ttt_cell_t temp_board[BOARD_SIZE];
                for (int k = 0; k < BOARD_SIZE; k++) temp_board[k] = s_board[k];
                if (s_player_count >= s_player_max) {
                    temp_board[s_player_queue[0]] = CELL_EMPTY;
                }
                temp_board[i] = CELL_PLAYER;
                if (check_winner(temp_board) == 1) return i;
            }
        }
    }

    // 3. 不败/困难模式：优先抢占中心天元 (4) 或 角落
    if (s_diff == TTT_DIFF_HARD) {
        if (s_board[4] == CELL_EMPTY) return 4;
        static const int CORNERS[] = { 0, 2, 6, 8 };
        int available_corners[4];
        int ac_cnt = 0;
        for (int c = 0; c < 4; c++) {
            if (s_board[CORNERS[c]] == CELL_EMPTY) available_corners[ac_cnt++] = CORNERS[c];
        }
        if (ac_cnt > 0 && (esp_random() % 100 < 80)) {
            return available_corners[esp_random() % ac_cnt];
        }
    }

    // 4. 随机落子
    int empty_cells[BOARD_SIZE];
    int count = 0;
    for (int i = 0; i < BOARD_SIZE; i++) {
        if (s_board[i] == CELL_EMPTY) empty_cells[count++] = i;
    }
    if (count == 0) return -1;
    return empty_cells[esp_random() % count];
}

static void redraw_board_cells(void) {
    for (int i = 0; i < BOARD_SIZE; i++) {
        lv_obj_clean(s_cells[i]);

        if (s_board[i] == CELL_PLAYER) {
            if (s_side == SIDE_BLUE) draw_x_icon(s_cells[i], 0x1689E8); // 玩家蓝X
            else draw_o_icon(s_cells[i], 0xE43B2F);                      // 玩家红O
        } else if (s_board[i] == CELL_AI) {
            if (s_side == SIDE_BLUE) draw_o_icon(s_cells[i], 0xE43B2F); // 电脑红O
            else draw_x_icon(s_cells[i], 0x1689E8);                      // 电脑蓝X
        }

        // 光标选中高亮
        bool is_cur = (i == s_cursor && s_stage == STAGE_PLAYING && !s_is_ai_turn);
        ui_pixel_set_selected(s_cells[i], is_cur, true);
    }
}

static void update_play_display(void) {
    // 顶部局数与局比分：三局两胜 (BO3)
    char score_buf[64];
    snprintf(score_buf, sizeof(score_buf), "第%d/3局 你%d-%d电脑 (局时:%02d:%02d)",
             s_current_round, s_player_round_wins, s_ai_round_wins,
             s_round_time_left / 60, s_round_time_left % 60);
    lv_label_set_text(s_top_score_label, score_buf);

    redraw_board_cells();

    char status_buf[64];
    char timer_buf[48];

    if (s_stage == STAGE_PLAYING) {
        if (!s_is_ai_turn) {
            // 轮到玩家
            snprintf(status_buf, sizeof(status_buf), "轮到你落子 (盘上:%d/%d颗)",
                     s_player_count, s_player_max);
            lv_label_set_text(s_status_label, status_buf);
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(s_side == SIDE_BLUE ? 0x1689E8 : 0xE43B2F), 0);

            snprintf(timer_buf, sizeof(timer_buf), "思考限时: %d 秒", s_step_countdown);
            lv_label_set_text(s_timer_label, timer_buf);
            lv_obj_set_style_text_color(s_timer_label, lv_color_hex(s_step_countdown <= 3 ? UI_RED : UI_INK), 0);

            if (s_player_count >= s_player_max) {
                lv_label_set_text(s_detail_label, "棋子已满额，再下一颗将消除最先下的子");
            } else {
                lv_label_set_text(s_detail_label, "UP/DOWN 选格，OK 确认落子");
            }
            lv_label_set_text(s_hint_label, "超时将自动为你落子 · 满额自动消除最旧子");
        } else {
            // 电脑思考（5秒）
            snprintf(status_buf, sizeof(status_buf), "电脑思考中 (对手盘上:%d/%d颗)",
                     s_ai_count, s_ai_max);
            lv_label_set_text(s_status_label, status_buf);
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x666666), 0);

            snprintf(timer_buf, sizeof(timer_buf), "对手将在 %d 秒后落子", s_step_countdown);
            lv_label_set_text(s_timer_label, timer_buf);
            lv_obj_set_style_text_color(s_timer_label, lv_color_hex(UI_ORANGE), 0);

            lv_label_set_text(s_detail_label, "观察对手走位与消除变化");
            lv_label_set_text(s_hint_label, "隔 5 秒电脑落子");
        }
    } else if (s_stage == STAGE_ROUND_OVER) {
        // 单局结束
        lv_label_set_text(s_timer_label, "本局时间到或已分胜负");
        lv_obj_set_style_text_color(s_timer_label, lv_color_hex(UI_INK), 0);

        if (s_round_winner == 1) {
            lv_label_set_text(s_status_label, "【恭喜拿下本局】三子连线！");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x1B8A2B), 0);
            lv_label_set_text(s_detail_label, "漂亮的连线！成功斩获 1 胜！");
            ui_pixel_mascot_jump(s_mascot);
        } else if (s_round_winner == 2) {
            lv_label_set_text(s_status_label, "【本局惜败】电脑连成一线！");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(UI_RED), 0);
            lv_label_set_text(s_detail_label, "电脑先下一城，下一局加油！");
        } else {
            lv_label_set_text(s_status_label, "【两分钟用完】本局平局！");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x0872C9), 0);
            lv_label_set_text(s_detail_label, "防守严密，双方战平！");
        }
        lv_label_set_text(s_hint_label, "按确定键开启下一局！");
    } else if (s_stage == STAGE_MATCH_OVER) {
        // 三局两胜决胜结束
        lv_label_set_text(s_timer_label, "三局两胜对决圆满结束");
        lv_obj_set_style_text_color(s_timer_label, lv_color_hex(UI_INK), 0);

        if (s_player_round_wins >= 2 || (s_player_round_wins > s_ai_round_wins)) {
            lv_label_set_text(s_status_label, "【夺得大胜利】恭喜拿下整场！");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x1B8A2B), 0);
            lv_label_set_text(s_detail_label, "先赢两局，成为井字棋大师！");
            ui_pixel_mascot_jump(s_mascot);
        } else if (s_ai_round_wins >= 2 || (s_ai_round_wins > s_player_round_wins)) {
            lv_label_set_text(s_status_label, "【遗憾惜败】电脑拿下对决！");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(UI_RED), 0);
            lv_label_set_text(s_detail_label, "电脑胜出，再来一场复仇战吧！");
        } else {
            lv_label_set_text(s_status_label, "【势均力敌】整场双方战平！");
            lv_obj_set_style_text_color(s_status_label, lv_color_hex(0x0872C9), 0);
            lv_label_set_text(s_detail_label, "大比分战平，不分胜负！");
        }
        lv_label_set_text(s_hint_label, "按确定键返回初始配置！");
    }
}

static void update_setup_display(void) {
    ui_pixel_set_selected(s_setup_diff_card, s_setup_item == 0, true);
    ui_pixel_set_selected(s_setup_side_card, s_setup_item == 1, true);
    ui_pixel_set_selected(s_setup_start_card, s_setup_item == 2, true);

    char diff_buf[48];
    snprintf(diff_buf, sizeof(diff_buf), "选择难度: %s", DIFF_NAMES[s_diff]);
    lv_label_set_text(s_setup_diff_label, diff_buf);

    char side_buf[48];
    snprintf(side_buf, sizeof(side_buf), "选择阵营: %s", SIDE_NAMES[s_side]);
    lv_label_set_text(s_setup_side_label, side_buf);
    lv_obj_set_style_text_color(s_setup_side_label, lv_color_hex(s_side == SIDE_BLUE ? 0x1689E8 : 0xE43B2F), 0);

    if (s_setup_item == 0) {
        lv_label_set_text(s_setup_hint_label, "按 OK 键切换难度等级");
    } else if (s_setup_item == 1) {
        lv_label_set_text(s_setup_hint_label, "按 OK 键切换红蓝旗 (蓝限3子/红限4子)");
    } else {
        lv_label_set_text(s_setup_hint_label, "三局两胜 · 每局两分钟 · 按 OK 开始！");
    }
}

static void build_play_ui(void);
static void build_setup_ui(void);

// 开启单局游戏
static void start_round(void) {
    s_stage = STAGE_PLAYING;
    for (int i = 0; i < BOARD_SIZE; i++) s_board[i] = CELL_EMPTY;
    s_cursor = 4;
    s_player_count = 0;
    s_ai_count = 0;
    s_is_ai_turn = false;
    s_step_countdown = 10;
    s_round_time_left = ROUND_MAX_SECONDS;
    s_round_winner = 0;

    build_play_ui();
    update_play_display();
}

// 电脑执行落子
static void do_ai_move(void) {
    int ai_move = ai_choose_move();
    if (ai_move >= 0) {
        place_ai_piece(ai_move);
        int w = check_winner(s_board);
        if (w == 2) {
            s_round_winner = 2;
            s_ai_round_wins++;
            if (s_ai_round_wins >= 2 || s_current_round >= 3) {
                s_stage = STAGE_MATCH_OVER;
            } else {
                s_stage = STAGE_ROUND_OVER;
            }
            update_play_display();
            return;
        }
    }
    // 电脑落子完成，轮到玩家思考（10秒）
    s_is_ai_turn = false;
    s_step_countdown = 10;
    update_play_display();
}

// 玩家超时自动落子
static void do_player_timeout_move(void) {
    int empty_cells[BOARD_SIZE];
    int count = 0;
    for (int i = 0; i < BOARD_SIZE; i++) {
        if (s_board[i] == CELL_EMPTY) empty_cells[count++] = i;
    }
    if (count > 0) {
        int pick = empty_cells[esp_random() % count];
        place_player_piece(pick);
        int w = check_winner(s_board);
        if (w == 1) {
            s_round_winner = 1;
            s_player_round_wins++;
            if (s_player_round_wins >= 2 || s_current_round >= 3) {
                s_stage = STAGE_MATCH_OVER;
            } else {
                s_stage = STAGE_ROUND_OVER;
            }
            update_play_display();
            return;
        }
        // 轮到电脑隔 5 秒思考
        s_is_ai_turn = true;
        s_step_countdown = 5;
        update_play_display();
    }
}

// 1秒定时器驱动每局2分钟倒计时与步时
static void on_second_timer(lv_timer_t *timer) {
    (void)timer;
    if (s_stage != STAGE_PLAYING) return;

    // 1. 每局总时间（120秒）倒计时
    if (s_round_time_left > 1) {
        s_round_time_left--;
    } else {
        // 2分钟完就为平局！
        s_round_time_left = 0;
        s_round_winner = 3;
        s_round_draws++;
        if (s_player_round_wins >= 2 || s_ai_round_wins >= 2 || s_current_round >= 3) {
            s_stage = STAGE_MATCH_OVER;
        } else {
            s_stage = STAGE_ROUND_OVER;
        }
        update_play_display();
        return;
    }

    // 2. 单步倒计时
    if (s_step_countdown > 1) {
        s_step_countdown--;
        update_play_display();
    } else {
        if (!s_is_ai_turn) {
            do_player_timeout_move();
        } else {
            do_ai_move();
        }
    }
}

static void build_setup_ui(void) {
    if (s_play_root) {
        lv_obj_add_flag(s_play_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_setup_root) {
        s_setup_root = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_setup_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_setup_root, 240, 320);
        lv_obj_set_pos(s_setup_root, 0, 0);
        lv_obj_set_style_bg_opa(s_setup_root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_setup_root, 0, 0);
        lv_obj_set_style_pad_all(s_setup_root, 0, 0);

        s_setup_box = ui_pixel_panel_create(s_setup_root, 16, 50, 208, 210, UI_PAPER);

        lv_obj_t *title = lv_label_create(s_setup_box);
        lv_label_set_text(title, "井字棋 · 初始配置");
        lv_obj_set_style_text_font(title, &font_chinese_14, 0);
        lv_obj_set_style_text_color(title, lv_color_hex(UI_INK), 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 4);

        // 选项1：难度
        s_setup_diff_card = ui_pixel_panel_create(s_setup_box, 10, 32, 174, 40, UI_PAPER);
        lv_obj_set_style_pad_all(s_setup_diff_card, 0, 0);
        s_setup_diff_label = lv_label_create(s_setup_diff_card);
        lv_obj_set_style_text_font(s_setup_diff_label, &font_chinese_14, 0);
        lv_obj_set_style_text_color(s_setup_diff_label, lv_color_hex(UI_INK), 0);
        lv_obj_center(s_setup_diff_label);

        // 选项2：阵营 (红/蓝)
        s_setup_side_card = ui_pixel_panel_create(s_setup_box, 10, 80, 174, 40, UI_PAPER);
        lv_obj_set_style_pad_all(s_setup_side_card, 0, 0);
        s_setup_side_label = lv_label_create(s_setup_side_card);
        lv_obj_set_style_text_font(s_setup_side_label, &font_chinese_14, 0);
        lv_obj_center(s_setup_side_label);

        // 选项3：开始对决
        s_setup_start_card = ui_pixel_panel_create(s_setup_box, 10, 128, 174, 40, UI_GRASS);
        lv_obj_set_style_pad_all(s_setup_start_card, 0, 0);
        s_setup_start_label = lv_label_create(s_setup_start_card);
        lv_label_set_text(s_setup_start_label, "【开始三局两胜】");
        lv_obj_set_style_text_font(s_setup_start_label, &font_chinese_14, 0);
        lv_obj_set_style_text_color(s_setup_start_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_center(s_setup_start_label);

        s_setup_hint_label = lv_label_create(s_setup_root);
        lv_obj_set_style_text_font(s_setup_hint_label, &font_chinese_14, 0);
        lv_obj_set_style_text_color(s_setup_hint_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_setup_hint_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    }
    lv_obj_clear_flag(s_setup_root, LV_OBJ_FLAG_HIDDEN);
    update_setup_display();
}

static void build_play_ui(void) {
    if (s_setup_root) {
        lv_obj_add_flag(s_setup_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (!s_play_root) {
        s_play_root = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_play_root, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_play_root, 240, 320);
        lv_obj_set_pos(s_play_root, 0, 0);
        lv_obj_set_style_bg_opa(s_play_root, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_play_root, 0, 0);
        lv_obj_set_style_pad_all(s_play_root, 0, 0);

        // 顶部比分
        lv_obj_t *score_bar = lv_obj_create(s_play_root);
        lv_obj_remove_flag(score_bar, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(score_bar, 226, 24);
        lv_obj_set_pos(score_bar, 7, 44);
        lv_obj_set_style_radius(score_bar, 4, 0);
        lv_obj_set_style_bg_color(score_bar, lv_color_hex(UI_PAPER), 0);
        lv_obj_set_style_border_color(score_bar, lv_color_hex(UI_INK), 0);
        lv_obj_set_style_border_width(score_bar, 2, 0);
        lv_obj_set_style_pad_all(score_bar, 0, 0);

        s_top_score_label = lv_label_create(score_bar);
        lv_obj_set_style_text_font(s_top_score_label, &font_chinese_14, 0);
        lv_obj_set_style_text_color(s_top_score_label, lv_color_hex(UI_INK), 0);
        lv_obj_center(s_top_score_label);

        // 中部九宫格
        int cell_w = 46;
        int cell_h = 40;
        int gap = 6;
        int board_w = cell_w * 3 + gap * 2;
        int start_x = (240 - board_w) / 2;
        int start_y = 72;

        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) {
                int idx = r * 3 + c;
                int cx = start_x + c * (cell_w + gap);
                int cy = start_y + r * (cell_h + gap);
                s_cells[idx] = ui_pixel_panel_create(s_play_root, cx, cy, cell_w, cell_h, UI_PAPER);
                lv_obj_set_style_pad_all(s_cells[idx], 0, 0);
            }
        }

        // 状态面板
        s_panel = ui_pixel_panel_create(s_play_root, 12, 210, 216, 68, UI_PAPER);

        s_status_label = lv_label_create(s_panel);
        lv_obj_set_style_text_font(s_status_label, &font_chinese_14, 0);
        lv_obj_align(s_status_label, LV_ALIGN_TOP_MID, 0, 2);

        s_timer_label = lv_label_create(s_panel);
        lv_obj_set_style_text_font(s_timer_label, &font_chinese_14, 0);
        lv_obj_align(s_timer_label, LV_ALIGN_TOP_MID, 0, 22);

        s_detail_label = lv_label_create(s_panel);
        lv_obj_set_style_text_font(s_detail_label, &font_chinese_14, 0);
        lv_obj_set_style_text_color(s_detail_label, lv_color_hex(0x666666), 0);
        lv_obj_align(s_detail_label, LV_ALIGN_TOP_MID, 0, 42);

        s_hint_label = lv_label_create(s_play_root);
        lv_obj_set_style_text_font(s_hint_label, &font_chinese_14, 0);
        lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xFFFFFF), 0);
        lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -18);

        s_mascot = ui_pixel_mascot_create(s_play_root, 101, 276);
    }
    lv_obj_clear_flag(s_play_root, LV_OBJ_FLAG_HIDDEN);
}

void demo_ttt_enter(void) {
    s_scr = ui_pixel_screen_create("井字棋");
    s_setup_root = NULL;
    s_play_root = NULL;
    s_stage = STAGE_SETUP;
    s_setup_item = 0;
    s_current_round = 1;
    s_player_round_wins = 0;
    s_ai_round_wins = 0;
    s_round_draws = 0;

    build_setup_ui();

    if (!s_sec_timer) {
        s_sec_timer = lv_timer_create(on_second_timer, 1000, NULL);
    }

    lv_screen_load(s_scr);
}

void demo_ttt_exit(void) {
    if (s_sec_timer) {
        lv_timer_delete(s_sec_timer);
        s_sec_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_setup_root = NULL;
        s_play_root = NULL;
        s_mascot = NULL;
    }
}

void demo_ttt_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (s_stage == STAGE_SETUP) {
        if (btn == BSP_BTN_UP) {
            s_setup_item = (s_setup_item + 3 - 1) % 3;
            update_setup_display();
        } else if (btn == BSP_BTN_DOWN) {
            s_setup_item = (s_setup_item + 1) % 3;
            update_setup_display();
        } else if (btn == BSP_BTN_OK) {
            if (s_setup_item == 0) {
                s_diff = (s_diff + 1) % TTT_DIFF_COUNT;
                update_setup_display();
            } else if (s_setup_item == 1) {
                s_side = (s_side + 1) % SIDE_COUNT;
                update_setup_display();
            } else if (s_setup_item == 2) {
                // 根据阵营配置棋子上限：蓝旗限3颗，红旗限4颗
                if (s_side == SIDE_BLUE) {
                    s_player_max = 3;
                    s_ai_max = 4;
                } else {
                    s_player_max = 4;
                    s_ai_max = 3;
                }
                s_current_round = 1;
                s_player_round_wins = 0;
                s_ai_round_wins = 0;
                s_round_draws = 0;
                start_round();
            }
        }
    } else if (s_stage == STAGE_PLAYING) {
        if (s_is_ai_turn) return; // 电脑思考5秒期间按键锁定

        if (btn == BSP_BTN_UP) {
            s_cursor = (s_cursor + BOARD_SIZE - 1) % BOARD_SIZE;
            update_play_display();
        } else if (btn == BSP_BTN_DOWN) {
            s_cursor = (s_cursor + 1) % BOARD_SIZE;
            update_play_display();
        } else if (btn == BSP_BTN_OK) {
            if (s_board[s_cursor] == CELL_EMPTY) {
                // 落子，如果超过上限则最早的子被消除
                place_player_piece(s_cursor);
                int w = check_winner(s_board);
                if (w == 1) {
                    s_round_winner = 1;
                    s_player_round_wins++;
                    if (s_player_round_wins >= 2 || s_current_round >= 3) {
                        s_stage = STAGE_MATCH_OVER;
                    } else {
                        s_stage = STAGE_ROUND_OVER;
                    }
                    update_play_display();
                    return;
                }

                // 轮到电脑，开启 5 秒思考倒计时
                s_is_ai_turn = true;
                s_step_countdown = 5;
                update_play_display();
            } else {
                ui_pixel_mascot_jump(s_mascot);
            }
        }
    } else if (s_stage == STAGE_ROUND_OVER) {
        // 单局结束，开启下一局 (进入第 2 或第 3 局)
        if (btn == BSP_BTN_OK || btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_current_round++;
            start_round();
            ui_pixel_mascot_jump(s_mascot);
        }
    } else if (s_stage == STAGE_MATCH_OVER) {
        // 三局两胜决胜完成，返回初始配置页面
        if (btn == BSP_BTN_OK || btn == BSP_BTN_UP || btn == BSP_BTN_DOWN) {
            s_stage = STAGE_SETUP;
            s_setup_item = 0;
            build_setup_ui();
            ui_pixel_mascot_jump(s_mascot);
        }
    }
}
