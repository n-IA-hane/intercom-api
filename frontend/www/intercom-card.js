/**
 * Intercom Card - Lovelace custom card for intercom_native integration
 */

const INTERCOM_CARD_VERSION = "1.0.1";

class IntercomCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._active = false;
    this._stopping = false;
    this._starting = false;
    this._audioContext = null;
    this._mediaStream = null;
    this._workletNode = null;
    this._source = null;
    this._playbackContext = null;
    this._gainNode = null;
    this._nextPlayTime = 0;
    this._chunksDropped = 0;
    this._unsubscribe = null;
    this._chunksSent = 0;
    this._chunksReceived = 0;
  }

  setConfig(config) {
    if (!config.device_id) throw new Error("device_id required");
    if (!config.host) throw new Error("host required");
    this.config = config;
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
  }

  _render() {
    const name = this.config.name || "Intercom";
    const statusText = this._starting ? "Starting..." :
                       this._stopping ? "Stopping..." :
                       this._active ? "Streaming" : "Ready";
    const statusClass = this._starting || this._stopping ? "transitioning" :
                        this._active ? "connected" : "disconnected";

    this.shadowRoot.innerHTML = `
      <style>
        :host { display: block; }
        .card {
          background: var(--ha-card-background, var(--card-background-color, white));
          border-radius: var(--ha-card-border-radius, 12px);
          box-shadow: var(--ha-card-box-shadow, 0 2px 6px rgba(0,0,0,0.1));
          padding: 16px;
        }
        .header { font-size: 1.2em; font-weight: 500; margin-bottom: 16px; color: var(--primary-text-color); }
        .button-container { display: flex; justify-content: center; margin-bottom: 16px; }
        .intercom-button {
          width: 100px; height: 100px; border-radius: 50%; border: none; cursor: pointer;
          font-size: 1em; font-weight: bold; transition: all 0.2s ease;
          display: flex; align-items: center; justify-content: center;
        }
        .intercom-button.off { background: var(--primary-color, #03a9f4); color: white; }
        .intercom-button.on { background: #f44336; color: white; animation: pulse 1.5s infinite; }
        .intercom-button:disabled { opacity: 0.5; cursor: not-allowed; animation: none; }
        @keyframes pulse {
          0% { box-shadow: 0 0 0 0 rgba(244, 67, 54, 0.4); }
          70% { box-shadow: 0 0 0 15px rgba(244, 67, 54, 0); }
          100% { box-shadow: 0 0 0 0 rgba(244, 67, 54, 0); }
        }
        .status { text-align: center; color: var(--secondary-text-color); font-size: 0.9em; }
        .status-indicator { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }
        .status-indicator.connected { background: #4caf50; }
        .status-indicator.disconnected { background: #9e9e9e; }
        .status-indicator.transitioning { background: #ff9800; animation: blink 0.5s infinite; }
        @keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
        .stats { font-size: 0.75em; color: #666; margin-top: 8px; text-align: center; }
        .error { color: #f44336; font-size: 0.85em; text-align: center; margin-top: 8px; }
      </style>
      <div class="card">
        <div class="header">${name}</div>
        <div class="button-container">
          <button class="intercom-button ${this._active ? "on" : "off"}" id="btn"
                  ${this._starting || this._stopping ? "disabled" : ""}>
            ${this._stopping ? "..." : this._active ? "STOP" : "START"}
          </button>
        </div>
        <div class="status">
          <span class="status-indicator ${statusClass}"></span>
          ${statusText}
        </div>
        <div class="stats" id="stats">Sent: 0 | Recv: 0</div>
        <div class="error" id="err"></div>
      </div>
    `;
    this.shadowRoot.getElementById("btn").onclick = () => this._toggle();
  }

  _updateStats() {
    const el = this.shadowRoot?.getElementById("stats");
    if (el) el.textContent = `Sent: ${this._chunksSent} | Recv: ${this._chunksReceived}`;
  }

  _showError(msg) {
    const el = this.shadowRoot?.getElementById("err");
    if (el) el.textContent = msg;
  }

  async _toggle() {
    if (this._starting || this._stopping) return;
    this._active ? await this._stop() : await this._start();
  }

  async _start() {
    this._starting = true;
    this._render();
    this._showError("");
    this._chunksSent = 0;
    this._chunksReceived = 0;

    try {
      this._mediaStream = await navigator.mediaDevices.getUserMedia({
        audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true }
      });
      this._audioContext = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: 16000 });
      if (this._audioContext.state === "suspended") await this._audioContext.resume();

      await this._audioContext.audioWorklet.addModule(`/local/intercom-processor.js?v=${INTERCOM_CARD_VERSION}`);
      this._source = this._audioContext.createMediaStreamSource(this._mediaStream);
      this._workletNode = new AudioWorkletNode(this._audioContext, "intercom-processor");
      this._workletNode.port.onmessage = (e) => {
        if (e.data.type === "audio") this._sendAudio(new Int16Array(e.data.buffer));
      };
      this._source.connect(this._workletNode);

      this._playbackContext = new (window.AudioContext || window.webkitAudioContext)();
      this._gainNode = this._playbackContext.createGain();
      this._gainNode.gain.value = 1.0;
      this._gainNode.connect(this._playbackContext.destination);

      const result = await this._hass.connection.sendMessagePromise({
        type: "intercom_native/start",
        device_id: this.config.device_id,
        host: this.config.host,
      });
      if (!result.success) throw new Error("Start failed");

      this._unsubscribe = await this._hass.connection.subscribeEvents(
        (e) => this._handleAudioEvent(e), "intercom_audio"
      );

      this._active = true;
      this._starting = false;
      this._render();
    } catch (err) {
      this._showError(err.message || String(err));
      await this._cleanup();
      this._starting = false;
      this._render();
    }
  }

  async _stop() {
    this._stopping = true;
    this._render();
    try {
      await this._hass.connection.sendMessagePromise({
        type: "intercom_native/stop",
        device_id: this.config.device_id,
      });
    } catch (err) {}
    await this._cleanup();
    this._active = false;
    this._stopping = false;
    this._render();
  }

  async _cleanup() {
    if (this._unsubscribe) { this._unsubscribe(); this._unsubscribe = null; }
    if (this._mediaStream) { this._mediaStream.getTracks().forEach(t => t.stop()); this._mediaStream = null; }
    if (this._workletNode) { this._workletNode.disconnect(); this._workletNode = null; }
    if (this._source) { this._source.disconnect(); this._source = null; }
    if (this._audioContext) { await this._audioContext.close().catch(() => {}); this._audioContext = null; }
    if (this._playbackContext) { await this._playbackContext.close().catch(() => {}); this._playbackContext = null; }
    this._gainNode = null;
    this._nextPlayTime = 0;
    this._chunksDropped = 0;
  }

  _sendAudio(int16Array) {
    if (!this._active) return;
    const bytes = new Uint8Array(int16Array.buffer);
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + 0x8000, bytes.length)));
    }
    this._hass.connection.sendMessage({
      type: "intercom_native/audio",
      device_id: this.config.device_id,
      audio: btoa(binary),
    });
    this._chunksSent++;
    if (this._chunksSent % 25 === 0) this._updateStats();
  }

  _handleAudioEvent(event) {
    if (!event.data || event.data.device_id !== this.config.device_id) return;
    if (!this._active || !this._playbackContext) return;

    this._chunksReceived++;
    if (this._chunksReceived % 50 === 0) this._updateStats();

    try {
      const binary = atob(event.data.audio);
      const bytes = new Uint8Array(binary.length);
      for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);

      const int16 = new Int16Array(bytes.buffer);
      const float32 = new Float32Array(int16.length);
      for (let i = 0; i < int16.length; i++) float32[i] = int16[i] / 32768.0;

      this._playScheduled(float32);
    } catch (err) {}
  }

  _playScheduled(float32) {
    if (!this._playbackContext || !this._gainNode) return;
    try {
      const buffer = this._playbackContext.createBuffer(1, float32.length, 16000);
      buffer.getChannelData(0).set(float32);
      const now = this._playbackContext.currentTime;
      if (this._nextPlayTime < now) this._nextPlayTime = now + 0.01;
      if (this._nextPlayTime - now > 0.2) { this._chunksDropped++; this._nextPlayTime = now + 0.02; return; }
      const src = this._playbackContext.createBufferSource();
      src.buffer = buffer;
      src.connect(this._gainNode);
      src.start(this._nextPlayTime);
      this._nextPlayTime += buffer.duration;
    } catch (err) {}
  }

  getCardSize() { return 3; }
  static getStubConfig() { return { device_id: "", host: "", name: "Intercom" }; }
}

customElements.define("intercom-card", IntercomCard);
window.customCards = window.customCards || [];
window.customCards.push({ type: "intercom-card", name: "Intercom Card", description: "Bidirectional audio intercom with ESP32" });
