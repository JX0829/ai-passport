// main/demo_relax.c —— 休闲时刻 (Relax Time)
// 核心玩法:
//   1. 初始页点击 OK: 随机在 [15, 20, 25, 30] 分钟中抽取一个时长并开启倒计时
//   2. 运行过程中: 按 UP +5 分钟，按 DOWN -5 分钟（只能调整一次机会）
//   3. 倒计时结束: 播放真人语音 "休息结束了，该干嘛干嘛去"，屏幕全屏高亮提示
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "lvgl.h"
#include "voice_prompt.h"
#include "font_chinese_14.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include <stdio.h>
#include <stdbool.h>

static const char *TAG = "demo_relax";

// 状态定义
typedef enum {
    STATE_IDLE = 0,       // 准备页: 提示点击 OK 抽取随机时间
    STATE_COUNTING,       // 倒计时进行中
    STATE_PAUSED,         // 暂停
    STATE_FINISHED        // 倒计时结束，播放提示语音中
} relax_state_t;

static const int TIME_OPTIONS_MINUTES[] = { 15, 20, 25, 30 };
#define TIME_OPTIONS_COUNT (sizeof(TIME_OPTIONS_MINUTES) / sizeof(TIME_OPTIONS_MINUTES[0]))

// UI 元素
static lv_obj_t *s_scr;
static lv_obj_t *s_panel;
static lv_obj_t *s_title_label;
static lv_obj_t *s_timer_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_sub_label;
static lv_obj_t *s_bar;
static lv_obj_t *s_mascot;

// 逻辑状态
static relax_state_t s_state = STATE_IDLE;
static int s_total_seconds = 0;
static int s_remain_seconds = 0;
static bool s_time_adjusted = false; // UP/DOWN 是否已经调整过一次
static lv_timer_t *s_second_timer = NULL;

// 异步音频播放任务与标志
static TaskHandle_t s_audio_task_handle = NULL;
static volatile bool s_audio_playing = false;
static volatile bool s_audio_stop_req = false;

// IMA-ADPCM 解码表
static const int8_t ADPCM_INDEX_TABLE[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
static const int16_t ADPCM_STEP_TABLE[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
};

static inline int16_t adpcm_decode_nibble(uint8_t code, int32_t *pred, int32_t *index) {
    int32_t step = ADPCM_STEP_TABLE[*index];
    int32_t diff = step >> 3;
    if (code & 4) diff += step;
    if (code & 2) diff += (step >> 1);
    if (code & 1) diff += (step >> 2);

    if (code & 8) *pred -= diff;
    else *pred += diff;

    if (*pred > 32767) *pred = 32767;
    else if (*pred < -32768) *pred = -32768;

    *index += ADPCM_INDEX_TABLE[code & 7];
    if (*index < 0) *index = 0;
    else if (*index > 88) *index = 88;

    return (int16_t)(*pred);
}

// 语音播放任务
static void voice_playback_task(void *arg) {
    (void)arg;
    s_audio_playing = true;
    bsp_audio_set_format(VOICE_PROMPT_SAMPLE_RATE, 16, 1);
    bsp_audio_set_volume(85);

    #define CHUNK_SIZE 256
    int16_t pcm_buf[CHUNK_SIZE];
    int chunk_idx = 0;

    int32_t pred = 0;
    int32_t step_idx = 0;

    ESP_LOGI(TAG, "开始播放提醒语音...");
    for (size_t i = 0; i < VOICE_PROMPT_ADPCM_LEN && !s_audio_stop_req; i++) {
        uint8_t byte = VOICE_PROMPT_ADPCM[i];
        uint8_t n1 = byte & 0x0f;
        uint8_t n2 = (byte >> 4) & 0x0f;

        pcm_buf[chunk_idx++] = adpcm_decode_nibble(n1, &pred, &step_idx);
        if (chunk_idx >= CHUNK_SIZE) {
            bsp_audio_write(pcm_buf, sizeof(pcm_buf));
            chunk_idx = 0;
        }

        pcm_buf[chunk_idx++] = adpcm_decode_nibble(n2, &pred, &step_idx);
        if (chunk_idx >= CHUNK_SIZE) {
            bsp_audio_write(pcm_buf, sizeof(pcm_buf));
            chunk_idx = 0;
        }
    }

    if (chunk_idx > 0 && !s_audio_stop_req) {
        bsp_audio_write(pcm_buf, chunk_idx * sizeof(int16_t));
    }

    ESP_LOGI(TAG, "语音播放完毕");
    s_audio_playing = false;
    s_audio_task_handle = NULL;
    vTaskDelete(NULL);
}

static void play_voice_prompt_async(void) {
    if (s_audio_playing) return;
    s_audio_stop_req = false;
    xTaskCreate(voice_playback_task, "voice_play", 3072, NULL, 5, &s_audio_task_handle);
}

static void stop_voice_prompt(void) {
    if (s_audio_playing) {
        s_audio_stop_req = true;
        int timeout = 50;
        while (s_audio_playing && --timeout > 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

// 刷新 UI 状态
static void update_ui(void) {
    if (!s_scr) return;

    if (s_state == STATE_IDLE) {
        lv_label_set_text(s_title_label, "休闲时刻");
        lv_label_set_text(s_timer_label, "--:--");
        lv_label_set_text(s_hint_label, "按 OK 开始");
        lv_label_set_text(s_sub_label, "随机: 15~30 分钟");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_PAPER), 0);
    } else if (s_state == STATE_COUNTING || s_state == STATE_PAUSED) {
        int m = s_remain_seconds / 60;
        int s = s_remain_seconds % 60;
        lv_label_set_text_fmt(s_timer_label, "%02d:%02d", m, s);
        lv_label_set_text(s_title_label, s_state == STATE_PAUSED ? "已暂停" : "休闲中...");

        if (!s_time_adjusted) {
            lv_label_set_text(s_hint_label, "UP:+5分  DOWN:-5分");
            lv_label_set_text(s_sub_label, "(仅限调整一次机会)");
        } else {
            lv_label_set_text(s_hint_label, "时间已调整");
            lv_label_set_text(s_sub_label, "OK: 暂停 / 继续");
        }

        int pct = 0;
        if (s_total_seconds > 0) {
            pct = 100 - (s_remain_seconds * 100 / s_total_seconds);
            if (pct < 0) pct = 0;
            if (pct > 100) pct = 100;
        }
        lv_bar_set_value(s_bar, pct, LV_ANIM_ON);
        lv_obj_set_style_bg_color(s_panel, lv_color_hex(UI_PAPER), 0);
    } else if (s_state == STATE_FINISHED) {
        lv_label_set_text(s_title_label, "时间到！");
        lv_label_set_text(s_timer_label, "00:00");
        lv_label_set_text(s_hint_label, "休息结束了！");
        lv_label_set_text(s_sub_label, "该干嘛干嘛去~");
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        // 醒目强调背景
        lv_obj_set_style_bg_color(s_panel, lv_color_hex(0xFFE4E4), 0);
    }
}

// 秒级计时器回调
static void on_second_timer(lv_timer_t *timer) {
    (void)timer;
    if (s_state != STATE_COUNTING) return;

    if (s_remain_seconds > 0) {
        s_remain_seconds--;
        update_ui();
        if (s_remain_seconds <= 0) {
            s_state = STATE_FINISHED;
            update_ui();
            ui_pixel_mascot_jump(s_mascot);
            play_voice_prompt_async();
        }
    }
}

void demo_relax_enter(void) {
    s_state = STATE_IDLE;
    s_total_seconds = 0;
    s_remain_seconds = 0;
    s_time_adjusted = false;
    s_audio_playing = false;
    s_audio_stop_req = false;

    s_scr = ui_pixel_screen_create("RELAX");
    s_panel = ui_pixel_panel_create(s_scr, 14, 52, 212, 180, UI_PAPER);

    s_title_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_title_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_title_label, lv_color_hex(0x666666), 0);
    lv_obj_set_style_text_align(s_title_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title_label, LV_ALIGN_TOP_MID, 0, 10);

    s_timer_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_timer_label, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_timer_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_timer_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_timer_label, LV_ALIGN_TOP_MID, 0, 36);

    // 进度条
    s_bar = lv_bar_create(s_panel);
    lv_obj_set_size(s_bar, 180, 10);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 84);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0xDDDDDD), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(UI_RED), LV_PART_INDICATOR);

    // 提示文字
    s_hint_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_hint_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_text_align(s_hint_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_hint_label, LV_ALIGN_TOP_MID, 0, 106);

    s_sub_label = lv_label_create(s_panel);
    lv_obj_set_style_text_font(s_sub_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_sub_label, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(s_sub_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sub_label, LV_ALIGN_TOP_MID, 0, 132);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 242);

    update_ui();
    s_second_timer = lv_timer_create(on_second_timer, 1000, NULL);
    lv_screen_load(s_scr);
}

