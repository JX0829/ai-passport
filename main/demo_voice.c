// main/demo_voice.c —— 004 智能中英双语自适应问答系统
// 架构升级：P0 (libhelix-mp3 定点数解码 + 边下边播流式播放) + 中英文自适应切换 (问中文答中文，问英文答英文)
#include "demo.h"
#include "bsp_audio.h"
#include "bsp_display.h"
#include "ui_pixel.h"
#include "font_chinese_14.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "mp3dec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_voice";

#define RECORD_SAMPLE_RATE 8000
#define RECORD_SECONDS     3
#define RECORD_BYTES       (RECORD_SAMPLE_RATE * 2 * RECORD_SECONDS) // 8kHz, 16bit, 1ch = 48000 字节

// API 配置 (直连本地 8317 代理 API，无须额外桥接服务)
#define LLM_API_URL        "http://192.168.31.115:8317/v1/chat/completions"
#define LLM_API_KEY        "***"
#define LLM_MODEL          "gemini-3.8-flash"

// Edge-TTS (OpenAI 兼容端点)
#define TTS_API_URL        "http://192.168.31.170:5050/v1/audio/speech"
#define TTS_MODEL          "tts-1"
#define TTS_VOICE_ZH       "zh-CN-XiaoxiaoNeural"
#define TTS_VOICE_EN       "en-US-JennyNeural"

typedef enum {
    VOICE_STATE_IDLE = 0,     // 待命：提示按 OK 开启倾听
    VOICE_STATE_LISTENING,    // 正在录音
    VOICE_STATE_THINKING,     // 正在识别思考 (LLM)
    VOICE_STATE_SPEAKING,     // 正在播放回答 (TTS)
    VOICE_STATE_ERROR         // 异常状态
} voice_state_t;

static lv_obj_t *s_scr;
static lv_obj_t *s_top_bar;
static lv_obj_t *s_status_badge;
static lv_obj_t *s_user_box;
static lv_obj_t *s_user_text_label;
static lv_obj_t *s_ai_box;
static lv_obj_t *s_ai_text_label;
static lv_obj_t *s_tip_box;
static lv_obj_t *s_tip_text_label;
static lv_obj_t *s_hint_label;
static lv_obj_t *s_mascot;
static lv_obj_t *s_visualizer_bar[5];

static TaskHandle_t s_worker_task = NULL;
static volatile voice_state_t s_state = VOICE_STATE_IDLE;
static volatile bool s_barge_in_requested = false; // 用户打断标志
static volatile bool s_start_recording_req = false;
static volatile int  s_audio_anim_tick = 0;

static char s_last_user_text[256] = "点击 OK 键，说中文或英文提问";
static char s_last_ai_reply[512]  = "你好！我是智能问答助手，支持中英文双语问答。";
static char s_last_tip_text[256]  = "模式: 中文提问中文答 / 英文提问英文答";
static char s_current_voice[64]   = TTS_VOICE_ZH;

