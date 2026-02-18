/**
 * Intercom AudioWorklet Processor
 * Based on Home Assistant's recorder-worklet.js
 * VERSION: 2.3.0 - Proper resampling for any input sample rate (44.1kHz, 48kHz, etc)
 *
 * This processor runs in a separate audio thread and converts
 * Float32 audio samples to Int16 PCM format at 16kHz.
 */

const TARGET_SAMPLE_RATE = 16000;

class RecorderProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this._buffer = [];
    this._targetSamples = 1024; // 64ms chunks @ 16kHz = ~15 msg/sec
    this._frameCount = 0;
    this._chunksSent = 0;
    this._totalSamplesProcessed = 0;

    // Resampling state - works for any input rate (44.1kHz, 48kHz, etc)
    this._resampleRatio = sampleRate / TARGET_SAMPLE_RATE;
    this._resampleAccum = 0;

    // Log initialization
    console.log("[IntercomProcessor] === INITIALIZED v2.3.0 ===");
    console.log("[IntercomProcessor] sampleRate:", sampleRate, "-> target:", TARGET_SAMPLE_RATE);
    console.log("[IntercomProcessor] resampleRatio:", this._resampleRatio.toFixed(4));
    console.log("[IntercomProcessor] targetSamples:", this._targetSamples);

    // Send init message to main thread
    this.port.postMessage({
      type: "debug",
      message: `Worklet v2.3.0: ${sampleRate}Hz -> ${TARGET_SAMPLE_RATE}Hz (ratio: ${this._resampleRatio.toFixed(2)})`
    });
  }

  process(inputList, _outputList, _parameters) {
    this._frameCount++;

    // Check input validity
    if (!inputList || inputList.length === 0) {
      if (this._frameCount % 500 === 0) {
        console.log("[IntercomProcessor] Frame", this._frameCount, "- no inputList");
      }
      return true;
    }

    if (!inputList[0] || inputList[0].length === 0) {
      if (this._frameCount % 500 === 0) {
        console.log("[IntercomProcessor] Frame", this._frameCount, "- no channels in inputList[0]");
      }
      return true;
    }

    const float32Data = inputList[0][0]; // First channel of first input
    if (!float32Data || float32Data.length === 0) {
      if (this._frameCount % 500 === 0) {
        console.log("[IntercomProcessor] Frame", this._frameCount, "- no data in channel 0");
      }
      return true;
    }

    // Log periodically
    if (this._frameCount % 200 === 1) {
      console.log("[IntercomProcessor] Frame", this._frameCount,
                  "- samples:", float32Data.length,
                  "bufferLen:", this._buffer.length,
                  "chunksSent:", this._chunksSent);
    }

    // Resample to 16kHz using fractional accumulator
    // Works correctly for any input rate (44.1kHz, 48kHz, etc)
    for (let i = 0; i < float32Data.length; i++) {
      this._resampleAccum += 1;
      if (this._resampleAccum >= this._resampleRatio) {
        this._buffer.push(float32Data[i]);
        this._resampleAccum -= this._resampleRatio;
      }
    }
    this._totalSamplesProcessed += float32Data.length;

    // When we have enough samples, convert and send
    while (this._buffer.length >= this._targetSamples) {
      const chunk = this._buffer.splice(0, this._targetSamples);

      // Convert Float32 (-1.0 to 1.0) to Int16 (-32768 to 32767)
      // Following HA's exact pattern from recorder-worklet.js
      const int16Data = new Int16Array(chunk.length);
      for (let i = 0; i < chunk.length; i++) {
        const s = Math.max(-1, Math.min(1, chunk[i]));
        int16Data[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
      }

      this._chunksSent++;

      // Log chunk send
      if (this._chunksSent <= 5 || this._chunksSent % 50 === 0) {
        console.log("[IntercomProcessor] Sending chunk", this._chunksSent,
                    "- samples:", int16Data.length,
                    "- bytes:", int16Data.buffer.byteLength);
      }

      // Send to main thread (transferable for performance)
      try {
        this.port.postMessage({
          type: "audio",
          buffer: int16Data.buffer
        }, [int16Data.buffer]);
      } catch (err) {
        console.error("[IntercomProcessor] postMessage error:", err);
      }
    }

    return true; // Keep processor alive
  }
}

registerProcessor("intercom-processor", RecorderProcessor);
console.log("[IntercomProcessor] Processor registered");
