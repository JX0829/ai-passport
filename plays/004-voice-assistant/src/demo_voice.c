// main/demo_voice.c —— 004 语音助手 (Voice Assistant with LLM & Streaming TTS)
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
#define MINIMP3_IMPLEMENTATION
#include "minimp3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "demo_voice";

#define RECORD_SAMPLE_RATE 16000
#define RECORD_SECONDS     3
#define RECORD_BYTES       (RECORD_SAMPLE_RATE * 2 * RECORD_SECONDS) // 16kHz, 16bit, 1ch = 96000 字节

// API 配置 (可根据局域网环境调整)
#define LLM_API_URL        "http://192.168.31.115:8317/v1/chat/completions"
#define LLM_API_KEY        "***"
#define LLM_MODEL          "gemini-3.8-flash"

#define TTS_API_URL        "http://192.168.31.170:5050/v1/audio/speech"

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
static lv_obj_t *s_hint_label;
static lv_obj_t *s_mascot;

static TaskHandle_t s_worker_task = NULL;
static volatile voice_state_t s_state = VOICE_STATE_IDLE;
static volatile bool s_barge_in_requested = false; // 用户打断标志
static volatile bool s_start_recording_req = false;

static char s_last_user_text[256] = "暂无语音输入";
static char s_last_ai_reply[512]  = "你好！点击 OK 键对我说话。";

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
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0; // Subchunk1Size (16 for PCM)
    header[20] = 1;  header[21] = 0; // AudioFormat (1 for PCM)
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

// 刷新 UI 显示 (需在 LVGL 上下文或通过加锁调用)
static void update_voice_ui(void) {
    if (!bsp_lvgl_lock(500)) return;

    if (!s_scr) {
        bsp_lvgl_unlock();
        return;
    }

    lv_label_set_text(s_user_text_label, s_last_user_text);
    lv_label_set_text(s_ai_text_label, s_last_ai_reply);

    switch (s_state) {
        case VOICE_STATE_IDLE:
            lv_label_set_text(s_status_badge, "[待命] 点击 OK 说话");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(0x1B8A2B), 0);
            lv_label_set_text(s_hint_label, "按 OK 键开始倾听语音");
            break;
        case VOICE_STATE_LISTENING:
            lv_label_set_text(s_status_badge, "[倾听] 正在录音(3秒)...");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(UI_RED), 0);
            lv_label_set_text(s_hint_label, "请对准机身麦克风清晰说话");
            ui_pixel_mascot_jump(s_mascot);
            break;
        case VOICE_STATE_THINKING:
            lv_label_set_text(s_status_badge, "[思考] 正在识别与思考...");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(UI_ORANGE), 0);
            lv_label_set_text(s_hint_label, "大模型正在生成解答与语音");
            break;
        case VOICE_STATE_SPEAKING:
            lv_label_set_text(s_status_badge, "[播报] 语音播报中(按OK打断)");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(0x1689E8), 0);
            lv_label_set_text(s_hint_label, "随时按 OK 键立即打断并开启新对话");
            ui_pixel_mascot_jump(s_mascot);
            break;
        case VOICE_STATE_ERROR:
            lv_label_set_text(s_status_badge, "[异常] 连接失败或超时");
            lv_obj_set_style_text_color(s_status_badge, lv_color_hex(UI_RED), 0);
            lv_label_set_text(s_hint_label, "请检查网络或后端代理服务状态");
            break;
    }

    bsp_lvgl_unlock();
}

// 录音函数：录制 3 秒 16kHz 单声道音频
static uint8_t *record_audio(size_t *out_size) {
    ESP_LOGI(TAG, "开始录音 %d 秒...", RECORD_SECONDS);
    bsp_audio_set_format(RECORD_SAMPLE_RATE, 16, 1);

    uint8_t *pcm_buf = malloc(RECORD_BYTES);
    if (!pcm_buf) {
        ESP_LOGE(TAG, "录音内存分配失败 (%d bytes)", RECORD_BYTES);
        return NULL;
    }

    size_t total_read = 0;
    while (total_read < RECORD_BYTES) {
        if (s_barge_in_requested) {
            ESP_LOGW(TAG, "录音被中断");
            break;
        }
        size_t chunk = 1024;
        if (total_read + chunk > RECORD_BYTES) chunk = RECORD_BYTES - total_read;
        esp_err_t err = bsp_audio_read(pcm_buf + total_read, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bsp_audio_read 失败: %s", esp_err_to_name(err));
            break;
        }
        total_read += chunk;
    }

    *out_size = total_read;
    ESP_LOGI(TAG, "录音完成，共读取 %d 字节", total_read);
    return pcm_buf;
}

