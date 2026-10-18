#pragma once
#include <stdbool.h>

void time_sync_init(void);
bool time_sync_is_synced(void);
void time_sync_get_time_str(char *buf, size_t max_len);
