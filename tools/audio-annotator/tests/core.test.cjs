"use strict";
const {test} = require("node:test");
const assert = require("node:assert/strict");
const fs = require("node:fs");
const path = require("node:path");
const C = require("../core.js");
const {makeWav, arrayBuffer, metadata} = require("./fixtures.cjs");
const bytes = makeWav();
const audio = metadata(bytes, C.parseWav(arrayBuffer(bytes)));
const protocol = {baselineSeconds: 1, normalBaselineConfirmed: true, split: "development"};
const label = {id: 1, startSample: 88200, endSample: 92610, className: "anomaly_impact", operatingState: "steady", confidence: "high", annotator: "test-annotator"};

for (const sampleRate of [44100, 48000, 96000]) test(`original sample clock preserved at ${sampleRate} Hz`, () => {
  const wav = C.parseWav(arrayBuffer(makeWav({sampleRate, junk: true})));
  assert.equal(wav.sampleRate, sampleRate); assert.equal(wav.sampleCount, sampleRate * 4); assert.equal(wav.duration, 4);
  for (const s of [0, 1, 257, sampleRate, wav.sampleCount]) assert.equal(C.secondsToSample(s / sampleRate, wav), s);
});
test("PCM16 integer extrema and little-endian samples preserved", () => {
  const b = makeWav(); b.writeInt16LE(-32768, 44); b.writeInt16LE(32767, 46);
  const result = C.parseWav(arrayBuffer(b)); assert.equal(result.samples[0], -1); assert.equal(result.samples[1], 32767 / 32768);
});
for (const [name, options] of [["stereo", {channels: 2}], ["float analysis WAV", {bits: 32, code: 3}], ["bad sample rate", {sampleRate: 1000}], ["empty", {seconds: 0}]])
  test(`reject ${name}`, () => assert.throws(() => C.parseWav(arrayBuffer(makeWav(options)))));
test("reject truncated or unfinished WAV and inconsistent format fields", () => {
  assert.throws(() => C.parseWav(arrayBuffer(bytes.subarray(0, -2))));
  const b = Buffer.from(bytes); b.writeUInt32LE(0, 4); assert.throws(() => C.parseWav(arrayBuffer(b)));
  b.writeUInt32LE(b.length - 8, 4); b.writeUInt32LE(123, 28); assert.throws(() => C.parseWav(arrayBuffer(b)));
});
test("reject malformed chunk bounds and duplicate data chunks", () => {
  const b = Buffer.from(bytes); b.writeUInt32LE(0xffffffff, 40); assert.throws(() => C.parseWav(arrayBuffer(b)));
  const duplicate = Buffer.concat([bytes, bytes.subarray(36)]); duplicate.writeUInt32LE(duplicate.length - 8, 4);
  assert.throws(() => C.parseWav(arrayBuffer(duplicate)));
});
test("sample coordinate mapping clamps and remains invariant across zoom", () => {
  assert.equal(C.positionToSample(200, 400, {start: 0, end: 4000}), 2000);
  assert.equal(C.positionToSample(200, 400, {start: 1900, end: 2100}), 2000);
  assert.equal(C.positionToSample(-10, 400, {start: 100, end: 200}), 100);
  assert.equal(C.positionToSample(900, 400, {start: 100, end: 200}), 200);
  assert.deepEqual(C.windowAround(-1, 100, 1000), {start: 0, end: 100});
  assert.deepEqual(C.windowAround(1000, 100, 1000), {start: 900, end: 1000});
});
test("envelope includes every sample, extremes and final partial bin", () => {
  const samples = new Float32Array([0, -1, 0.5, 0, 0.9, 0, -0.8]);
  const bins = C.envelope(samples, 0, samples.length, 3);
  assert.equal(bins[0].first, 0); assert.equal(bins.at(-1).last, samples.length);
  assert.equal(Math.min(...bins.map(b => b.min)), -1);
  assert.equal(Math.max(...bins.map(b => b.max)), samples[4]);
  for (let i = 1; i < bins.length; i++) assert.equal(bins[i - 1].last, bins[i].first);
});
test("CSV is sorted, compatible unquoted schema, no fabricated labels", () => {
  const csv = C.exportCsv([{...label, id: 2, startSample: 132300, endSample: 136710}, label], audio, protocol);
  assert.equal(csv.split("\n")[0], "start_seconds,end_seconds,class,operating_state,confidence,annotator");
  assert.equal(csv.split("\n")[1], "2,2.1,anomaly_impact,steady,high,test-annotator");
  assert.equal(C.exportCsv([], audio, protocol).trim().split("\n").length, 1);
});
test("end-of-file time round-trips without rounding past duration", () => {
  const wav = makeWav({seconds: 2643456 / 44100}); const a = metadata(wav, C.parseWav(arrayBuffer(wav)));
  const l = {...label, startSample: a.sampleCount - 100, endSample: a.sampleCount};
  const end = Number(C.exportCsv([l], a, protocol).split("\n")[1].split(",")[1]); assert.equal(end, a.duration);
});
for (const [name, patch] of [["negative", {startSample: -1}], ["reversed", {endSample: 88000}], ["zero length", {endSample: 88200}],
  ["fractional sample", {startSample: 88200.5}], ["past EOF", {endSample: audio.sampleCount + 1}],
  ["unknown class", {className: "fault"}], ["state", {operatingState: "startup"}], ["confidence", {confidence: "certain"}],
  ["comma", {annotator: "a,b"}], ["quote", {annotator: 'a"b'}], ["formula", {annotator: "=SUM(1)"}], ["newline", {annotator: "a\nb"}]])
  test(`invalid label rejected: ${name}`, () => assert.throws(() => C.validateLabels([{...label, ...patch}], audio, protocol)));
