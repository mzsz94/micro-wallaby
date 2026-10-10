class WallabyCapture extends AudioWorkletProcessor {
  constructor() { super(); this.buffer = new Float32Array(2048); this.offset = 0; }
  process(inputs) {
    const channels = inputs[0];
    if (!channels?.length) return true;
    for (let i = 0; i < channels[0].length; i++) {
      let sample = 0;
      for (const channel of channels) sample += channel[i] || 0;
      this.buffer[this.offset++] = sample / channels.length;
      if (this.offset === this.buffer.length) {
        this.port.postMessage(this.buffer, [this.buffer.buffer]);
        this.buffer = new Float32Array(2048);
        this.offset = 0;
      }
    }
    // The output remains silent: microphone audio is never played back.
    return true;
  }
}
registerProcessor('wallaby-capture', WallabyCapture);
