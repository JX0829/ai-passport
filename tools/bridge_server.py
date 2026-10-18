#!/usr/bin/env python3
"""
OpenClaw 智能硬件语音问答网关桥梁 (Gateway Bridge)
监听 8318 端口：
1. 接收 ESP32-C3 流式上传的音频 (WAV Base64 或 PCM)；
2. 调用 8317 多模态 Gemini 完成精准语音转写 (ASR)；
3. 检查意图并调用小助理技能（如实时天气 wttr.in、实时时间、通用大模型等）；
4. 格式化为中英文自适应问答 JSON 返回给 ESP32-C3，驱动 Edge-TTS 流式发音。
"""

import http.server
import socketserver
import json
import urllib.request
import urllib.error
import re
import datetime

PORT = 8318
LLM_URL = "http://192.168.31.115:8317/v1/chat/completions"
LLM_KEY = "***"

WEATHER_MAP = {
    "Sunny": "晴", "Clear": "晴朗", "Partly cloudy": "多云",
    "Cloudy": "阴", "Overcast": "阴天", "Mist": "有薄雾",
    "Patchy rain possible": "局部有雨", "Light rain": "小雨",
    "Moderate rain": "中雨", "Heavy rain": "大雨",
    "Smoke": "晴间薄雾", "Haze": "霾", "Fog": "大雾",
    "Thundery outbreaks possible": "雷阵雨"
}

CITY_MAP = {
    "北京": "Beijing", "上海": "Shanghai", "广州": "Guangzhou", "深圳": "Shenzhen",
    "杭州": "Hangzhou", "南京": "Nanjing", "成都": "Chengdu", "武汉": "Wuhan",
    "西安": "Xi'an", "重庆": "Chongqing", "天津": "Tianjin", "苏州": "Suzhou"
}

def get_realtime_weather(query_text):
    matched_city_cn = "北京"
    matched_city_en = "Beijing"
    for cn, en in CITY_MAP.items():
        if cn in query_text:
            matched_city_cn = cn
            matched_city_en = en
            break
            
    try:
        url = f"https://wttr.in/{matched_city_en}?format=j1"
        req = urllib.request.Request(url, headers={"User-Agent": "curl/8.0"})
        with urllib.request.urlopen(req, timeout=3) as resp:
            data = json.loads(resp.read().decode())
            cur = data["current_condition"][0]
            temp = cur["temp_C"]
            raw_desc = cur["weatherDesc"][0]["value"]
            desc = WEATHER_MAP.get(raw_desc, raw_desc)
            humidity = cur["humidity"]
            wind = cur["windspeedKmph"]
            return f"{matched_city_cn}当前天气{desc}，气温{temp}℃，湿度{humidity}%，风速{wind}km/h。"
    except Exception as e:
        print("[Weather Err]:", e)
        return None

class BridgeHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(content_length)

        try:
            req_data = json.loads(body.decode('utf-8'))
        except Exception as e:
            self.send_response(400)
            self.end_headers()
            self.wfile.write(b'{"error": "Invalid JSON"}')
            return

        messages = req_data.get('messages', [])
        
        # 1. 查找音频并调用 8317 进行首轮 ASR 转写与意图识别
        # 首先透传请求到 8317 获取初步识别和回答
        asr_payload = {
            "model": "gemini-3.8-flash",
            "messages": messages,
            "response_format": {"type": "json_object"}
        }

        try:
            req = urllib.request.Request(
                LLM_URL,
                data=json.dumps(asr_payload).encode('utf-8'),
                headers={
                    "Content-Type": "application/json",
                    "Authorization": f"Bearer {LLM_KEY}"
                }
            )
            with urllib.request.urlopen(req, timeout=15) as resp:
                res = json.loads(resp.read().decode())
                raw_content = res["choices"][0]["message"]["content"]
        except Exception as e:
            print("[Bridge LLM Err]:", e)
            self.send_response(500)
            self.end_headers()
            self.wfile.write(json.dumps({"error": str(e)}).encode())
            return

        # 清洗 JSON
        clean_content = raw_content
        if "```json" in clean_content:
            clean_content = clean_content.split("```json")[1]
        if "```" in clean_content:
            clean_content = clean_content.split("```")[0]
        clean_content = clean_content.strip()

        try:
            parsed = json.loads(clean_content)
        except Exception:
            parsed = {
                "user_text": "提问已收到",
                "reply": clean_content[:60],
                "lang": "zh",
                "voice": "zh-CN-XiaoxiaoNeural"
            }

        user_text = parsed.get("user_text", "")
        reply = parsed.get("reply", "")
        lang = parsed.get("lang", "zh")

        # 2. 如果用户问的是天气，注入实时气象监测技能
        if any(w in user_text for w in ["天气", "气温", "温度", "多少度", "weather", "temperature"]):
            print(f"[Bridge Skill Triggered]: 实时天气查询 - {user_text}")
            weather_report = get_realtime_weather(user_text)
            if weather_report:
                if lang == "en":
                    reply = f"Current weather: {weather_report}"
                else:
                    reply = weather_report
                parsed["reply"] = reply

        # 3. 如果用户问的是时间日期，注入当前系统时间
        elif any(w in user_text for w in ["几点", "现在时间", "今天几号", "星期几", "what time", "date"]):
            now = datetime.datetime.now()
            weekdays = ["一", "二", "三", "四", "五", "六", "日"]
            time_str = f"现在是{now.hour}点{now.minute}分，星期{weekdays[now.weekday()]}。"
            parsed["reply"] = time_str

        # 4. 回写响应格式（保持 OpenAI chat.completions 兼容结构）
        final_json_str = json.dumps(parsed, ensure_ascii=False)
        response_body = json.dumps({
            "id": "bridge_resp",
            "object": "chat.completion",
            "choices": [
                {
                    "index": 0,
                    "message": {
                        "role": "assistant",
                        "content": final_json_str
                    }
                }
            ]
        }, ensure_ascii=False)

        self.send_response(200)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.end_headers()
        self.wfile.write(response_body.encode('utf-8'))

    def do_GET(self):
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(b'{"status":"running","bridge":"openclaw-hardware-gateway"}')

def run():
    server_address = ('', PORT)
    with socketserver.TCPServer(server_address, BridgeHandler) as httpd:
        print(f"OpenClaw 智能硬件问答网关桥梁已启动: http://0.0.0.0:{PORT}")
        httpd.serve_forever()

if __name__ == '__main__':
    run()
