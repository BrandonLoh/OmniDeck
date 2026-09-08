/**
 * ai/broadcaster.js — AI 语音播报引擎
 *
 * 闹钟触发后的两步流水线：
 *   1. generateBroadcastSpeech() — 调用 LLM 生成"日期 + 温湿度 + 今日日程"播报文案；
 *      未配置 API Key 时回退到本地模板（离线 NAS 环境也能稳定播报）。
 *   2. ttsSpeak()               — 调用 TTS 引擎把文案合成为音频；
 *      未配置时返回 null，固件端会退回本地铃声 + 文本日志。
 *
 * 支持的供应商（通过环境变量配置，见 .env.example）：
 *   LLM: OPENAI_API_KEY + OPENAI_BASE_URL (兼容 OpenAI 协议的任意中转)
 *   TTS: TTS_API_KEY + TTS_BASE_URL + TTS_VOICE（同样走 OpenAI /v1/audio/speech 协议）
 */

/** 星期几的中文表述 */
const WEEK_CN = ['周一', '周二', '周三', '周四', '周五', '周六', '周日'];

/** 将 day_of_week(1-7) 映射为中文 */
const dayName = (d) => WEEK_CN[d - 1] || '';

/**
 * 生成播报文案
 * @param {object} p
 * @param {string} p.today         "2026-09-06"
 * @param {number} [p.temp]        温度 °C
 * @param {number} [p.humi]        湿度 %RH
 * @param {Array}  p.todaySchedules 今天的周日程数组
 * @param {object|null} p.topCountdown 置顶倒计时
 * @returns {Promise<string>}
 */
async function generateBroadcastSpeech(p) {
  const [y, m, d] = p.today.split('-');
  const weekday = dayName(((new Date(p.today + 'T00:00:00').getDay() + 6) % 7) + 1);
  const dateStr = `${y}年${parseInt(m)}月${parseInt(d)}日 ${weekday}`;

  const scheduleList = p.todaySchedules
    .map((s) => `${s.start_time}到${s.end_time}的${s.title}`)
    .join('，');

  // ---- 优先走 LLM ----
  if (process.env.OPENAI_API_KEY) {
    try {
      const prompt = `请生成一段 60 秒以内的早晨播报语音文案，使用第二人称、亲切自然，依次包含：今天的日期（${dateStr}）、` +
        `当前室内温湿度（${p.temp ?? '未知'}°C / ${p.humi ?? '未知'}%RH）、今日日程安排（${scheduleList || '今天没有日程，好好休息'}）` +
        (p.topCountdown ? `，以及一句距离「${p.topCountdown.title}」（${p.topCountdown.target_date}）的鼓励语` : '') +
        '。只输出播报正文，不要标题和解释。';
      const base = (process.env.OPENAI_BASE_URL || 'https://api.openai.com').replace(/\/$/, '');
      const resp = await fetch(`${base}/v1/chat/completions`, {
        method: 'POST',
        headers: {
          'Content-Type': 'application/json',
          Authorization: `Bearer ${process.env.OPENAI_API_KEY}`,
        },
        body: JSON.stringify({
          model: process.env.OPENAI_MODEL || 'gpt-4o-mini',
          messages: [{ role: 'user', content: prompt }],
        }),
      });
      const data = await resp.json();
      const text = data?.choices?.[0]?.message?.content;
      if (text) return text.trim();
    } catch (err) {
      console.error('[AI] LLM 调用失败，回退本地模板:', err.message);
    }
  }

  // ---- 本地模板兜底（无需联网）----
  let text = `早上好。今天是${dateStr}。`;
  if (p.temp != null) text += `当前室内温度${Math.round(p.temp)}度，湿度${Math.round(p.humi ?? 0)}%。`;
  text += scheduleList
    ? `今天的日程有：${scheduleList}。`
    : '今天没有安排日程，好好休息吧。';
  if (p.topCountdown) text += `距离「${p.topCountdown.title}」越来越近了，加油！`;
  return text;
}

/**
 * TTS 合成
 * @returns {Promise<{format: string, dataBase64: string}|null>} 失败/未配置返回 null
 */
async function ttsSpeak(text) {
  if (!process.env.TTS_API_KEY) return null;
  try {
    const base = (process.env.TTS_BASE_URL || 'https://api.openai.com').replace(/\/$/, '');
    const resp = await fetch(`${base}/v1/audio/speech`, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
        Authorization: `Bearer ${process.env.TTS_API_KEY}`,
      },
      body: JSON.stringify({
        model: process.env.TTS_MODEL || 'tts-1',
        voice: process.env.TTS_VOICE || 'alloy',
        input: text,
        // 固件端只有 OGG(Opus) 解复用器，不支持 MP3
        response_format: 'opus',
      }),
    });
    if (!resp.ok) throw new Error(`TTS HTTP ${resp.status}`);
    const buf = Buffer.from(await resp.arrayBuffer());
    return { format: 'mp3', dataBase64: buf.toString('base64') };
  } catch (err) {
    console.error('[AI] TTS 合成失败:', err.message);
    return null;
  }
}

module.exports = { generateBroadcastSpeech, ttsSpeak };
