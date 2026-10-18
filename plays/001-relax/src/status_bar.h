#pragma once
#include "lvgl.h"

// 创建或更新全局状态栏（左上角电量，右上角时间）
// 返回包含状态栏的容器对象，或者直接附着到屏幕 scr
void status_bar_attach(lv_obj_t *scr);
