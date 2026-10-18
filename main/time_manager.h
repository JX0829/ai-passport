#pragma once
#include <stdbool.h>
#include <time.h>

// 初始化时间同步服务（在后台连 Wi-Fi 同步 SNTP）
void time_manager_init(void);

// 获取当前时间字符串，如 "19:02"；若未同步，则返回开机时长如 "00:05" 或真实时间
void time_manager_get_time_str(char *buf, size_t max_len);

bool time_manager_is_synced(void);