// Base64 编码辅助函数
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static char *base64_encode(const unsigned char *src, size_t len, size_t *out_len) {
    size_t olen = 4 * ((len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i, j = 0;
    for (i = 0; i < len;) {
        uint32_t a = (i < len) ? src[i++] : 0;
        uint32_t b = (i < len) ? src[i++] : 0;
        uint32_t c = (i < len) ? src[i++] : 0;
        uint32_t triple = (a << 16) + (b << 8) + c;
        out[j++] = b64_table[(triple >> 18) & 0x3F];
        out[j++] = b64_table[(triple >> 12) & 0x3F];
        out[j++] = (i > len + 1) ? '=' : b64_table[(triple >> 6) & 0x3F];
        out[j++] = (i > len) ? '=' : b64_table[triple & 0x3F];
    }
    out[olen] = '\0';
    if (out_len) *out_len = olen;
    return out;
}

// 构造 44 字节 WAV 文件头
static void write_wav_header(uint8_t *header, uint32_t pcm_data_size, uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample) {
    uint32_t total_size = pcm_data_size + 36;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    memcpy(header, "RIFF", 4);
    header[4] = (uint8_t)(total_size & 0xFF);
    header[5] = (uint8_t)((total_size >> 8) & 0xFF);
    header[6] = (uint8_t)((total_size >> 16) & 0xFF);
    header[7] = (uint8_t)((total_size >> 24) & 0xFF);
    memcpy(header + 8, "WAVEfmt ", 8);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1;  header[21] = 0;
    header[22] = (uint8_t)(channels & 0xFF);
    header[23] = (uint8_t)((channels >> 8) & 0xFF);
    header[24] = (uint8_t)(sample_rate & 0xFF);
    header[25] = (uint8_t)((sample_rate >> 8) & 0xFF);
    header[26] = (uint8_t)((sample_rate >> 16) & 0xFF);
    header[27] = (uint8_t)((sample_rate >> 24) & 0xFF);
    header[28] = (uint8_t)(byte_rate & 0xFF);
    header[29] = (uint8_t)((byte_rate >> 8) & 0xFF);
    header[30] = (uint8_t)((byte_rate >> 16) & 0xFF);
    header[31] = (uint8_t)((byte_rate >> 24) & 0xFF);
    header[32] = (uint8_t)(block_align & 0xFF);
    header[33] = (uint8_t)((block_align >> 8) & 0xFF);
    header[34] = (uint8_t)(bits_per_sample & 0xFF);
    header[35] = (uint8_t)((bits_per_sample >> 8) & 0xFF);
    memcpy(header + 36, "data", 4);
    header[40] = (uint8_t)(pcm_data_size & 0xFF);
    header[41] = (uint8_t)((pcm_data_size >> 8) & 0xFF);
    header[42] = (uint8_t)((pcm_data_size >> 16) & 0xFF);
    header[43] = (uint8_t)((pcm_data_size >> 24) & 0xFF);
}

// 刷新 UI 显示 (加锁调用)
static void update_voice_ui(void) {
    if (!bsp_lvgl_lock(500)) return;

    if (!s_scr) {
        bsp_lvgl_unlock();
        return;
    }

    lv_label_set_text(s_user_text_label, s_last_user_text);
    lv_label_set_text(s_ai_text_label, s_last_ai_reply);
    lv_label_set_text(s_tip_text_label, s_last_tip_text);

    switch (s_state) {
        case VOICE_STATE_IDLE:
            lv_label_set_text(s_status_badge, "[待命] 点击 OK 开始提问");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(0x1B8A2B), 0);
            lv_label_set_text(s_hint_label, "按 OK 说话 / 长按返回");
            break;
        case VOICE_STATE_LISTENING:
            lv_label_set_text(s_status_badge, "[倾听] 正在录音(3秒)...");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(UI_RED), 0);
            lv_label_set_text(s_hint_label, "请对准麦克风说中文或英文");
            ui_pixel_mascot_jump(s_mascot);
            break;
        case VOICE_STATE_THINKING:
            lv_label_set_text(s_status_badge, "[思考] 正在识别与生成回答...");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(0xD97706), 0);
            lv_label_set_text(s_hint_label, "AI 思考并组织回答中...");
            break;
        case VOICE_STATE_SPEAKING:
            lv_label_set_text(s_status_badge, "[回答] 语音播报中 (按OK打断)");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(0x2563EB), 0);
            lv_label_set_text(s_hint_label, "随时按 OK 键即时打断插话");
            ui_pixel_mascot_jump(s_mascot);
            break;
        case VOICE_STATE_ERROR:
            lv_label_set_text(s_status_badge, "[提示] 网络连接稍慢");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(UI_RED), 0);
            lv_label_set_text(s_hint_label, "请重试按 OK 提问");
            break;
    }

    bsp_lvgl_unlock();
}

// 声波律动跳动更新
static void update_visualizer_wave(int tick) {
    if (!bsp_lvgl_lock(100)) return;
    if (s_scr && s_state == VOICE_STATE_SPEAKING) {
        int heights[5] = {
            6 + (abs((tick * 7) % 18)),
            10 + (abs((tick * 11) % 20)),
            14 + (abs((tick * 13) % 24)),
            10 + (abs((tick * 9) % 20)),
            6 + (abs((tick * 5) % 16))
        };
        for (int i = 0; i < 5; i++) {
            if (s_visualizer_bar[i]) {
                lv_obj_set_height(s_visualizer_bar[i], heights[i]);
            }
        }
    } else if (s_scr) {
        for (int i = 0; i < 5; i++) {
            if (s_visualizer_bar[i]) {
                lv_obj_set_height(s_visualizer_bar[i], 4);
            }
        }
    }
    bsp_lvgl_unlock();
}

