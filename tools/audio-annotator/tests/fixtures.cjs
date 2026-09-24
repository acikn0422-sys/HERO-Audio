"use strict";
const crypto = require("node:crypto");
// Synthetic PCM only: no microphone recording or user label is a test fixture.
function makeWav({sampleRate = 44100, seconds = 4, bits = 16, channels = 1, code = 1, junk = false} = {}) {
  const count = Math.round(sampleRate * seconds), bytesPerSample = bits / 8;
  const fmt = Buffer.alloc(24); fmt.write("fmt "); fmt.writeUInt32LE(16, 4);
  fmt.writeUInt16LE(code, 8); fmt.writeUInt16LE(channels, 10); fmt.writeUInt32LE(sampleRate, 12);
  fmt.writeUInt32LE(sampleRate * channels * bytesPerSample, 16); fmt.writeUInt16LE(channels * bytesPerSample, 20); fmt.writeUInt16LE(bits, 22);
  const data = Buffer.alloc(8 + count * channels * bytesPerSample); data.write("data"); data.writeUInt32LE(data.length - 8, 4);
  for (let i = 0; i < count; i++) {
    const local = i / sampleRate - 2;
    const value = local >= 0 && local < 0.08 ? 0.4 * Math.sin(local * 2 * Math.PI * 800) * Math.exp(-local * 35) : 0;
    for (let c = 0; c < channels; c++) {
      const p = 8 + (i * channels + c) * bytesPerSample;
      if (code === 3 && bits === 32) data.writeFloatLE(value, p);
      else if (bits === 16) data.writeInt16LE(Math.round(value * 32767), p);
    }
  }
  const unknown = Buffer.from([74, 85, 78, 75, 1, 0, 0, 0, 7, 0]);
  const chunks = junk ? [unknown, fmt, data] : [fmt, data];
  const header = Buffer.alloc(12); header.write("RIFF"); header.writeUInt32LE(4 + chunks.reduce((n, b) => n + b.length, 0), 4); header.write("WAVE", 8);
  return Buffer.concat([header, ...chunks]);
}
function arrayBuffer(buffer) { return buffer.buffer.slice(buffer.byteOffset, buffer.byteOffset + buffer.byteLength); }
function metadata(bytes, parsed) { return {...parsed, bytes: bytes.length, name: "synthetic.wav", sha256: crypto.createHash("sha256").update(bytes).digest("hex")}; }
module.exports = {makeWav, arrayBuffer, metadata};