// 请求 LLM：将录音以 input_audio 格式发送给 OpenAI 兼容代理，返回 user_text 和 reply
static bool request_llm(const uint8_t *pcm, size_t pcm_len, char *user_text, size_t user_text_size, char *ai_reply, size_t ai_reply_size) {
    size_t wav_len = pcm_len + 44;
    uint8_t *wav_buf = malloc(wav_len);
    if (!wav_buf) return false;

    write_wav_header(wav_buf, pcm_len, RECORD_SAMPLE_RATE, 1, 16);
    memcpy(wav_buf + 44, pcm, pcm_len);

    size_t b64_len = 0;
    char *wav_b64 = base64_encode(wav_buf, wav_len, &b64_len);
    free(wav_buf);
    if (!wav_b64) return false;

    // 构造请求 JSON
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", LLM_MODEL);
    cJSON *messages = cJSON_AddArrayToObject(root, "messages");
    cJSON *user_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON *content_arr = cJSON_AddArrayToObject(user_msg, "content");

    cJSON *prompt_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(prompt_obj, "type", "text");
    cJSON_AddStringToObject(prompt_obj, "text", "你是一个语音助手。请严格按照JSON格式输出：{\"user_text\":\"转写的用户说话文字\",\"reply\":\"你的简明回答(30字以内)\"}");
    cJSON_AddItemToArray(content_arr, prompt_obj);

    cJSON *audio_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(audio_obj, "type", "input_audio");
    cJSON *audio_sub = cJSON_AddObjectToObject(audio_obj, "input_audio");
    cJSON_AddStringToObject(audio_sub, "data", wav_b64);
    cJSON_AddStringToObject(audio_sub, "format", "wav");
    cJSON_AddItemToArray(content_arr, audio_obj);

    cJSON_AddItemToArray(messages, user_msg);
    cJSON *resp_fmt = cJSON_AddObjectToObject(root, "response_format");
    cJSON_AddStringToObject(resp_fmt, "type", "json_object");

    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(wav_b64);

    if (!post_data) return false;

    esp_http_client_config_t config = {
        .url = LLM_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .buffer_size = 2048,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    char auth_hdr[128];
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s", LLM_API_KEY);
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_perform(client);
    free(post_data);

    bool success = false;
    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        if (status_code == 200) {
            int content_len = esp_http_client_get_content_length(client);
            if (content_len <= 0) content_len = 4096;
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
                                // 解析嵌套 JSON
                                char *raw_c = content->valuestring;
                                if (strstr(raw_c, "```json")) raw_c = strstr(raw_c, "```json") + 7;
                                cJSON *nested = cJSON_Parse(raw_c);
                                if (nested) {
                                    cJSON *u = cJSON_GetObjectItem(nested, "user_text");
                                    cJSON *r = cJSON_GetObjectItem(nested, "reply");
                                    if (u && u->valuestring) strncpy(user_text, u->valuestring, user_text_size - 1);
                                    if (r && r->valuestring) strncpy(ai_reply, r->valuestring, ai_reply_size - 1);
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
    } else {
        ESP_LOGE(TAG, "HTTP perform 失败: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    return success;
}

// 请求 TTS 服务并流式软解播放 MP3 (支持按 OK 键即时打断)
static void stream_tts_playback(const char *text) {
    if (!text || strlen(text) == 0) return;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "input", text);
    char *post_data = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!post_data) return;

    esp_http_client_config_t config = {
        .url = TTS_API_URL,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, post_data, strlen(post_data));

    esp_err_t err = esp_http_client_open(client, strlen(post_data));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TTS 连接打开失败: %s", esp_err_to_name(err));
        free(post_data);
        esp_http_client_cleanup(client);
        return;
    }

    int write_len = esp_http_client_write(client, post_data, strlen(post_data));
    (void)write_len;
    free(post_data);
    esp_http_client_fetch_headers(client);

    int status_code = esp_http_client_get_status_code(client);
    if (status_code != 200) {
        ESP_LOGE(TAG, "TTS 服务返回状态: %d", status_code);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // 初始化 minimp3 解码器
    mp3dec_t dec;
    mp3dec_init(&dec);
    mp3dec_frame_info_t info;
    short *pcm_frame = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(short));
    if (!pcm_frame) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return;
    }

    // 设置初始音频输出：24kHz，16bit，单声道 (实测该 TTS 格式为 24000Hz 1ch)
    bsp_audio_set_format(24000, 16, 1);
    bsp_audio_set_volume(85);

    #define STREAM_CHUNK 1024
    uint8_t *stream_buf = malloc(STREAM_CHUNK * 2);
    int buf_len = 0;

    while (!s_barge_in_requested) {
        int rlen = esp_http_client_read(client, (char *)stream_buf + buf_len, STREAM_CHUNK);
        if (rlen <= 0) break; // 读取结束
        buf_len += rlen;

        int offset = 0;
        while (offset < buf_len && !s_barge_in_requested) {
            int samples = mp3dec_decode_frame(&dec, stream_buf + offset, buf_len - offset, pcm_frame, &info);
            if (info.frame_bytes == 0) {
                // 剩余数据不足以构成一帧，移到缓冲区头部待下次继续填充
                break;
            }
            offset += info.frame_bytes;

            if (samples > 0 && !s_barge_in_requested) {
                bsp_audio_write(pcm_frame, samples * sizeof(short) * info.channels);
            }
        }

        // 整理未解码的残余数据
        if (offset < buf_len) {
            memmove(stream_buf, stream_buf + offset, buf_len - offset);
            buf_len = buf_len - offset;
        } else {
            buf_len = 0;
        }
    }

    free(pcm_frame);
    free(stream_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (s_barge_in_requested) {
        ESP_LOGW(TAG, "播报已被用户 OK 键打断！");
    }
}

// 语音助手工作任务 (非阻塞后台线程)
static void voice_worker_task(void *arg) {
    while (1) {
        if (s_start_recording_req) {
            s_start_recording_req = false;
            s_barge_in_requested = false;

            // 1. 切换状态：正在录音
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

            // 2. 切换状态：正在思考识别
            s_state = VOICE_STATE_THINKING;
            update_voice_ui();

            char user_txt[128] = {0};
            char ai_txt[256] = {0};

            bool ok = request_llm(pcm_data, pcm_size, user_txt, sizeof(user_txt), ai_txt, sizeof(ai_txt));
            free(pcm_data);

            if (ok) {
                strncpy(s_last_user_text, user_txt, sizeof(s_last_user_text) - 1);
                strncpy(s_last_ai_reply, ai_txt, sizeof(s_last_ai_reply) - 1);

                // 3. 切换状态：正在语音播报
                s_state = VOICE_STATE_SPEAKING;
                update_voice_ui();

                stream_tts_playback(s_last_ai_reply);

                // 4. 播报完毕，回到待命状态
                if (!s_barge_in_requested) {
                    s_state = VOICE_STATE_IDLE;
                    update_voice_ui();
                }
            } else {
                s_state = VOICE_STATE_ERROR;
                snprintf(s_last_ai_reply, sizeof(s_last_ai_reply), "网络连接异常，请重试");
                update_voice_ui();
                vTaskDelay(pdMS_TO_TICKS(1500));
                s_state = VOICE_STATE_IDLE;
                update_voice_ui();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void demo_voice_enter(void) {
    s_state = VOICE_STATE_IDLE;
    s_barge_in_requested = false;
    s_start_recording_req = false;

    s_scr = ui_pixel_screen_create("语音助手");

    // 顶部状态栏提示横幅
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

    // 用户说话文本框 (蓝底框)
    s_user_box = ui_pixel_panel_create(s_scr, 10, 72, 220, 68, UI_PAPER);
    lv_obj_t *u_title = lv_label_create(s_user_box);
    lv_label_set_text(u_title, "[你]：");
    lv_obj_set_style_text_font(u_title, &font_chinese_14, 0);
    lv_obj_set_style_text_color(u_title, lv_color_hex(0x1689E8), 0);
    lv_obj_align(u_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_user_text_label = lv_label_create(s_user_box);
    lv_obj_set_style_text_font(s_user_text_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_user_text_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(s_user_text_label, 204);
    lv_label_set_long_mode(s_user_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_user_text_label, LV_ALIGN_TOP_LEFT, 0, 20);

    // AI 回答文本框 (白底框)
    s_ai_box = ui_pixel_panel_create(s_scr, 10, 146, 220, 96, UI_PAPER);
    lv_obj_t *ai_title = lv_label_create(s_ai_box);
    lv_label_set_text(ai_title, "[助手]：");
    lv_obj_set_style_text_font(ai_title, &font_chinese_14, 0);
    lv_obj_set_style_text_color(ai_title, lv_color_hex(0x1B8A2B), 0);
    lv_obj_align(ai_title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_ai_text_label = lv_label_create(s_ai_box);
    lv_obj_set_style_text_font(s_ai_text_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_ai_text_label, lv_color_hex(UI_INK), 0);
    lv_obj_set_width(s_ai_text_label, 204);
    lv_label_set_long_mode(s_ai_text_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_ai_text_label, LV_ALIGN_TOP_LEFT, 0, 20);

    // 底部提示
    s_hint_label = lv_label_create(s_scr);
    lv_obj_set_style_text_font(s_hint_label, &font_chinese_14, 0);
    lv_obj_set_style_text_color(s_hint_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_hint_label, LV_ALIGN_BOTTOM_MID, 0, -18);

    s_mascot = ui_pixel_mascot_create(s_scr, 101, 252);

    update_voice_ui();

    if (!s_worker_task) {
        xTaskCreate(voice_worker_task, "voice_task", 8192, NULL, 5, &s_worker_task);
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
            // 实时打断（Barge-in）：立刻掐断播音，瞬间启动新一轮录音！
            ESP_LOGI(TAG, "用户按 OK 键打断当前播音，开启新一轮倾听");
            s_barge_in_requested = true;
            s_start_recording_req = true;
        } else if (s_state == VOICE_STATE_IDLE || s_state == VOICE_STATE_ERROR) {
            // 开始录音
            s_barge_in_requested = false;
            s_start_recording_req = true;
        }
    }
}