// 录音函数：录制 3 秒 8kHz 16-bit 单声道 PCM
static uint8_t *record_audio(size_t *out_len) {
    ESP_LOGI(TAG, "配置音频为录音格式: %dHz 16-bit mono", RECORD_SAMPLE_RATE);
    esp_err_t err = bsp_audio_set_format(RECORD_SAMPLE_RATE, 16, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "设置音频格式失败");
        return NULL;
    }

    uint8_t *pcm_buf = malloc(RECORD_BYTES);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "分配录音缓冲区失败 (%d bytes)", RECORD_BYTES);
        return NULL;
    }

    ESP_LOGI(TAG, "开始采集录音...");
    size_t chunk_size = 2048;
    size_t total_read = 0;

    while (total_read < RECORD_BYTES) {
        size_t to_read = (RECORD_BYTES - total_read > chunk_size) ? chunk_size : (RECORD_BYTES - total_read);
        err = bsp_audio_read(pcm_buf + total_read, to_read);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "录音读取中断 at %d", total_read);
            break;
        }
        total_read += to_read;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "录音完成，共读取 %d 字节", total_read);
    *out_len = total_read;
    return pcm_buf;
}

// 字符清洗辅助函数：将中文/特殊单双引号与破折号转为标准 ASCII，防止自定义小字库缺字乱码
static void sanitize_display_text(char *str) {
    if (!str) return;
    char *src = str;
    char *dst = str;
    while (*src) {
        // UTF-8 ‘ (0xE2 0x80 0x98) 或 ’ (0xE2 0x80 0x99) -> '
        if ((unsigned char)src[0] == 0xE2 && (unsigned char)src[1] == 0x80 &&
            ((unsigned char)src[2] == 0x98 || (unsigned char)src[2] == 0x99)) {
            *dst++ = '\'';
            src += 3;
            continue;
        }
        // UTF-8 “ (0xE2 0x80 0x9C) 或 ” (0xE2 0x80 0x9D) -> "
        if ((unsigned char)src[0] == 0xE2 && (unsigned char)src[1] == 0x80 &&
            ((unsigned char)src[2] == 0x9C || (unsigned char)src[2] == 0x9D)) {
            *dst++ = '"';
            src += 3;
            continue;
        }
        // UTF-8 — (0xE2 0x80 0x94) -> -
        if ((unsigned char)src[0] == 0xE2 && (unsigned char)src[1] == 0x80 && (unsigned char)src[2] == 0x94) {
            *dst++ = '-';
            src += 3;
            continue;
        }
        // 过滤反引号 `
        if (*src == '`') {
            src++;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

// 请求 LLM：流式组装并发送音频，获取自适应中英文问答与音色选择
static bool request_llm(const uint8_t *pcm, size_t pcm_len,
                         char *user_text, size_t user_text_size,
                         char *ai_reply, size_t ai_reply_size,
                         char *lang_text, size_t lang_text_size,
                         char *voice_text, size_t voice_text_size) {
    size_t wav_len = pcm_len + 44;
    size_t b64_len = 4 * ((wav_len + 2) / 3);

    static const char prefix[] =
        "{\"model\":\"" LLM_MODEL "\",\"messages\":[{\"role\":\"user\",\"content\":["
        "{\"type\":\"text\",\"text\":\"You are an intelligent bilingual QA assistant. "
        "1. Transcribe the user audio accurately into user_text (Chinese or English). "
        "2. Language-adaptive rule: If user asked in Chinese, reply concisely in natural Chinese (within 35 Chinese characters). "
        "If user asked in English, reply concisely in natural English (within 25 English words). "
        "3. Output voice field: 'zh-CN-XiaoxiaoNeural' for Chinese reply, or 'en-US-JennyNeural' for English reply. "
        "Strictly output JSON format: {\\\"user_text\\\":\\\"...\\\",\\\"reply\\\":\\\"...\\\",\\\"lang\\\":\\\"zh|en\\\",\\\"voice\\\":\\\"...\\\"}\"},"
        "{\"type\":\"input_audio\",\"input_audio\":{\"format\":\"wav\",\"data\":\"";
    static const char suffix[] = "\"}}]}],\"response_format\":{\"type\":\"json_object\"}}";

    size_t total_post_len = (sizeof(prefix) - 1) + b64_len + (sizeof(suffix) - 1);

    esp_http_client_config_t config = {
        .url = LLM_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_hdr[128];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", LLM_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_hdr);

    esp_err_t err = esp_http_client_open(client, total_post_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LLM open 失败: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    // 1. 发送 JSON 前缀
    esp_http_client_write(client, prefix, sizeof(prefix) - 1);

    // 2. 流式逐段编码 Base64 并发送（每次 768 字节，避免大内存占用）
    uint8_t wav_header[44];
    write_wav_header(wav_header, pcm_len, RECORD_SAMPLE_RATE, 1, 16);

    #define CHUNK_RAW_SIZE 768
    uint8_t raw_chunk[CHUNK_RAW_SIZE];
    size_t wav_pos = 0;

    while (wav_pos < wav_len) {
        size_t n = 0;
        while (n < CHUNK_RAW_SIZE && wav_pos < wav_len) {
            if (wav_pos < 44) {
                raw_chunk[n++] = wav_header[wav_pos++];
            } else {
                raw_chunk[n++] = pcm[wav_pos - 44];
                wav_pos++;
            }
        }
        size_t out_b64 = 0;
        char *chunk_b64 = base64_encode(raw_chunk, n, &out_b64);
        if (chunk_b64) {
            esp_http_client_write(client, chunk_b64, out_b64);
            free(chunk_b64);
        }
    }

    // 3. 发送 JSON 后缀
    esp_http_client_write(client, suffix, sizeof(suffix) - 1);

    // 4. 等待并读取响应
    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    bool success = false;

    if (status_code == 200) {
        int content_len = esp_http_client_get_content_length(client);
        if (content_len <= 0) content_len = 2048;
        char *resp_buf = malloc(content_len + 1);
        if (resp_buf) {
            int read_len = esp_http_client_read(client, resp_buf, content_len);
            if (read_len > 0) {
                resp_buf[read_len] = '\0';
                cJSON *res_json = cJSON_Parse(resp_buf);
                if (res_json) {
                    cJSON *choices = cJSON_GetObjectItem(res_json, "choices");
                    if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) > 0) {
                        cJSON *c0 = cJSON_GetArrayItem(choices, 0);
                        cJSON *msg = cJSON_GetObjectItem(c0, "message");
                        cJSON *content = cJSON_GetObjectItem(msg, "content");
                        if (content && content->valuestring) {
                            char *raw_c = content->valuestring;
                            if (strstr(raw_c, "```json")) raw_c = strstr(raw_c, "```json") + 7;
                            char *end_fence = strstr(raw_c, "```");
                            if (end_fence) *end_fence = '\0';
                            cJSON *nested = cJSON_Parse(raw_c);
                            if (nested) {
                                cJSON *u = cJSON_GetObjectItem(nested, "user_text");
                                cJSON *r = cJSON_GetObjectItem(nested, "reply");
                                cJSON *l = cJSON_GetObjectItem(nested, "lang");
                                cJSON *v = cJSON_GetObjectItem(nested, "voice");
                                if (u && u->valuestring) {
                                    strncpy(user_text, u->valuestring, user_text_size - 1);
                                    sanitize_display_text(user_text);
                                }
                                if (r && r->valuestring) {
                                    strncpy(ai_reply, r->valuestring, ai_reply_size - 1);
                                    sanitize_display_text(ai_reply);
                                }
                                if (l && l->valuestring) {
                                    strncpy(lang_text, l->valuestring, lang_text_size - 1);
                                }
                                if (v && v->valuestring) {
                                    strncpy(voice_text, v->valuestring, voice_text_size - 1);
                                }
                                cJSON_Delete(nested);
                                success = true;
                            }
                        }
                    }
                    cJSON_Delete(res_json);
                }
            }
            free(resp_buf);
        }
    } else {
        ESP_LOGE(TAG, "LLM 返回错误码: %d", status_code);
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

// P0: 升级为 libhelix-mp3 边下边播流式定点数解码器
// 动态根据语言选择音色 (中文使用 zh-CN-XiaoxiaoNeural, 英文使用 en-US-JennyNeural)
#define STREAM_RING_SIZE (MAINBUF_SIZE * 3)

static void stream_tts_playback(const char *text, const char *voice) {
    if (!text || strlen(text) == 0) return;

    const char *sel_voice = voice;
    if (!sel_voice || strlen(sel_voice) == 0) {
        sel_voice = TTS_VOICE_ZH;
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", TTS_MODEL);
    cJSON_AddStringToObject(root, "input", text);
    cJSON_AddStringToObject(root, "voice", sel_voice);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return;

    ESP_LOGI(TAG, "请求 Edge-TTS, 音色: %s, 文本: %s", sel_voice, text);

    esp_http_client_config_t config = {
        .url = TTS_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");

    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS 连接打开失败: %s", esp_err_to_name(err));
        free(post_data);
        esp_http_client_cleanup(client);
        return;
    }

    esp_http_client_write(client, post_data, strlen(post_data));
    free(post_data);

    esp_http_client_fetch_headers(client);
    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "TTS 服务返回状态: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // 初始化 Helix MP3 定点数解码器 (全局单次分配，播放全过程零 malloc)
    HMP3Decoder dec = MP3InitDecoder();
    if (!dec) {
        ESP_LOGE(TAG, "Helix MP3InitDecoder 失败");
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    short *outbuf = malloc(MAX_NCHAN * MAX_NSAMP * sizeof(short));
    unsigned char *ring = malloc(STREAM_RING_SIZE);
    if (!outbuf || !ring) {
        ESP_LOGE(TAG, "流式解码临时内存不足");
        if (outbuf) free(outbuf);
        if (ring) free(ring);
        MP3FreeDecoder(dec);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    bsp_audio_set_format(24000, 16, 1);
    bsp_audio_set_volume(85);

    int ring_bytes = 0;
    bool format_set = false;
    int anim_cnt = 0;

    ESP_LOGI(TAG, "开始 Helix MP3 边下边播...");

    while (!s_barge_in_requested) {
        int to_read = STREAM_RING_SIZE - ring_bytes;
        int r = 0;
        if (to_read > 0) {
            r = esp_http_client_read(client, (char *)ring + ring_bytes, to_read);
            if (r > 0) {
                ring_bytes += r;
            }
        }

        if (ring_bytes == 0) break; // 播放完成并读空

        unsigned char *read_ptr = ring;
        int bytes_left = ring_bytes;

        while (bytes_left > 0 && !s_barge_in_requested) {
            int offset = MP3FindSyncWord(read_ptr, bytes_left);
            if (offset < 0) {
                if (bytes_left > 3) {
                    memmove(ring, read_ptr + bytes_left - 3, 3);
                    ring_bytes = 3;
                } else {
                    memmove(ring, read_ptr, bytes_left);
                    ring_bytes = bytes_left;
                }
                break;
            }
            read_ptr += offset;
            bytes_left -= offset;

            unsigned char *frame_start = read_ptr;
            int bytes_before = bytes_left;
            int decode_err = MP3Decode(dec, &read_ptr, &bytes_left, outbuf, 0);

            if (decode_err == ERR_MP3_NONE) {
                MP3FrameInfo info;
                MP3GetLastFrameInfo(dec, &info);
                if (!format_set) {
                    bsp_audio_set_format(info.samprate, 16, info.nChans);
                    format_set = true;
                }
                bsp_audio_write(outbuf, info.outputSamps * sizeof(short));

                anim_cnt++;
                if (anim_cnt % 3 == 0) {
                    s_audio_anim_tick++;
                    update_visualizer_wave(s_audio_anim_tick);
                }
            } else if (decode_err == ERR_MP3_INDATA_UNDERFLOW || decode_err == ERR_MP3_MAINDATA_UNDERFLOW) {
                memmove(ring, frame_start, bytes_before);
                ring_bytes = bytes_before;
                break;
            } else {
                read_ptr = frame_start + 1;
                bytes_left = bytes_before - 1;
            }
        }

        if (bytes_left == 0) {
            ring_bytes = 0;
        }

        if (r <= 0 && ring_bytes == 0) {
            break; // HTTP 数据流读完且缓冲全部解码
        }
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    MP3FreeDecoder(dec);
    free(outbuf);
    free(ring);

    update_visualizer_wave(0);

    if (s_barge_in_requested) {
        ESP_LOGW(TAG, "播报已被 OK 键即时打断！");
    } else {
        ESP_LOGI(TAG, "Helix MP3 播报圆满结束");
    }
}

// 智能问答后台任务
static void voice_worker_task(void *arg) {
    while (1) {
        if (s_start_recording_req) {
            s_start_recording_req = false;
            s_barge_in_requested = false;

            s_state = VOICE_STATE_LISTENING;
            update_voice_ui();

            size_t pcm_size = 0;
            uint8_t *pcm_data = record_audio(&pcm_size);

            if (!pcm_data || pcm_size == 0) {
                s_state = VOICE_STATE_ERROR;
                snprintf(s_last_ai_reply, sizeof(s_last_ai_reply), "麦克风采集失败");
                update_voice_ui();
                if (pcm_data) free(pcm_data);
                continue;
            }

            s_state = VOICE_STATE_THINKING;
            update_voice_ui();

            char user_txt[128] = {0};
            char ai_txt[256] = {0};
            char lang_txt[16] = {0};
            char voice_txt[64] = {0};

            bool ok = request_llm(pcm_data, pcm_size,
                                  user_txt, sizeof(user_txt),
                                  ai_txt, sizeof(ai_txt),
                                  lang_txt, sizeof(lang_txt),
                                  voice_txt, sizeof(voice_txt));
            free(pcm_data);

            if (ok) {
                strncpy(s_last_user_text, user_txt, sizeof(s_last_user_text) - 1);
                strncpy(s_last_ai_reply, ai_txt, sizeof(s_last_ai_reply) - 1);

                if (strcmp(lang_txt, "en") == 0) {
                    snprintf(s_last_tip_text, sizeof(s_last_tip_text), "模式: [English] 英文问答");
                    strncpy(s_current_voice, TTS_VOICE_EN, sizeof(s_current_voice) - 1);
                } else {
                    snprintf(s_last_tip_text, sizeof(s_last_tip_text), "模式: [中文] 中文问答");
                    strncpy(s_current_voice, TTS_VOICE_ZH, sizeof(s_current_voice) - 1);
                }

                s_state = VOICE_STATE_SPEAKING;
                update_voice_ui();

                stream_tts_playback(s_last_ai_reply, s_current_voice);

                if (!s_barge_in_requested) {
                    s_state = VOICE_STATE_IDLE;
                    update_voice_ui();
                }
            } else {
                s_state = VOICE_STATE_ERROR;
                snprintf(s_last_ai_reply, sizeof(s_last_ai_reply), "网络连接稍慢，请重试");
                update_voice_ui();
                vTaskDelay(pdMS_TO_TICKS(1500));
                s_state = VOICE_STATE_IDLE;
                update_voice_ui();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}

void demo_voice_enter(void) {
    s_state = VOICE_STATE_IDLE;
    s_barge_in_requested = false;
    s_start_recording_req = false;
    s_audio_anim_tick = 0;

    s_scr = ui_pixel_screen_create("智能问答");

    // 顶部状态栏
    s_top_bar = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_top_bar, 226, 24);
    lv_obj_set_pos(s_top_bar, 7, 44);
    lv_obj_set_style_radius(s_top_bar, 4, 0);
    lv_obj_set_style_bg_color(s_top_bar, lv_color_hex(UI_PAPER), 0);
    lv_obj_set_style_border_color(s_top_bar, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(s_top_bar, 2, 0);
    lv_obj_set_style_pad_all(s_top_bar, 0, 0);

    s_status_badge = lv_label_create(s_top_bar);
    lv_obj_set_style_text_font(s_status_badge, &font_chinese_14, 0);
    lv_obj_center(s_status_badge);

    // 用户说话框 (上)
    s_user_box = ui_pixel_panel_create(s_scr, 8, 72, 224, 52, UI_PAPER);
    lv_obj_t *u_title = lv_label_create(s_user_box);
    lv_label_set_text(u_title, "[问]：");
    lv_obj_set_style_text_font(u_title, &font_chinese_14, 0);
    lv_obj_set_style_text_color(u_title, lv_color_hex(0x1689E8), 0);
    lv_obj_align(u_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_user_text_label = lv_label_create(s_user_box);
    lv_obj_set_style_text_font(s_user_text_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_user_text_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(s_user_text_label, 208);
    lv_label_set_long_mode(s_user_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_user_text_label, LV_ALIGN_TOP_LEFT, 0, 16);

    // AI 回答框 (中)
    s_ai_box = ui_pixel_panel_create(s_scr, 8, 128, 224, 76, UI_PAPER);
    lv_obj_t *ai_title = lv_label_create(s_ai_box);
    lv_label_set_text(ai_title, "[答]：");
    lv_obj_set_style_text_font(ai_title, &font_chinese_14, 0);
    lv_obj_set_style_text_color(ai_title, lv_color_hex(0x1B8A2B), 0);
    lv_obj_align(ai_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ai_text_label = lv_label_create(s_ai_box);
    lv_obj_set_style_text_font(s_ai_text_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_ai_text_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(s_ai_text_label, 208);
    lv_label_set_long_mode(s_ai_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_ai_text_label, LV_ALIGN_TOP_LEFT, 0, 16);

    // 模式/语言提示条 (黄底小横幅)
    s_tip_box = lv_obj_create(s_scr);
    lv_obj_remove_flag(s_tip_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(s_tip_box, 224, 22);
    lv_obj_set_pos(s_tip_box, 8, 208);
    lv_obj_set_style_radius(s_tip_box, 3, 0);
    lv_obj_set_style_bg_color(s_tip_box, lv_color_hex(0xFEF3C7), 0);
    lv_obj_set_style_border_color(s_tip_box, lv_color_hex(0xD97706), 0);
    lv_obj_set_style_border_width(s_tip_box, 1, 0);
    lv_obj_set_style_pad_all(s_tip_box, 0, 0);

    s_tip_text_label = lv_label_create(s_tip_box);
    lv_obj_set_style_text_font(s_tip_text_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_tip_text_label, lv_color_hex(0x92400E), 0);
    lv_obj_align(s_tip_text_label, LV_ALIGN_LEFT_MID, 4, 0);

    // 声波律动柱状条 (5 根像素柱，在右下角跃动)
    for (int i = 0; i < 5; i++) {
        s_visualizer_bar[i] = lv_obj_create(s_scr);
        lv_obj_remove_flag(s_visualizer_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(s_visualizer_bar[i], 4, 4);
        lv_obj_set_pos(s_visualizer_bar[i], 192 + i * 6, 232);
        lv_obj_set_style_radius(s_visualizer_bar[i], 1, 0);
        lv_obj_set_style_bg_color(s_visualizer_bar[i], lv_color_hex(0x2563EB), 0);
        lv_obj_set_style_border_width(s_visualizer_bar[i], 0, 0);
    }

    // 底部提示
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_LEFT, 10, -2);

    s_mascot = ui_pixel_mascot_create(s_scr, 150, 240);

    update_voice_ui();

    if (!s_worker_task) {
        xTaskCreate(voice_worker_task, "voice_task", 10240, NULL, 5, &s_worker_task);
    }

    lv_screen_load(s_scr);
}

void demo_voice_exit(void) {
    s_barge_in_requested = true;
    s_start_recording_req = false;
    if (s_worker_task) {
        vTaskDelete(s_worker_task);
        s_worker_task = NULL;
    }
    if (s_scr) {
        lv_obj_delete(s_scr);
        s_scr = NULL;
        s_mascot = NULL;
    }
}

void demo_voice_key(bsp_btn_t btn, bsp_btn_ev_t ev) {
    if (ev != BSP_BTN_CLICK) return;

    if (btn == BSP_BTN_OK) {
        if (s_state == VOICE_STATE_SPEAKING) {
            ESP_LOGI(TAG, "用户按 OK 键即时打断当前播音，开启新一轮倾听");
            s_barge_in_requested = true;
            s_start_recording_req = true;
        } else if (s_state == VOICE_STATE_IDLE || s_state == VOICE_STATE_ERROR) {
            s_barge_in_requested = false;
            s_start_recording_req = true;
        }
    }
}