test("baseline anomaly rejected; background is allowed", () => {
  const early = {...label, startSample: 0, endSample: 400};
  assert.throws(() => C.validateLabels([early], audio, protocol));
  assert.doesNotThrow(() => C.validateLabels([{...early, className: "normal_background"}], audio, protocol));
  assert.throws(() => C.exportCsv([label], audio, {...protocol, normalBaselineConfirmed: false}));
});
test("overlap and duplicate IDs rejected, touching intervals allowed", () => {
  assert.throws(() => C.validateLabels([label, {...label}], audio, protocol));
  assert.throws(() => C.validateLabels([label, {...label, id: 2, startSample: label.startSample + 1}], audio, protocol));
  assert.doesNotThrow(() => C.validateLabels([label, {...label, id: 2, startSample: label.endSample, endSample: label.endSample + 100}], audio, protocol));
});
test("draft exact round-trip and wrong audio/version rejection", () => {
  const draft = C.makeDraft(audio, protocol, [label]);
  assert.deepEqual(C.loadDraft(JSON.parse(JSON.stringify(draft)), audio), {labels: [label], protocol});
  assert.throws(() => C.loadDraft(draft, {...audio, sha256: "a".repeat(64)}));
  assert.throws(() => C.loadDraft({...draft, schemaVersion: 99}, audio));
  assert.throws(() => C.loadDraft({...draft, labels: [{...label, endSample: Infinity}]}, audio));
});
test("undo/redo stores snapshots and clears redo on a new edit", () => {
  const h = new C.History(2), a = {labels: []}, b = {labels: [label]};
  h.push(a); assert.deepEqual(h.undo(b), a); assert.deepEqual(h.redo(a), b);
  h.undo(b); h.push(a); assert.equal(h.redo(a), null);
  h.push(a); h.push(a); assert.equal(h.undoStack.length, 2);
});
test("production page has no remote resources, network clients or prediction inputs", () => {
  const root = path.resolve(__dirname, "..");
  const html = fs.readFileSync(path.join(root, "index.html"), "utf8");
  assert.match(html, /connect-src 'none'/);
  assert.doesNotMatch(html, /(?:src|href)=["']https?:/);
  const app = fs.readFileSync(path.join(root, "app.js"), "utf8");
  assert.doesNotMatch(app, /\b(?:fetch|XMLHttpRequest|WebSocket|sendBeacon)\s*\(/);
  assert.doesNotMatch(app, /anomaly-events\.csv/);
});
