export function rms(samples) {
  let sum = 0;
  for (const sample of samples) sum += sample * sample;
  return samples.length ? Math.sqrt(sum / samples.length) : 0;
}

export function resample(samples, sourceRate, targetRate = 16000) {
  if (!Number.isFinite(sourceRate) || sourceRate <= 0) throw new Error('잘못된 샘플레이트입니다.');
  if (sourceRate === targetRate) return new Float32Array(samples);
  const count = Math.floor(samples.length * targetRate / sourceRate);
  const output = new Float32Array(count);
  const ratio = sourceRate / targetRate;
  for (let i = 0; i < count; i++) {
    const start = i * ratio, end = Math.min((i + 1) * ratio, samples.length);
    let sum = 0;
    for (let j = Math.floor(start); j < Math.ceil(end); j++) {
      sum += (samples[j] || 0) * (Math.min(j + 1, end) - Math.max(j, start));
    }
    output[i] = sum / (end - start);
  }
  return output;
}

export function wavBuffer(samples, sourceRate) {
  const pcm = resample(samples, sourceRate);
  const buffer = new ArrayBuffer(44 + pcm.length * 2);
  const view = new DataView(buffer);
  const string = (offset, value) => [...value].forEach((letter, i) => view.setUint8(offset + i, letter.charCodeAt(0)));
  string(0, 'RIFF'); view.setUint32(4, buffer.byteLength - 8, true); string(8, 'WAVE'); string(12, 'fmt ');
  view.setUint32(16, 16, true); view.setUint16(20, 1, true); view.setUint16(22, 1, true);
  view.setUint32(24, 16000, true); view.setUint32(28, 32000, true); view.setUint16(32, 2, true); view.setUint16(34, 16, true);
  string(36, 'data'); view.setUint32(40, pcm.length * 2, true);
  pcm.forEach((sample, i) => view.setInt16(44 + i * 2, Math.round(Math.max(-1, Math.min(1, sample)) * (sample < 0 ? 32768 : 32767)), true));
  return buffer;
}

export class VoiceActivity {
  constructor({ sampleRate, threshold = .012, onSpeech }) {
    this.sampleRate = sampleRate;
    this.threshold = threshold;
    this.onSpeech = onSpeech;
    this.reset();
  }
  reset() { this.chunks = []; this.preRoll = []; this.length = 0; this.voiced = 0; this.quiet = 0; this.active = false; }
  feed(samples) {
    const level = rms(samples);
    const speaking = level >= this.threshold;
    if (!this.active) {
      this.preRoll.push(samples);
      while (this.preRoll.length > Math.ceil(this.sampleRate * .25 / samples.length)) this.preRoll.shift();
      if (!speaking) return { level, active: false };
      this.active = true;
      this.chunks = this.preRoll.splice(0);
      this.length = this.chunks.reduce((sum, chunk) => sum + chunk.length, 0);
    } else { this.chunks.push(samples); this.length += samples.length; }
    if (speaking) { this.voiced += samples.length; this.quiet = 0; }
    else this.quiet += samples.length;
    if (this.quiet >= this.sampleRate * .8 || this.length >= this.sampleRate * 20) {
      const valid = this.voiced >= this.sampleRate * .3;
      const output = valid ? new Float32Array(this.length) : null;
      if (output) {
        let cursor = 0;
        for (const chunk of this.chunks) { output.set(chunk, cursor); cursor += chunk.length; }
      }
      this.reset();
      if (output) this.onSpeech(output);
    }
    return { level, active: this.active };
  }
}

export async function startMicrophone({ threshold, onSpeech, onLevel, isGated, onEnded }) {
  if (!navigator.mediaDevices?.getUserMedia || !globalThis.AudioWorkletNode) throw new Error('이 브라우저에서는 마이크를 사용할 수 없습니다. localhost 또는 HTTPS에서 최신 브라우저로 열어 주세요.');
  const stream = await navigator.mediaDevices.getUserMedia({ audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true } });
  let context;
  try {
    context = new AudioContext();
    await context.audioWorklet.addModule('/audio-capture.worklet.mjs');
    await context.resume();
    const input = context.createMediaStreamSource(stream);
    const node = new AudioWorkletNode(context, 'wallaby-capture');
    const vad = new VoiceActivity({ sampleRate: context.sampleRate, threshold, onSpeech: samples => onSpeech(wavBuffer(samples, context.sampleRate)) });
    node.port.onmessage = event => {
      if (isGated()) { vad.reset(); onLevel(0, false); return; }
      const state = vad.feed(event.data);
      onLevel(state.level, state.active);
    };
    input.connect(node); node.connect(context.destination);
    for (const track of stream.getAudioTracks()) track.addEventListener('ended', onEnded, { once: true });
    return {
      threshold(value) { vad.threshold = value; }, reset() { vad.reset(); },
      async stop() { node.port.onmessage = null; node.disconnect(); input.disconnect(); stream.getTracks().forEach(track => track.stop()); await context.close(); },
    };
  } catch (error) { stream.getTracks().forEach(track => track.stop()); if (context) await context.close(); throw error; }
}

export async function fileToWav(file) {
  if (file.size > 12 * 1024 * 1024) throw new Error('WAV 파일은 12MB 이하로 선택해 주세요.');
  const bytes = await file.arrayBuffer();
  const view = new DataView(bytes);
  if (bytes.byteLength < 44 || view.getUint32(0) !== 0x52494646 || view.getUint32(8) !== 0x57415645) throw new Error('RIFF/WAVE 형식의 WAV 파일을 선택해 주세요.');
  const context = new AudioContext();
  try {
    const audio = await context.decodeAudioData(bytes);
    if (audio.duration > 30 || audio.duration < .1) throw new Error('0.1초 이상, 30초 이하의 WAV 파일을 선택해 주세요.');
    const samples = new Float32Array(audio.length);
    for (let channel = 0; channel < audio.numberOfChannels; channel++) {
      const data = audio.getChannelData(channel);
      for (let i = 0; i < samples.length; i++) samples[i] += data[i] / audio.numberOfChannels;
    }
    if (rms(samples) < .0001) throw new Error('무음 파일입니다. 음성이 포함된 파일을 선택해 주세요.');
    return wavBuffer(samples, audio.sampleRate);
  } finally { await context.close(); }
}
