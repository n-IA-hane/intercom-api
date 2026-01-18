/**
 * Intercom Card v1.0.0 - Lovelace custom card for intercom_native integration
 *
 * P2P Mode: Single ESP device streaming to Home Assistant browser
 * - Card editor: select which intercom device this card represents
 * - Runtime: Call/Hangup button for bidirectional audio
 */

const INTERCOM_CARD_VERSION = "1.0.0";

class IntercomCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });
    this._active = false;
    this._ringing = false;  // ESP is ringing, waiting for local answer
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
    this._unsubscribeAudio = null;
    this._unsubscribeState = null;
    this._chunksSent = 0;
    this._chunksReceived = 0;
    this._activeDeviceInfo = null;  // Device info during active call
  }

  setConfig(config) {
    if (!config.entity_id && !config.device_id) {
      // Allow unconfigured card in editor
      this.config = config;
      this._render();
      return;
    }
    this.config = config;
    this._render();
  }

  _getConfigDeviceId() {
    return this.config?.entity_id || this.config?.device_id;
  }

  set hass(hass) {
    this._hass = hass;
  }

  _render() {
    const name = this.config?.name || "Intercom";
    const deviceId = this._getConfigDeviceId();

    if (!deviceId) {
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
          .unconfigured {
            text-align: center;
            color: var(--secondary-text-color);
            padding: 20px;
            font-style: italic;
          }
          .version { font-size: 0.65em; color: #999; text-align: right; margin-top: 8px; }
        </style>
        <div class="card">
          <div class="header">${name}</div>
          <div class="unconfigured">
            Please configure the card to select an intercom device.
          </div>
          <div class="version">v${INTERCOM_CARD_VERSION}</div>
        </div>
      `;
      return;
    }

    const statusText = this._starting ? "Connecting..." :
                       this._stopping ? "Ending call..." :
                       this._ringing ? "Ringing..." :
                       this._active ? "In Call" : "Ready";
    const statusClass = this._starting || this._stopping ? "transitioning" :
                        this._ringing ? "ringing" :
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
        .intercom-button.call { background: #4caf50; color: white; }
        .intercom-button.hangup { background: #f44336; color: white; animation: pulse 1.5s infinite; }
        .intercom-button.ringing { background: #ff9800; color: white; animation: ring-pulse 1s infinite; }
        .intercom-button:disabled { opacity: 0.5; cursor: not-allowed; animation: none; }
        @keyframes pulse {
          0% { box-shadow: 0 0 0 0 rgba(244, 67, 54, 0.4); }
          70% { box-shadow: 0 0 0 15px rgba(244, 67, 54, 0); }
          100% { box-shadow: 0 0 0 0 rgba(244, 67, 54, 0); }
        }
        @keyframes ring-pulse {
          0%, 100% { transform: scale(1); }
          50% { transform: scale(1.05); }
        }
        .status { text-align: center; color: var(--secondary-text-color); font-size: 0.9em; }
        .status-indicator { display: inline-block; width: 10px; height: 10px; border-radius: 50%; margin-right: 6px; }
        .status-indicator.connected { background: #4caf50; }
        .status-indicator.disconnected { background: #9e9e9e; }
        .status-indicator.transitioning { background: #ff9800; animation: blink 0.5s infinite; }
        .status-indicator.ringing { background: #ff9800; animation: blink 0.5s infinite; }
        @keyframes blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }
        .stats { font-size: 0.75em; color: #666; margin-top: 8px; text-align: center; }
        .error { color: #f44336; font-size: 0.85em; text-align: center; margin-top: 8px; }
        .version { font-size: 0.65em; color: #999; text-align: right; margin-top: 8px; }
      </style>
      <div class="card">
        <div class="header">${name}</div>
        <div class="button-container">
          <button class="intercom-button ${this._active ? "hangup" : this._ringing ? "ringing" : "call"}" id="btn"
                  ${this._starting || this._stopping ? "disabled" : ""}>
            ${this._stopping ? "..." : this._active || this._ringing ? "Hangup" : "Call"}
          </button>
        </div>
        <div class="status">
          <span class="status-indicator ${statusClass}"></span>
          ${statusText}
        </div>
        <div class="stats" id="stats">Sent: 0 | Recv: 0</div>
        <div class="error" id="err"></div>
        <div class="version">v${INTERCOM_CARD_VERSION}</div>
      </div>
    `;

    const btn = this.shadowRoot.getElementById("btn");
    if (btn) btn.onclick = () => this._toggle();
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
    (this._active || this._ringing) ? await this._hangup() : await this._call();
  }

  async _call() {
    // Get device info for the configured entity
    const deviceInfo = await this._getDeviceInfo();
    if (!deviceInfo || !deviceInfo.host) {
      this._showError("Device IP not available");
      return;
    }
    this._activeDeviceInfo = deviceInfo;

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
        device_id: deviceInfo.device_id,
        host: deviceInfo.host,
      });
      if (!result.success) throw new Error("Start failed");

      // Subscribe to audio events
      this._unsubscribeAudio = await this._hass.connection.subscribeEvents(
        (e) => this._handleAudioEvent(e), "intercom_audio"
      );

      // Subscribe to state events (ringing, streaming, disconnected)
      this._unsubscribeState = await this._hass.connection.subscribeEvents(
        (e) => this._handleStateEvent(e), "intercom_state"
      );

      // Handle initial state based on response
      if (result.state === "ringing") {
        // ESP has auto_answer OFF, waiting for local answer
        this._ringing = true;
        this._active = false;
      } else {
        // Streaming started immediately
        this._active = true;
        this._ringing = false;
      }

      this._starting = false;
      this._render();
    } catch (err) {
      this._showError(err.message || String(err));
      await this._cleanup();
      this._starting = false;
      this._render();
    }
  }

  async _getDeviceInfo() {
    try {
      const result = await this._hass.connection.sendMessagePromise({
        type: "intercom_native/list_devices",
      });
      if (result && result.devices) {
        const configId = this.config.entity_id || this.config.device_id;
        return result.devices.find(d =>
          d.device_id === configId ||
          d.esphome_id === configId ||
          d.name === configId ||
          d.name?.toLowerCase().replace(/\s+/g, '-') === configId
        );
      }
    } catch (err) {
      console.error("Failed to get device info:", err);
    }
    return null;
  }

  async _hangup() {
    this._stopping = true;
    this._render();
    try {
      if (this._activeDeviceInfo) {
        await this._hass.connection.sendMessagePromise({
          type: "intercom_native/stop",
          device_id: this._activeDeviceInfo.device_id,
        });
      }
    } catch (err) {}
    await this._cleanup();
    this._active = false;
    this._ringing = false;
    this._stopping = false;
    this._activeDeviceInfo = null;
    this._render();
  }

  async _cleanup() {
    if (this._unsubscribeAudio) { this._unsubscribeAudio(); this._unsubscribeAudio = null; }
    if (this._unsubscribeState) { this._unsubscribeState(); this._unsubscribeState = null; }
    if (this._mediaStream) { this._mediaStream.getTracks().forEach(t => t.stop()); this._mediaStream = null; }
    if (this._workletNode) { this._workletNode.disconnect(); this._workletNode = null; }
    if (this._source) { this._source.disconnect(); this._source = null; }
    if (this._audioContext) { await this._audioContext.close().catch(() => {}); this._audioContext = null; }
    if (this._playbackContext) { await this._playbackContext.close().catch(() => {}); this._playbackContext = null; }
    this._gainNode = null;
    this._nextPlayTime = 0;
    this._chunksDropped = 0;
    this._ringing = false;
  }

  _sendAudio(int16Array) {
    if (!this._active || !this._activeDeviceInfo) return;
    const bytes = new Uint8Array(int16Array.buffer);
    let binary = "";
    for (let i = 0; i < bytes.length; i += 0x8000) {
      binary += String.fromCharCode.apply(null, bytes.subarray(i, Math.min(i + 0x8000, bytes.length)));
    }
    this._hass.connection.sendMessage({
      type: "intercom_native/audio",
      device_id: this._activeDeviceInfo.device_id,
      audio: btoa(binary),
    });
    this._chunksSent++;
    if (this._chunksSent % 25 === 0) this._updateStats();
  }

  _handleStateEvent(event) {
    if (!event.data || !this._activeDeviceInfo) return;
    if (event.data.device_id !== this._activeDeviceInfo.device_id) return;

    const state = event.data.state;

    if (state === "streaming") {
      this._ringing = false;
      this._active = true;
      this._render();
    } else if (state === "ringing") {
      this._ringing = true;
      this._active = false;
      this._render();
    } else if (state === "disconnected") {
      this._hangup();
    }
  }

  _handleAudioEvent(event) {
    if (!event.data || !this._activeDeviceInfo) return;
    if (event.data.device_id !== this._activeDeviceInfo.device_id) return;
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

  static getConfigElement() {
    return document.createElement("intercom-card-editor");
  }

  static getStubConfig() {
    return { name: "Intercom" };
  }
}

