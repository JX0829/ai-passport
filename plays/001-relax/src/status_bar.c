#include "status_bar.h"
#include "bsp_battery.h"
#include "ui_pixel.h"
#include "esp_log.h"
#include <time.h>
#include <stdio.h>

typedef struct {
    lv_obj_t *battery_label;
    lv_obj_t *time_label;
    lv_timer_t *timer;
} status_bar_ctx_t;

static void status_bar_update(status_bar_ctx_t *ctx) {
    if (!ctx) return;

    // 1. 电量（显示在右上角时间下方，紧凑字体格式）
    int soc = bsp_battery_soc();
    if (ctx->battery_label) {
        if (soc >= 0) {
            lv_label_set_text_fmt(ctx->battery_label, "BAT %d%%", soc);
            if (soc < 20) {
                lv_obj_set_style_text_color(ctx->battery_label, lv_color_hex(UI_RED), 0);
            } else {
                lv_obj_set_style_text_color(ctx->battery_label, lv_color_hex(0x555555), 0);
            }
        } else {
            lv_label_set_text(ctx->battery_label, "BAT --%");
            lv_obj_set_style_text_color(ctx->battery_label, lv_color_hex(0x555555), 0);
        }
    }

    // 2. 时间
    if (ctx->time_label) {
        time_t now;
        time(&now);
        struct tm timeinfo;
        localtime_r(&now, &timeinfo);
        if (timeinfo.tm_year < (2020 - 1900)) {
            lv_label_set_text_fmt(ctx->time_label, "%02d:%02d", timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            lv_label_set_text_fmt(ctx->time_label, "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
        }
    }
}

static void on_timer(lv_timer_t *t) {
    status_bar_ctx_t *ctx = (status_bar_ctx_t *)lv_timer_get_user_data(t);
    status_bar_update(ctx);
}

static void on_scr_delete(lv_event_t *e) {
    status_bar_ctx_t *ctx = (status_bar_ctx_t *)lv_event_get_user_data(e);
    if (ctx) {
        if (ctx->timer) {
            lv_timer_delete(ctx->timer);
            ctx->timer = NULL;
        }
        free(ctx);
    }
}

void status_bar_attach(lv_obj_t *scr) {
    if (!scr) return;

    status_bar_ctx_t *ctx = calloc(1, sizeof(status_bar_ctx_t));
    if (!ctx) return;

    // 右上角容器（统一包裹时间与电量）
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_set_size(box, 66, 36);
    lv_obj_set_pos(box, 168, 6);
    lv_obj_set_style_radius(box, 4, 0);
    lv_obj_set_style_bg_color(box, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    // 上行：时间（14号常规字体）
    ctx->time_label = lv_label_create(box);
    lv_obj_set_style_text_font(ctx->time_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(ctx->time_label, lv_color_hex(UI_INK), 0);
    lv_obj_align(ctx->time_label, LV_ALIGN_TOP_MID, 0, 2);

    // 下行：电量（10号小字体，大约是常规字体缩小一半，清晰不占位）
    ctx->battery_label = lv_label_create(box);
    lv_obj_set_style_text_font(ctx->battery_label, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(ctx->battery_label, lv_color_hex(0x555555), 0);
    lv_obj_align(ctx->battery_label, LV_ALIGN_BOTTOM_MID, 0, -2);

    status_bar_update(ctx);

    ctx->timer = lv_timer_create(on_timer, 1000, ctx);
    lv_obj_add_event_cb(scr, on_scr_delete, LV_EVENT_DELETE, ctx);
}