void demo_relax_exit(void) {
    stop_voice_prompt();
    if (s_second_timer) {
        lv_timer_delete(s_second_timer);
        s_second_timer = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_panel = NULL;
        s_title_label = NULL;
        s_timer_label = NULL;
        s_hint_label = NULL;
        s_sub_label = NULL;
        s_bar = NULL;
        s_mascot = NULL;
    }
}

void demo_relax_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (s_state == STATE_IDLE) {
        if (btn == BSP_BTN_OK) {
            // 从 15, 20, 25, 30 分钟中随机抽取一个
            uint32_t r = esp_random() % TIME_OPTIONS_COUNT;
            int chosen_min = TIME_OPTIONS_MINUTES[r];
            s_total_seconds = chosen_min * 60;
            s_remain_seconds = s_total_seconds;
            s_time_adjusted = false;
            s_state = STATE_COUNTING;
            ESP_LOGI(TAG, "随机选择时长: %d 分钟", chosen_min);
            ui_pixel_mascot_jump(s_mascot);
            update_ui();
        }
    } else if (s_state == STATE_COUNTING || s_state == STATE_PAUSED) {
        if (btn == BSP_BTN_OK) {
            // 短按 OK 暂停/继续
            s_state = (s_state == STATE_COUNTING) ? STATE_PAUSED : STATE_COUNTING;
            update_ui();
        } else if (btn == BSP_BTN_UP && !s_time_adjusted) {
            // 加 5 分钟 (仅限 1 次)
            s_time_adjusted = true;
            s_total_seconds += 5 * 60;
            s_remain_seconds += 5 * 60;
            ui_pixel_mascot_jump(s_mascot);
            ESP_LOGI(TAG, "执行加 5 分钟");
            update_ui();
        } else if (btn == BSP_BTN_DOWN && !s_time_adjusted) {
            // 减 5 分钟 (仅限 1 次)
            s_time_adjusted = true;
            if (s_remain_seconds > 5 * 60) {
                s_remain_seconds -= 5 * 60;
            } else {
                s_remain_seconds = 10; // 保留 10 秒倒计时防越界
            }
            if (s_total_seconds > 5 * 60) {
                s_total_seconds -= 5 * 60;
            }
            ui_pixel_mascot_jump(s_mascot);
            ESP_LOGI(TAG, "执行减 5 分钟");
            update_ui();
        }
    } else if (s_state == STATE_FINISHED) {
        // 结束界面按任意键: 停止声音并重置回初始状态
        stop_voice_prompt();
        s_state = STATE_IDLE;
        update_ui();
    }
}
