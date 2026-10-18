---
name: dictation
description: 听写/默写出题：选词→生成语音（中文/英语）→以飞书语音消息发出。触发词："听写""默写""帮我选几个词""发语音过来"。
---

# dictation（听写语音）

把"选词 → 生成听写音频 → 发语音消息"一次做完。

## 一、确定科目与选词

| 科目 | 词源 | 选词规则 |
|------|------|----------|
| 语文 | 对应单元的 `语文/测验/默写/` 或 `音频/*打乱答案.txt` | 优先易错字、结构不重复，10–12 个词（用户指定数量时按用户要求） |
| 英语 | 对应单元词汇表（随堂单元练「Core contents to use」） | 优先四会单词/易错拼写，单词为主（如 dance / jump / player） |

- 用户只说"第二单元"时：先看最近同步的科目材料判断科目（如刚上传英语 U2 练习 → 英语）。
- **词的清单（答案）只留存/单独给，默认不在聊天正文里列出**，避免孩子提前看到答案；用户要答案再发。

## 二、生成音频

**必须统一调用内网 TTS 服务生成**（默认端点: `http://192.168.31.170:5050/v1/audio/speech`）。
**严禁自行写 Python/Shell 脚本去调用 macOS 系统 `say` 命令（Tingting 等机械音）！严禁使用已损坏的 ffmpeg！**

统一使用标准脚本：
```bash
python3 ~/.agents/skills/dictation/scripts/make_dictation.py \
  --subject english --words "dance,jump,player" --out /tmp/dict.m4a

python3 ~/.agents/skills/dictation/scripts/make_dictation.py \
  --subject chinese --words "寒霜,斜坡,凌乱,邮票,菊花" --out /tmp/dict.m4a
```

参数与音色说明：
- `--subject chinese|english`
  - 中文音色：内网 TTS `zh-CN-YunxiaNeural`（云夏纯净自然声）
  - 英文音色：内网 TTS 单词 `en-US-JennyNeural`，开场/结束中文为 `zh-CN-YunxiaNeural`
- `--repeat`（每个词读几遍；默认中文 3、英文 2）
- `--gap`（词内间隔秒，默认中文 2 / 英文 3）、`--word-gap`（词间间隔，默认中文 5 / 英文 8）
- `--out` 输出 m4a 路径

底层逻辑：
脚本已内置完整流程：逐词请求内网 TTS 服务 MP3 -> `afconvert` 转 22050Hz 单声道 WAV -> 拼接精确秒数静音轨 -> `afconvert` 压制输出 M4A/AAC。
无需且严禁在文本中加入 `<break>` 或 `<speak>` 等 SSML/HTML 标签。

## 三、发语音消息（已验证有效的路径）

生成后**用 message 工具发送音频**（不要只在回复正文写 MEDIA 路径：实测不会投递）：

```
message(action="send",
        media="/tmp/dict.m4a",
        target="user:<对方 open_id>",
        message="（简短说明：科目/单元/词数/朗读规则）")
```

- 当前会话可直接省略 `target`（自动回到当前会话）。
- 发送成功的判据：返回 `ok:true` 且 receipt 里包含 `kind:"media"` 的消息。
- 正文不要列出单词（防泄题）；可提示"要单词清单/对答案说一声"。

## 四、验收清单

1. 音频时长与词数匹配（每词 2–3 遍 + 间隔）；
2. 严禁出现 macOS 系统 `say` 机械音，必须是内网云夏/珍妮自然声；
3. 英语词的发音用英文语音（不要用中文语音读英文单词）；
4. 以语音消息送达（用户能直接点开听），文件格式 m4a/AAC 22050Hz。
