/* Pure, dependency-free annotation logic; shared by the browser and Node tests.
 * Coordinates and draft boundaries always refer to ORIGINAL WAV sample indices.
 * No detector output, peak picking, inference, or network access belongs here.
 */
(function (root, factory) {
  const api = factory();
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.HeroAnnotation = api;
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";
  const VERSION = 1;
  const MAX_BYTES = 128 * 1024 * 1024;
  const CLASSES = ["anomaly_impact", "anomaly_burst", "normal_background", "normal_transition"];
  const CONFIDENCES = ["high", "medium", "low"];
  function requireThat(condition, message) { if (!condition) throw new Error(message); }
  const clamp = (value, min, max) => Math.max(min, Math.min(max, value));

  function parseWav(buffer) {
    requireThat(buffer instanceof ArrayBuffer && buffer.byteLength >= 44, "不是完整的 WAV 文件。");
    requireThat(buffer.byteLength <= MAX_BYTES, "第一版最多支持 128 MiB 的录音。");
    const data = new DataView(buffer);
    const text = (p, n) => String.fromCharCode(...new Uint8Array(buffer, p, n));
    requireThat(text(0, 4) === "RIFF" && text(8, 4) === "WAVE", "只支持 little-endian RIFF/WAVE。");
    const limit = data.getUint32(4, true) + 8;
    requireThat(limit === buffer.byteLength, "WAV 长度与文件头不符；录音可能未正常结束。");
    let format = null, payload = null, offset = 12;
    while (offset < limit) {
      requireThat(offset + 8 <= limit, "WAV chunk 头不完整。");
      const tag = text(offset, 4), size = data.getUint32(offset + 4, true), start = offset + 8;
      const next = start + size + (size % 2);
      requireThat(next <= limit, "WAV chunk 内容或 padding 不完整。");
      if (tag === "fmt ") {
        requireThat(!format && size >= 16, "WAV fmt chunk 重复或无效。");
        format = {code: data.getUint16(start, true), channels: data.getUint16(start + 2, true),
          sampleRate: data.getUint32(start + 4, true), byteRate: data.getUint32(start + 8, true),
          blockAlign: data.getUint16(start + 12, true), bits: data.getUint16(start + 14, true)};
      } else if (tag === "data") {
        requireThat(!payload, "第一版不支持多个 data chunk。");
        payload = {start, size};
      }
      offset = next;
    }
    requireThat(format && payload, "WAV 缺少 fmt 或 data chunk。");
    requireThat(format.code === 1 && format.channels === 1 && format.bits === 16,
      "请选择供回听的单声道 PCM16 WAV，不要选择 float32 分析录音。");
    requireThat(format.sampleRate >= 8000 && format.sampleRate <= 96000 &&
      format.blockAlign === 2 && format.byteRate === format.sampleRate * 2,
    "不支持此采样率或 WAV 格式字段不一致（支持 8–96 kHz）。");
    requireThat(payload.size > 0 && payload.size % 2 === 0, "WAV 样本为空或被截断。");
    const sampleCount = payload.size / 2;
    const samples = new Float32Array(sampleCount);
    for (let i = 0; i < sampleCount; i++) samples[i] = data.getInt16(payload.start + i * 2, true) / 32768;
    return {samples, sampleRate: format.sampleRate, sampleCount, duration: sampleCount / format.sampleRate};
  }

  function windowAround(center, span, sampleCount) {
    const width = clamp(Math.round(span), 1, sampleCount);
    const start = clamp(Math.round(center - width / 2), 0, sampleCount - width);
    return {start, end: start + width};
  }
  function positionToSample(x, width, view) {
    requireThat(Number.isFinite(x) && width > 0 && view.end > view.start, "无效的波形坐标。");
    return clamp(Math.round(view.start + clamp(x / width, 0, 1) * (view.end - view.start)), view.start, view.end);
  }
  function secondsToSample(seconds, audio) {
    requireThat(Number.isFinite(seconds) && seconds >= 0 && seconds <= audio.duration, "时间超出录音范围。");
    return clamp(Math.round(seconds * audio.sampleRate), 0, audio.sampleCount);
  }
  function envelope(samples, start, end, columns) {
    requireThat(Number.isInteger(start) && Number.isInteger(end) && start >= 0 && end <= samples.length && end > start,
      "无效的波形范围。");
    const count = Math.min(end - start, Math.max(1, Math.floor(columns)));
    const bins = [];
    for (let b = 0; b < count; b++) {
      const first = start + Math.floor(b * (end - start) / count);
      const last = start + Math.floor((b + 1) * (end - start) / count);
      let min = Infinity, max = -Infinity;
      for (let i = first; i < last; i++) { min = Math.min(min, samples[i]); max = Math.max(max, samples[i]); }
      bins.push({first, last, min, max});
    }
    return bins;
  }
  function validProtocol(protocol, audio) {
    requireThat(protocol && ["development", "held-out"].includes(protocol.split), "请选择 development 或 held-out。");
    requireThat(Number.isFinite(protocol.baselineSeconds) && protocol.baselineSeconds >= 0 &&
      protocol.baselineSeconds <= audio.duration, "基线时间必须在录音范围内。");
    requireThat(typeof protocol.normalBaselineConfirmed === "boolean", "基线确认字段无效。");
  }
  function validateLabels(labels, audio, protocol) {
    validProtocol(protocol, audio);
    requireThat(Array.isArray(labels) && labels.length <= 10000, "标签列表无效或超过 10000 条。");
    const ids = new Set();
    const anomalies = [];
    for (const label of labels) {
      requireThat(label && Number.isSafeInteger(label.id) && label.id > 0 && !ids.has(label.id), "标签 ID 重复或无效。");
      ids.add(label.id);
      requireThat(Number.isInteger(label.startSample) && Number.isInteger(label.endSample) && label.startSample >= 0 &&
        label.endSample > label.startSample && label.endSample <= audio.sampleCount, "每条标签必须有合法且非空的起止区间。");
      requireThat(CLASSES.includes(label.className) && label.operatingState === "steady" &&
        CONFIDENCES.includes(label.confidence), "标签类别、工况或置信度无效。");
      // The C++ CSV reader intentionally uses unquoted fields. Reject separators,
      // newlines and spreadsheet formula prefixes instead of exporting ambiguous CSV.
      requireThat(typeof label.annotator === "string" && /^[\p{L}\p{N}][\p{L}\p{N} ._-]{0,39}$/u.test(label.annotator),
        "标注人请填 1–40 字的姓名/缩写；以字母、汉字或数字开头，不含逗号、引号和换行。");
      if (label.className.startsWith("anomaly_")) {
        requireThat(label.startSample >= Math.ceil(protocol.baselineSeconds * audio.sampleRate),
          "异常标签落在计划基线中；请复核录音和实验协议，不要为通过检查移动真实事件。");
        anomalies.push(label);
      }
    }
    anomalies.sort((a, b) => a.startSample - b.startSample || a.endSample - b.endSample);
    for (let i = 1; i < anomalies.length; i++) requireThat(anomalies[i].startSample >= anomalies[i - 1].endSample,
      "异常区间不能重叠；请复核两条标签的边界。");
    return labels;
  }
  function exportCsv(labels, audio, protocol) {
    validateLabels(labels, audio, protocol);
    requireThat(protocol.normalBaselineConfirmed, "导出前请确认计划基线确实是正常背景声。");
    const lines = ["start_seconds,end_seconds,class,operating_state,confidence,annotator"];
    for (const label of [...labels].sort((a, b) => a.startSample - b.startSample || a.id - b.id)) {
      // Number.toString round-trips the double. Fixed decimal rounding could move
      // an end-of-file label just beyond the duration, which the C++ CLI rejects.
      lines.push([label.startSample / audio.sampleRate, label.endSample / audio.sampleRate,
        label.className, label.operatingState, label.confidence, label.annotator].join(","));
    }
    return lines.join("\n") + "\n";
  }
  function makeDraft(audio, protocol, labels) {
    validateLabels(labels, audio, protocol);
    requireThat(/^[a-f0-9]{64}$/.test(audio.sha256), "音频 SHA-256 无效。");
    return {app: "HERO-Audio manual annotator", schemaVersion: VERSION,
      source: {sha256: audio.sha256, name: audio.name, bytes: audio.bytes,
        sampleRate: audio.sampleRate, sampleCount: audio.sampleCount},
      protocol: {...protocol}, labels: labels.map(label => ({...label}))};
  }
  function loadDraft(draft, audio) {
    requireThat(draft && draft.app === "HERO-Audio manual annotator" && draft.schemaVersion === VERSION, "不支持此草稿版本。");
    const source = draft.source;
    requireThat(source && source.sha256 === audio.sha256 && source.bytes === audio.bytes &&
      source.sampleRate === audio.sampleRate && source.sampleCount === audio.sampleCount,
    "草稿与当前录音的指纹或采样信息不符，已拒绝导入。");
    validateLabels(draft.labels, audio, draft.protocol);
    return {protocol: {...draft.protocol}, labels: draft.labels.map(label => ({...label}))};
  }
  class History {
    constructor(limit = 100) { this.undoStack = []; this.redoStack = []; this.limit = limit; }
    push(snapshot) { this.undoStack.push(JSON.stringify(snapshot)); this.undoStack = this.undoStack.slice(-this.limit); this.redoStack = []; }
    undo(current) { if (!this.undoStack.length) return null; this.redoStack.push(JSON.stringify(current)); return JSON.parse(this.undoStack.pop()); }
    redo(current) { if (!this.redoStack.length) return null; this.undoStack.push(JSON.stringify(current)); return JSON.parse(this.redoStack.pop()); }
  }
  return {VERSION, MAX_BYTES, CLASSES, CONFIDENCES, clamp, parseWav, windowAround, positionToSample,
    secondsToSample, envelope, validProtocol, validateLabels, exportCsv, makeDraft, loadDraft, History};
});
