/* Standalone file:// UI. Never loads CSV predictions or sends network requests.
 * Playback is a listening aid; labels are anchored to original sample indices.
 */
(function () {
  "use strict";
  const C = window.HeroAnnotation;
  const $ = id => document.getElementById(id);
  let audio = null, view = null, selection = null, labels = [], editingId = null;
  let protocol = {baselineSeconds: 30, normalBaselineConfirmed: false, split: "development"};
  let history = new C.History(), dirty = false, loadTicket = 0, busy = false;
  let context = null, audioBuffer = null, node = null, gainNode = null, playTicket = 0;
  let playInfo = null, animation = null, drag = null;
  const envelopeCache = new Map();
  const names = {anomaly_impact: "敲击", anomaly_burst: "连续突发", normal_background: "正常背景", normal_transition: "正常切换"};
  const confidenceNames = {high: "高", medium: "中", low: "低"};
  const geometry = {left: 62, right: 18, top: 15, bottom: 30};

  function report(text) { $("status").textContent = text; $("error").hidden = true; }
  function fail(error) { $("error").textContent = error.message || String(error); $("error").hidden = false; }
  function safely(action) { return async (...args) => { try { await action(...args); } catch (error) { fail(error); } }; }
  function on(id, action, event = "click") { $(id).addEventListener(event, safely(action)); }
  function snapshot() { return {labels: labels.map(l => ({...l})), protocol: {...protocol}}; }
  function updateDirty() { dirty = true; $("save-state").textContent = "有未保存的草稿更改。"; }
  function remember() { history.push(snapshot()); updateDirty(); }
  function clearEdit() {
    editingId = null; $("editing").textContent = "新标签"; $("save-label").textContent = "添加标签";
  }
  function applySnapshot(state) {
    C.validateLabels(state.labels, audio, state.protocol);
    labels = state.labels; protocol = state.protocol; clearEdit(); updateDirty(); syncProtocol(); renderLabels(); draw();
  }
  function syncProtocol() {
    $("baseline").value = protocol.baselineSeconds;
    $("split").value = protocol.split;
    $("baseline-confirmed").checked = protocol.normalBaselineConfirmed;
  }
  function setSelection(start, end, updateFields = true) {
    selection = {start: C.clamp(Math.min(start, end), 0, audio.sampleCount), end: C.clamp(Math.max(start, end), 0, audio.sampleCount)};
    if (updateFields) {
      // Keep round-trippable sample times even at the end of the recording.
      $("start-time").value = selection.start / audio.sampleRate;
      $("end-time").value = selection.end / audio.sampleRate;
    }
    $("selection-info").textContent = `原始采样点 ${selection.start} → ${selection.end} ｜ 长度 ${((selection.end - selection.start) / audio.sampleRate * 1000).toFixed(3)} ms`;
    draw();
  }
  function readSelection() {
    if ($("start-time").value === "" || $("end-time").value === "") throw new Error("请先选择声音的开始和结束。");
    const start = C.secondsToSample($("start-time").valueAsNumber, audio);
    const end = C.secondsToSample($("end-time").valueAsNumber, audio);
    if (end <= start) throw new Error("结束必须晚于开始；请拖出一个非空区间。");
    setSelection(start, end);
    return {start, end};
  }
  function setView(center, span) {
    view = C.windowAround(center, Math.max(audio.sampleRate * 0.01, span), audio.sampleCount);
    envelopeCache.delete("detail");
    $("view-info").textContent = `${(view.start / audio.sampleRate).toFixed(3)}–${(view.end / audio.sampleRate).toFixed(3)} s`;
    draw();
  }
  function center() { return selection ? (selection.start + selection.end) / 2 : (view.start + view.end) / 2; }

  function gridStep(span, width) {
    const raw = span / Math.max(2, Math.floor(width / 90));
    const base = 10 ** Math.floor(Math.log10(raw));
    return [1, 2, 5, 10].map(v => v * base).find(v => v >= raw);
  }
  function drawCanvas(id, range, overview) {
    const canvas = $(id), width = canvas.clientWidth, height = canvas.clientHeight;
    if (width <= geometry.left + geometry.right) return;
    const dpr = Math.min(window.devicePixelRatio || 1, 3);
    const w = Math.round(width * dpr), h = Math.round(height * dpr);
    if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; }
    const ctx = canvas.getContext("2d");
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0); ctx.clearRect(0, 0, width, height);
    const left = geometry.left, top = geometry.top, right = width - geometry.right, bottom = height - geometry.bottom;
    const plotWidth = right - left, plotHeight = bottom - top;
    const scale = overview ? 1 : Number($("gain").value), limit = 1 / scale;
    const x = sample => left + (sample - range.start) / (range.end - range.start) * plotWidth;
    const y = value => top + (1 - C.clamp(value / limit, -1, 1)) * plotHeight / 2;
    ctx.save(); ctx.beginPath(); ctx.rect(left, top, plotWidth, plotHeight); ctx.clip();
    ctx.fillStyle = "#f0f3f6";
    ctx.fillRect(left, top, Math.max(0, x(protocol.baselineSeconds * audio.sampleRate) - left), plotHeight);
    if (overview) { ctx.fillStyle = "#d9edf4"; ctx.fillRect(x(view.start), top, x(view.end) - x(view.start), plotHeight); }
    const step = gridStep((range.end - range.start) / audio.sampleRate, plotWidth);
    ctx.lineWidth = 1; ctx.strokeStyle = "#dde6ec";
    for (let t = Math.ceil(range.start / audio.sampleRate / step) * step; t <= range.end / audio.sampleRate + step * 1e-6; t += step) {
      ctx.beginPath(); ctx.moveTo(x(t * audio.sampleRate), top); ctx.lineTo(x(t * audio.sampleRate), bottom); ctx.stroke();
    }
    for (const value of [-limit, 0, limit]) { ctx.beginPath(); ctx.moveTo(left, y(value)); ctx.lineTo(right, y(value)); ctx.stroke(); }
    for (const label of labels) {
      ctx.fillStyle = "#148c681c"; ctx.fillRect(x(label.startSample), top, Math.max(1, x(label.endSample) - x(label.startSample)), plotHeight);
    }
    if (selection) {
      ctx.fillStyle = "#2165ad24"; ctx.fillRect(x(selection.start), top, Math.max(1, x(selection.end) - x(selection.start)), plotHeight);
    }
    const key = `${range.start}:${range.end}:${Math.floor(plotWidth)}`;
    let cache = envelopeCache.get(id);
    if (!cache || cache.key !== key) {
      cache = {key, bins: C.envelope(audio.samples, range.start, range.end, Math.floor(plotWidth))}; envelopeCache.set(id, cache);
    }
    ctx.strokeStyle = "#087f9c"; ctx.lineWidth = 1; ctx.beginPath();
    for (const bin of cache.bins) {
      const px = x((bin.first + bin.last - 1) / 2);
      const y1 = y(bin.max), y2 = y(bin.min);
      ctx.moveTo(px, y1); ctx.lineTo(px, Math.max(y1 + 0.7, y2));
    }
    ctx.stroke();
    if (selection) {
      ctx.strokeStyle = "#235ea0"; ctx.lineWidth = 1.5;
      for (const sample of [selection.start, selection.end]) { ctx.beginPath(); ctx.moveTo(x(sample), top); ctx.lineTo(x(sample), bottom); ctx.stroke(); }
    }
    if (playInfo && node) {
      let elapsed = Math.max(0, context.currentTime - playInfo.clock) * playInfo.rate;
      elapsed = playInfo.loop ? elapsed % (playInfo.end - playInfo.start) : Math.min(elapsed, playInfo.end - playInfo.start);
      const px = x((playInfo.start + elapsed) * audio.sampleRate);
      ctx.strokeStyle = "#b04423"; ctx.lineWidth = 2; ctx.beginPath(); ctx.moveTo(px, top); ctx.lineTo(px, bottom); ctx.stroke();
    }
    ctx.restore();
    ctx.strokeStyle = "#b9c9d5"; ctx.lineWidth = 1; ctx.strokeRect(left, top, plotWidth, plotHeight);
    ctx.fillStyle = "#536777"; ctx.font = "12px system-ui"; ctx.textBaseline = "middle"; ctx.textAlign = "right";
    for (const value of [-limit, 0, limit]) ctx.fillText(value.toFixed(scale > 16 ? 3 : 2), left - 8, y(value));
    ctx.textBaseline = "top";
    for (let t = Math.ceil(range.start / audio.sampleRate / step) * step; t <= range.end / audio.sampleRate + step * 1e-6; t += step) {
      const px = x(t * audio.sampleRate);
      ctx.textAlign = px < left + 25 ? "left" : px > right - 25 ? "right" : "center";
      ctx.fillText(t.toFixed(step < 0.01 ? 3 : step < 1 ? 2 : 1), px, bottom + 8);
    }
  }
  function draw() {
    if (!audio) return;
    drawCanvas("overview", {start: 0, end: audio.sampleCount}, true);
    drawCanvas("detail", view, false);
  }
  function pointerSample(event, id, range) {
    const canvas = $(id), rect = canvas.getBoundingClientRect();
    // Match the canvas content box used by drawCanvas, excluding its CSS border.
    return C.positionToSample(event.clientX - rect.left - canvas.clientLeft - geometry.left,
      canvas.clientWidth - geometry.left - geometry.right, range);
  }
  $("overview").addEventListener("pointerdown", event => {
    if (!audio || busy) return;
    setView(pointerSample(event, "overview", {start: 0, end: audio.sampleCount}), view.end - view.start);
  });
  $("detail").addEventListener("pointerdown", event => {
    if (!audio || busy || (event.button !== 0 && event.pointerType === "mouse")) return;
    stopPlayback(); drag = {pointer: event.pointerId, start: pointerSample(event, "detail", view)};
    $("detail").setPointerCapture(event.pointerId); setSelection(drag.start, drag.start);
  });
  $("detail").addEventListener("pointermove", event => {
    if (drag && event.pointerId === drag.pointer) setSelection(drag.start, pointerSample(event, "detail", view));
  });
  for (const eventName of ["pointerup", "pointercancel", "lostpointercapture"]) $("detail").addEventListener(eventName, () => { drag = null; });
  new ResizeObserver(() => draw()).observe($("detail"));

  function renderLabels() {
    const body = $("labels-body"); body.replaceChildren();
    const sorted = [...labels].sort((a, b) => a.startSample - b.startSample || a.id - b.id);
    sorted.forEach((label, index) => {
      const tr = document.createElement("tr");
      for (const value of [index + 1, `${(label.startSample / audio.sampleRate).toFixed(6)} / ${(label.endSample / audio.sampleRate).toFixed(6)}`,
        names[label.className], `${confidenceNames[label.confidence]} / ${label.annotator}`]) {
        const td = document.createElement("td"); td.textContent = value; tr.append(td);
      }
      const actions = document.createElement("td");
      const edit = document.createElement("button"); edit.type = "button"; edit.textContent = "编辑"; edit.setAttribute("aria-label", `编辑标签 ${index + 1}`);
      edit.addEventListener("click", () => {
        editingId = label.id; setSelection(label.startSample, label.endSample);
        setView((label.startSample + label.endSample) / 2, Math.max(audio.sampleRate * 0.5, (label.endSample - label.startSample) * 3));
        $("class-name").value = label.className; $("confidence").value = label.confidence; $("annotator").value = label.annotator;
        $("editing").textContent = `正在编辑第 ${index + 1} 条`; $("save-label").textContent = "保存修改";
      });
      const remove = document.createElement("button"); remove.type = "button"; remove.textContent = "删除"; remove.setAttribute("aria-label", `删除标签 ${index + 1}`);
      remove.addEventListener("click", () => { remember(); labels = labels.filter(l => l.id !== label.id); clearEdit(); renderLabels(); draw(); });
      actions.append(edit, remove); tr.append(actions); body.append(tr);
    });
    $("empty-labels").hidden = labels.length > 0;
    $("label-count").textContent = `${labels.length} 条标签 · ${labels.filter(l => l.className.startsWith("anomaly_")).length} 条异常`;
    $("undo").disabled = !history.undoStack.length; $("redo").disabled = !history.redoStack.length;
  }
  function stopPlayback() {
    playTicket++;
    if (node) { node.onended = null; try { node.stop(); } catch (_) { /* Already ended. */ } node.disconnect(); node = null; }
    if (gainNode) { gainNode.disconnect(); gainNode = null; }
    if (animation) cancelAnimationFrame(animation);
    animation = null; playInfo = null; $("play-state").textContent = "已停止"; draw();
  }
  async function play(start, end) {
    if (!Number.isFinite(start) || !Number.isFinite(end) || end <= start) throw new Error("没有可播放的区间。");
    stopPlayback(); const ticket = playTicket;
    const AudioContextClass = window.AudioContext || window.webkitAudioContext;
    if (!AudioContextClass) throw new Error("当前浏览器不支持 Web Audio，请使用更新的浏览器。");
    if (!context) context = new AudioContextClass();
    await context.resume(); if (ticket !== playTicket) return;
    if (!audioBuffer) {
      // Avoid decodeAudioData: it may resample to the context's rate. Our
      // coordinate system is always the WAV's original rate and sample count.
      audioBuffer = context.createBuffer(1, audio.sampleCount, audio.sampleRate);
      audioBuffer.copyToChannel(audio.samples, 0);
    }
    node = context.createBufferSource(); node.buffer = audioBuffer;
    const rate = Number($("rate").value), loop = $("loop").checked;
    node.playbackRate.value = rate; node.loop = loop; node.loopStart = start; node.loopEnd = end;
    gainNode = context.createGain(); gainNode.gain.value = Number($("volume").value);
    node.connect(gainNode); gainNode.connect(context.destination);
    const current = node; node.onended = () => { if (node === current) stopPlayback(); };
    playInfo = {start, end, rate, loop, clock: context.currentTime};
    if (loop) node.start(0, start); else node.start(0, start, end - start);
    $("play-state").textContent = `${loop ? "循环" : "播放"} ${rate}× · ${start.toFixed(3)}–${end.toFixed(3)} s`;
    function animate() { if (node) { draw(); animation = requestAnimationFrame(animate); } }
    animate();
  }
  function download(contents, name, type) {
    const url = URL.createObjectURL(new Blob([contents], {type}));
    const link = document.createElement("a"); link.href = url; link.download = name; document.body.append(link); link.click(); link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 30000);
  }
  function prefix() { return `hero-labels-${audio.sha256.slice(0, 12)}`; }

  on("audio-file", async event => {
    const file = event.target.files[0]; if (!file) return;
    if (dirty && !window.confirm("有未下载的草稿。确定放弃当前更改并载入另一段录音？")) { event.target.value = ""; return; }
    if (file.size > C.MAX_BYTES) throw new Error("录音超过 128 MiB。");
    if (!window.crypto?.subtle) throw new Error("当前环境不支持安全音频指纹，请在本地 Chrome 或支持 Web Crypto 的浏览器中打开。");
    const ticket = ++loadTicket; busy = true; $("workspace").disabled = true; stopPlayback(); report("正在读取原始样本并计算音频指纹…");
    try {
      const buffer = await file.arrayBuffer();
      const parsed = C.parseWav(buffer);
      const digest = await crypto.subtle.digest("SHA-256", buffer);
      if (ticket !== loadTicket) return;
      audio = {...parsed, sha256: Array.from(new Uint8Array(digest), b => b.toString(16).padStart(2, "0")).join(""), name: file.name, bytes: file.size};
      labels = []; selection = null; history = new C.History(); dirty = false; audioBuffer = null; envelopeCache.clear(); clearEdit();
      protocol = {baselineSeconds: Math.min(30, audio.duration), normalBaselineConfirmed: false, split: "development"}; syncProtocol();
      $("start-time").value = ""; $("end-time").value = ""; $("selection-info").textContent = "尚未选择区间。";
      $("jump-time").value = Math.min(30, audio.duration); $("jump-time").max = audio.duration; $("baseline").max = audio.duration;
      $("audio-info").textContent = `${file.name} · ${audio.sampleRate.toLocaleString()} Hz · ${audio.duration.toFixed(6)} s · SHA-256 ${audio.sha256}`;
      $("save-state").textContent = "尚未保存草稿。";
      renderLabels(); setView(Math.min(32.5, audio.duration / 2 + 15) * audio.sampleRate, Math.min(audio.sampleCount, audio.sampleRate * 5));
      report("录音已载入。请独立回听并标注；页面不会自动寻找事件。");
    } finally { if (ticket === loadTicket) { busy = false; $("workspace").disabled = !audio; } }
  }, "change");
  on("jump", () => setView(C.secondsToSample($("jump-time").valueAsNumber, audio), view.end - view.start));
  on("pan-left", () => setView((view.start + view.end) / 2 - (view.end - view.start) / 2, view.end - view.start));
  on("pan-right", () => setView((view.start + view.end) / 2 + (view.end - view.start) / 2, view.end - view.start));
  on("zoom-in", () => setView(center(), (view.end - view.start) / 2));
  on("zoom-out", () => setView(center(), (view.end - view.start) * 2));
  on("fit-all", () => setView(audio.sampleCount / 2, audio.sampleCount));
  on("fit-selection", () => { const s = readSelection(); setView((s.start + s.end) / 2, (s.end - s.start) * 1.5); });
  on("gain", draw, "change");
  for (const id of ["start-time", "end-time"]) on(id, () => { if ($("start-time").value !== "" && $("end-time").value !== "") readSelection(); }, "change");
  on("play-view", () => play(view.start / audio.sampleRate, view.end / audio.sampleRate));
  on("play-selection", () => {
    const s = readSelection(), padding = $("padding").valueAsNumber;
    if (!Number.isFinite(padding) || padding < 0 || padding > 2) throw new Error("回听前后留白必须在 0–2 秒之间。");
    return play(Math.max(0, s.start / audio.sampleRate - padding), Math.min(audio.duration, s.end / audio.sampleRate + padding));
  });
  on("stop", stopPlayback);
  for (const id of ["rate", "loop"]) on(id, stopPlayback, "change");
  on("volume", () => { if (gainNode) gainNode.gain.value = Number($("volume").value); }, "input");
  on("save-label", () => {
    const s = readSelection();
    const label = {id: editingId || Math.max(0, ...labels.map(l => l.id)) + 1, startSample: s.start, endSample: s.end,
      className: $("class-name").value, operatingState: "steady", confidence: $("confidence").value, annotator: $("annotator").value.trim()};
    const next = editingId ? labels.map(l => l.id === editingId ? label : l) : [...labels, label];
    C.validateLabels(next, audio, protocol); remember(); labels = next; clearEdit(); renderLabels(); draw(); report("标签已加入工作区；请记得下载草稿。");
  });
  on("new-label", () => { clearEdit(); selection = null; $("start-time").value = ""; $("end-time").value = ""; $("selection-info").textContent = "尚未选择区间。"; draw(); });
  on("undo", () => { const previous = history.undo(snapshot()); if (previous) applySnapshot(previous); });
  on("redo", () => { const next = history.redo(snapshot()); if (next) applySnapshot(next); });
  for (const id of ["baseline", "split", "baseline-confirmed"]) on(id, () => {
    const next = {baselineSeconds: $("baseline").valueAsNumber, split: $("split").value, normalBaselineConfirmed: $("baseline-confirmed").checked};
    try { C.validateLabels(labels, audio, next); } catch (error) { syncProtocol(); throw error; }
    remember(); protocol = next; renderLabels(); draw();
  }, "change");
  on("save-draft", () => {
    const draft = C.makeDraft(audio, protocol, labels);
    download(JSON.stringify(draft, null, 2) + "\n", `${prefix()}.draft.json`, "application/json;charset=utf-8");
    dirty = false; $("save-state").textContent = "已发起草稿下载，请确认浏览器已保存。";
  });
  on("draft-file", async event => {
    const file = event.target.files[0]; if (!file) return;
    event.target.value = "";
    if (file.size > 8 * 1024 * 1024) throw new Error("草稿超过 8 MiB。");
    const currentAudio = audio;
    const parsed = JSON.parse(await file.text());
    if (currentAudio !== audio) throw new Error("读取草稿时录音已改变，请重新选择草稿。");
    const state = C.loadDraft(parsed, audio);
    if (dirty && !window.confirm("导入草稿会替换当前标签；仍可撤销。是否继续？")) return;
    remember(); applySnapshot(state); report("草稿指纹验证通过，标签已恢复；可用撤销恢复导入前状态。");
  }, "change");
  on("export-csv", () => {
    const csv = C.exportCsv(labels, audio, protocol);
    if (!labels.length && !window.confirm("当前没有标签。仅当你确认这是没有目标事件的负样本时，才导出空表。继续？")) return;
    download(csv, `${prefix()}.human-labels.csv`, "text/csv;charset=utf-8");
    report("已发起 CSV 下载。请同时保存草稿，并确认文件后再放入对应 session；不会自动覆盖项目文件。");
  });
  window.addEventListener("beforeunload", event => { if (dirty) { event.preventDefault(); event.returnValue = ""; } });
  window.addEventListener("pagehide", stopPlayback);
})();