// Card editor for visual config
class IntercomCardEditor extends HTMLElement {
  constructor() {
    super();
    this._config = {};
    this._hass = null;
    this._devices = [];
    this._devicesLoaded = false;
  }

  setConfig(config) {
    this._config = config;
    this._render();
  }

  set hass(hass) {
    this._hass = hass;
    if (hass && !this._devicesLoaded) {
      this._loadDevices();
    }
  }

  async _loadDevices() {
    if (!this._hass || this._devicesLoaded) return;

    try {
      const result = await this._hass.connection.sendMessagePromise({
        type: "intercom_native/list_devices",
      });

      if (result && result.devices) {
        this._devices = result.devices;
        this._devicesLoaded = true;
        this._render();
      }
    } catch (err) {
      console.error("Failed to load intercom devices:", err);
    }
  }

  _render() {
    const deviceOptions = this._devices.map(d =>
      `<option value="${d.device_id}" ${this._config.entity_id === d.device_id ? 'selected' : ''}>
        ${d.name}
      </option>`
    ).join('');

    this.innerHTML = `
      <style>
        .form-group {
          margin-bottom: 16px;
        }
        .form-group label {
          display: block;
          margin-bottom: 4px;
          font-weight: 500;
          color: var(--primary-text-color);
        }
        .form-group input,
        .form-group select {
          width: 100%;
          padding: 8px;
          border: 1px solid var(--divider-color, #ccc);
          border-radius: 4px;
          background: var(--card-background-color, white);
          color: var(--primary-text-color);
          font-size: 1em;
          box-sizing: border-box;
        }
        .form-group input:focus,
        .form-group select:focus {
          outline: none;
          border-color: var(--primary-color, #03a9f4);
        }
        .info {
          color: var(--secondary-text-color);
          font-size: 0.85em;
          margin-top: 8px;
        }
        .mode-info {
          background: var(--secondary-background-color, #f5f5f5);
          border-radius: 8px;
          padding: 12px;
          margin-top: 16px;
        }
        .mode-info h4 {
          margin: 0 0 8px 0;
          color: var(--primary-text-color);
        }
        .mode-info p {
          margin: 0;
          color: var(--secondary-text-color);
          font-size: 0.9em;
        }
      </style>
      <div style="padding: 16px;">
        <div class="form-group">
          <label>Intercom Device</label>
          <select id="entity-select">
            <option value="">-- Select device --</option>
            ${deviceOptions}
          </select>
          <div class="info">
            ${this._devicesLoaded
              ? (this._devices.length === 0 ? 'No intercom devices found' : 'Select the intercom device for this card')
              : 'Loading devices...'}
          </div>
        </div>
        <div class="form-group">
          <label>Card Name (optional)</label>
          <input type="text" id="name-input" value="${this._config.name || ''}" placeholder="Intercom">
        </div>
        <div class="mode-info">
          <h4>P2P Mode</h4>
          <p>Direct audio streaming between this browser and the selected ESP device.</p>
          <p style="margin-top: 8px; font-style: italic; color: #888;">PTMP mode (multi-device) coming soon!</p>
        </div>
      </div>
    `;

    const entitySelect = this.querySelector('#entity-select');
    if (entitySelect) {
      entitySelect.onchange = (e) => this._valueChanged('entity_id', e.target.value);
    }

    const nameInput = this.querySelector('#name-input');
    if (nameInput) {
      nameInput.onchange = (e) => this._valueChanged('name', e.target.value);
    }
  }

  _valueChanged(key, value) {
    if (!this._config) return;
    const newConfig = { ...this._config };
    if (value) {
      newConfig[key] = value;
    } else {
      delete newConfig[key];
    }
    const event = new CustomEvent("config-changed", {
      detail: { config: newConfig },
      bubbles: true,
      composed: true,
    });
    this.dispatchEvent(event);
  }
}

customElements.define("intercom-card", IntercomCard);
customElements.define("intercom-card-editor", IntercomCardEditor);

window.customCards = window.customCards || [];
window.customCards.push({
  type: "intercom-card",
  name: "Intercom Card",
  description: "Bidirectional audio intercom with ESP32 (P2P mode)",
  preview: true,
});
