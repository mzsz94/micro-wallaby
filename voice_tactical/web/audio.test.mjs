import test from 'node:test';
import assert from 'node:assert/strict';
import { wavBuffer, resample, VoiceActivity } from './audio.mjs';

test('48kHz input becomes mono 16-bit 16kHz WAV with clipped PCM', () => {
  const input = new Float32Array(48000).fill(2);
  const view = new DataView(wavBuffer(input, 48000));
  assert.equal(view.byteLength, 32044);
  assert.equal(view.getUint16(22, true), 1);
  assert.equal(view.getUint32(24, true), 16000);
  assert.equal(view.getUint16(34, true), 16);
  assert.equal(view.getUint32(40, true), 32000);
  assert.equal(view.getInt16(44, true), 32767);
  assert.equal(resample(new Float32Array([1, 1, -1, -1]), 32000)[0], 1);
});
test('silence and a brief click do not submit speech', () => {
  let submissions = 0;
  const vad = new VoiceActivity({ sampleRate: 16000, onSpeech: () => submissions++ });
  for (let i = 0; i < 1000; i++) vad.feed(new Float32Array(160));
  vad.feed(new Float32Array(160).fill(.5));
  for (let i = 0; i < 100; i++) vad.feed(new Float32Array(160));
  assert.equal(submissions, 0);
});
test('utterance ends at silence and resets, continuous sound is bounded', () => {
  const clips = [];
  const vad = new VoiceActivity({ sampleRate: 16000, onSpeech: samples => clips.push(samples) });
  for (let i = 0; i < 40; i++) vad.feed(new Float32Array(160).fill(.05));
  for (let i = 0; i < 81; i++) vad.feed(new Float32Array(160));
  assert.equal(clips.length, 1);
  assert.equal(vad.active, false);
  for (let i = 0; i < 2100; i++) vad.feed(new Float32Array(160).fill(.05));
  assert.equal(clips.length, 2);
  assert.ok(clips[1].length <= 16000 * 20);
});
