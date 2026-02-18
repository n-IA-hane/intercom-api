/**
 * Intercom Card v2.0.0 - Pure mirror of ESP state
 *
 * The card is a simple frontend that mirrors the ESP's intercom_state entity.
 * No complex internal state tracking - just read ESP state and render UI.
 *
 * ESP States -> Card UI:
 * - Idle       -> Show destination + Call button
 * - Calling    -> Show "Calling [dest]..." + Hangup
 * - Ringing    -> Show "Incoming [caller]" + Answer/Decline
 * - Streaming  -> Show "In Call [peer]" + Hangup
 */

const INTERCOM_CARD_VERSION = "2.0.0";

class IntercomCard extends HTMLElement {
  constructor() {
    super();
    this.attachShadow({ mode: "open" });

    // UI transition states only
    this._starting = false;
    this._stopping = false;

    // Audio (simple mode only)
    this._audioContext = null;
    this._mediaStream = null;
    this._workletNode = null;
    this._source = null;
    this._playbackContext = null;
    this._gainNode = null;
    this._nextPlayTime = 0;
    this._unsubscribeAudio = null;
    this._unsubscribeState = null;
    this._chunksSent = 0;
    this._chunksReceived = 0;

    // Device info
    this._activeDeviceInfo = null;
    this._availableDevices = [];
    this._activeBridgeId = null;

    // Entity IDs (discovered once)
    this._intercomStateEntityId = null;
    this._callerEntityId = null;
    this._destinationEntityId = null;
    this._previousButtonEntityId = null;
    this._nextButtonEntityId = null;
    this._callButtonEntityId = null;
    this._declineButtonEntityId = null;

    // Audio streaming active (for P2P)
    this._audioStreaming = false;
  }

  setConfig(config) {
    this.config = config;
    this._render();
  }

  set hass(hass) {
    const oldHass = this._hass;
    this._hass = hass;

    // Load devices for full mode
    if (hass && this._isFullMode() && this._availableDevices.length === 0) {
      this._loadAvailableDevices();
    }

    // Discover entity IDs once
    if (hass && !this._intercomStateEntityId) {
      this._findEntityIds();
    }

    // Re-render when ESP state or destination changes
    if (hass) {
      let needsRender = false;
      let newEspState = null;
      let espStateChanged = false;

      // Check intercom_state
      if (this._intercomStateEntityId) {
        const stateEntity = hass.states[this._intercomStateEntityId];
        const oldStateEntity = oldHass?.states?.[this._intercomStateEntityId];
        newEspState = stateEntity?.state?.toLowerCase();
        if (stateEntity?.state !== oldStateEntity?.state) {
          needsRender = true;
          espStateChanged = true;
        }
      }

      // Check destination (for full mode contact cycling)
      if (this._destinationEntityId) {
        const destEntity = hass.states[this._destinationEntityId];
        const oldDestEntity = oldHass?.states?.[this._destinationEntityId];
        if (destEntity?.state !== oldDestEntity?.state) {
          needsRender = true;
        }
      }

      // Check caller (for incoming call info)
      if (this._callerEntityId) {
        const callerEntity = hass.states[this._callerEntityId];
        const oldCallerEntity = oldHass?.states?.[this._callerEntityId];
        if (callerEntity?.state !== oldCallerEntity?.state) {
          needsRender = true;
        }
      }

      // CRITICAL: Cleanup audio when ESP goes to Idle
      if (espStateChanged && newEspState === "idle" && (this._audioStreaming || this._activeBridgeId)) {
        this._cleanup();
      }

      if (needsRender) {
        this._render();
      }
    }
  }

  _isFullMode() {
    return this.config?.mode === "full";
  }

  _getConfigDeviceId() {
    return this.config?.entity_id || this.config?.device_id;
  }

  // Get current ESP state from entity
  _getEspState() {
    if (!this._hass || !this._intercomStateEntityId) return "unknown";
    const entity = this._hass.states[this._intercomStateEntityId];
    return entity?.state || "unknown";
  }

  // Get caller name from entity
  _getCallerName() {
    if (!this._hass || !this._callerEntityId) return "";
    const entity = this._hass.states[this._callerEntityId];
    const state = entity?.state;
    if (!state || state === "unknown" || state === "") return "";
    return state;
  }

  // Get destination from entity
  _getDestination() {
    if (!this._hass || !this._destinationEntityId) return "Home Assistant";
    const entity = this._hass.states[this._destinationEntityId];
    return entity?.state || "Home Assistant";
  }

  async _findEntityIds() {
    if (!this._hass) return;

    const deviceInfo = await this._getDeviceInfo();
    if (!deviceInfo?.device_id) return;

    // Use entities mapping from backend
    if (deviceInfo.entities && typeof deviceInfo.entities === "object") {
      const e = deviceInfo.entities;
      this._intercomStateEntityId = e.intercom_state || null;
      this._callerEntityId = e.incoming_caller || null;
      this._destinationEntityId = e.destination || null;
      this._previousButtonEntityId = e.previous || null;
      this._nextButtonEntityId = e.next || null;
      this._callButtonEntityId = e.call || null;
      this._declineButtonEntityId = e.decline || null;
      this._render();
      return;
    }

    // Fallback: entity registry
    try {
      const registry = await this._hass.connection.sendMessagePromise({
        type: "config/entity_registry/list",
      });
      if (!registry) return;

      for (const entity of registry) {
        if (entity.device_id !== deviceInfo.device_id) continue;
        const id = entity.entity_id;
        if (id.includes("intercom_state")) this._intercomStateEntityId = id;
        else if (id.includes("caller")) this._callerEntityId = id;
        else if (id.includes("destination")) this._destinationEntityId = id;
        else if (id.startsWith("button.") && id.includes("previous")) this._previousButtonEntityId = id;
        else if (id.startsWith("button.") && id.includes("next")) this._nextButtonEntityId = id;
        else if (id.startsWith("button.") && id.includes("call") && !id.includes("decline")) this._callButtonEntityId = id;
        else if (id.startsWith("button.") && id.includes("decline")) this._declineButtonEntityId = id;
      }
      this._render();
    } catch (err) {
      console.error("Entity discovery failed:", err);
    }
  }

  async _loadAvailableDevices() {
    if (!this._hass) return;
    try {
      const result = await this._hass.connection.sendMessagePromise({
        type: "intercom_native/list_devices",
      });
      if (result?.devices) {
        this._availableDevices = result.devices;
        this._render();
      }
    } catch (err) {
      console.error("Failed to load devices:", err);
    }
  }

  _render() {
    const name = this.config?.name || "Intercom";
    const deviceId = this._getConfigDeviceId();

    if (!deviceId) {
      this._renderUnconfigured(name);
      return;
    }

    const espState = this._getEspState();
    const isFullMode = this._isFullMode();
    const destination = this._getDestination();
    const caller = this._getCallerName();

    // Determine what to show based on ESP state
    let statusText = "";
    let statusClass = "disconnected";
    let showAnswer = false;
    let showHangup = false;
    let showCall = false;
    let buttonDisabled = this._starting || this._stopping;

    // Get ESP device name for incoming call display
    // Try activeDeviceInfo first, then search in availableDevices, fallback to config name
    let espDeviceName = this._activeDeviceInfo?.name;
    if (!espDeviceName && deviceId) {
      const device = this._availableDevices.find(d =>
        d.device_id === deviceId || d.esphome_id === deviceId ||
        d.name === deviceId || d.name?.toLowerCase().replace(/\s+/g, '-') === deviceId
      );
      espDeviceName = device?.name;
    }
    espDeviceName = espDeviceName || name;

    switch (espState.toLowerCase()) {
      case "idle":
        statusText = "Ready";
        statusClass = "disconnected";
        showCall = true;
        break;
      case "calling":
      case "outgoing":
        // Special case: ESP calling "Home Assistant" = incoming call TO the card
        if (destination === "Home Assistant") {
          statusText = `Incoming: ${espDeviceName}`;
          statusClass = "ringing";
          showAnswer = true;
        } else {
          statusText = `Calling ${destination}...`;
          statusClass = "transitioning";
          showHangup = true;
        }
        break;
      case "ringing":
      case "incoming":
        statusText = `Incoming: ${caller || "Unknown"}`;
        statusClass = "ringing";
        showAnswer = true;
        break;
      case "streaming":
      case "answering":
        statusText = `In Call: ${caller || destination || "Active"}`;
        statusClass = "connected";
        showHangup = true;
        break;
      default:
        statusText = espState;
        statusClass = "disconnected";
        showCall = true;
    }

    if (this._starting) statusText = "Connecting...";
    if (this._stopping) statusText = "Ending call...";

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
        .mode-badge {
          display: inline-block; font-size: 0.7em; padding: 2px 6px;
          border-radius: 4px; margin-left: 8px; vertical-align: middle;
        }
        .mode-badge.simple { background: #4caf50; color: white; }
        .mode-badge.full { background: #2196f3; color: white; }

        .destination-row {
          display: flex; align-items: center; justify-content: center;
          gap: 12px; margin-bottom: 16px;
        }
        .nav-btn {
          width: 36px; height: 36px; border-radius: 50%;
          border: 1px solid var(--divider-color, #ccc);
          background: var(--card-background-color, white);
          color: var(--primary-text-color); cursor: pointer;
          font-size: 1.2em; display: flex; align-items: center; justify-content: center;
        }
        .nav-btn:hover { background: var(--secondary-background-color, #f5f5f5); }
        .nav-btn:disabled { opacity: 0.5; cursor: not-allowed; }
        .destination-value {
          flex: 1; text-align: center; font-size: 1.1em; font-weight: 500;
          color: var(--primary-text-color); padding: 8px 0;
        }
        .destination-label {
          font-size: 0.75em; color: var(--secondary-text-color);
          display: block; margin-bottom: 2px;
        }

        .button-container { display: flex; justify-content: center; gap: 20px; margin-bottom: 16px; }
        .intercom-button {
          width: 100px; height: 100px; border-radius: 50%; border: none; cursor: pointer;
          font-size: 1em; font-weight: bold; transition: all 0.2s ease;
          display: flex; align-items: center; justify-content: center;
        }
        .intercom-button.small { width: 80px; height: 80px; font-size: 0.9em; }
        .intercom-button.call { background: #4caf50; color: white; }
        .intercom-button.answer { background: #4caf50; color: white; animation: ring-pulse 1s infinite; }
        .intercom-button.decline { background: #f44336; color: white; animation: ring-pulse 1s infinite; }
        .intercom-button.hangup { background: #f44336; color: white; }
        .intercom-button:disabled { opacity: 0.5; cursor: not-allowed; animation: none; }
        @keyframes ring-pulse { 0%, 100% { transform: scale(1); } 50% { transform: scale(1.05); } }

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
        <div class="header">
          ${name}
          <span class="mode-badge ${isFullMode ? 'full' : 'simple'}">${isFullMode ? 'Full' : 'Simple'}</span>
        </div>

        ${isFullMode && showCall ? `
        <div class="destination-row">
          <button class="nav-btn" id="prev-btn" ${buttonDisabled ? 'disabled' : ''} title="Previous">&lt;</button>
          <div class="destination-value">
            <span class="destination-label">Destination</span>
            ${destination}
          </div>
          <button class="nav-btn" id="next-btn" ${buttonDisabled ? 'disabled' : ''} title="Next">&gt;</button>
        </div>
        ` : ''}

        <div class="button-container">
          ${showAnswer ? `
            <button class="intercom-button small answer" id="answer-btn" ${buttonDisabled ? 'disabled' : ''}>Answer</button>
            <button class="intercom-button small decline" id="decline-btn" ${buttonDisabled ? 'disabled' : ''}>Decline</button>
          ` : showHangup ? `
            <button class="intercom-button hangup" id="hangup-btn" ${buttonDisabled ? 'disabled' : ''}>Hangup</button>
          ` : showCall ? `
            <button class="intercom-button call" id="call-btn" ${buttonDisabled ? 'disabled' : ''}>Call</button>
          ` : `
            <button class="intercom-button" disabled>...</button>
          `}
        </div>

        <div class="status">
          <span class="status-indicator ${statusClass}"></span>
          ${statusText}
        </div>
        <div class="stats" id="stats">${isFullMode ? (destination === 'Home Assistant' ? 'Browser ↔ ESP' : 'ESP ↔ ESP') : 'Sent: 0 | Recv: 0'}</div>
        <div class="error" id="err"></div>
        <div class="version">v${INTERCOM_CARD_VERSION}</div>
      </div>
    `;

    this._attachEventHandlers();
  }

  _renderUnconfigured(name) {
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
        .unconfigured { text-align: center; color: var(--secondary-text-color); padding: 20px; font-style: italic; }
        .version { font-size: 0.65em; color: #999; text-align: right; margin-top: 8px; }
      </style>
      <div class="card">
        <div class="header">${name}</div>
        <div class="unconfigured">Please configure the card to select an intercom device.</div>
        <div class="version">v${INTERCOM_CARD_VERSION}</div>
      </div>
    `;
  }

  _attachEventHandlers() {
    const callBtn = this.shadowRoot.getElementById("call-btn");
    const hangupBtn = this.shadowRoot.getElementById("hangup-btn");
    const answerBtn = this.shadowRoot.getElementById("answer-btn");
    const declineBtn = this.shadowRoot.getElementById("decline-btn");
    const prevBtn = this.shadowRoot.getElementById("prev-btn");
    const nextBtn = this.shadowRoot.getElementById("next-btn");

    if (callBtn) callBtn.onclick = () => this._startCall();
    if (hangupBtn) hangupBtn.onclick = () => this._hangup();
    if (answerBtn) answerBtn.onclick = () => this._answer();
    if (declineBtn) declineBtn.onclick = () => this._decline();
    if (prevBtn) prevBtn.onclick = () => this._prevContact();
    if (nextBtn) nextBtn.onclick = () => this._nextContact();
  }

  async _prevContact() {
    if (this._previousButtonEntityId) {
      await this._hass.callService("button", "press", { entity_id: this._previousButtonEntityId });
    }
  }

  async _nextContact() {
    if (this._nextButtonEntityId) {
      await this._hass.callService("button", "press", { entity_id: this._nextButtonEntityId });
    }
  }

  async _startCall() {
    const deviceInfo = await this._getDeviceInfo();
    if (!deviceInfo?.host) {
      this._showError("Device not available");
      return;
    }

    this._activeDeviceInfo = deviceInfo;
    this._starting = true;
    this._render();
    this._showError("");

    try {
      const destination = this._getDestination();

      if (this._isFullMode() && destination !== "Home Assistant") {
        // Full mode: Bridge to another ESP
        await this._startBridge(deviceInfo, destination);
      } else {
        // P2P: Direct call with browser audio
        await this._startP2P(deviceInfo);
      }
    } catch (err) {
      this._showError(err.message || String(err));
      await this._cleanup();
    } finally {
      this._starting = false;
      this._render();
    }
  }

  async _startP2P(deviceInfo) {
    // Setup mic
    this._mediaStream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true }
    });

    const track = this._mediaStream.getAudioTracks()[0];
    const trackSampleRate = track?.getSettings?.().sampleRate;
    this._audioContext = new (window.AudioContext || window.webkitAudioContext)(
      trackSampleRate ? { sampleRate: trackSampleRate } : undefined
    );
    if (this._audioContext.state === "suspended") await this._audioContext.resume();

    await this._audioContext.audioWorklet.addModule(`/local/intercom-processor.js?v=${INTERCOM_CARD_VERSION}`);
    this._source = this._audioContext.createMediaStreamSource(this._mediaStream);
    this._workletNode = new AudioWorkletNode(this._audioContext, "intercom-processor");
    this._workletNode.port.onmessage = (e) => {
      if (e.data.type === "audio") this._sendAudio(new Int16Array(e.data.buffer));
    };
    this._source.connect(this._workletNode);

    // Setup speaker
    this._playbackContext = new (window.AudioContext || window.webkitAudioContext)();
    this._gainNode = this._playbackContext.createGain();
    this._gainNode.gain.value = 1.0;
    this._gainNode.connect(this._playbackContext.destination);

    // Start session
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

    this._audioStreaming = true;
    this._chunksSent = 0;
    this._chunksReceived = 0;
  }

  async _answerEspCall(deviceInfo) {
    // Answer an ESP-initiated call (ESP called Home Assistant)
    // Similar to _startP2P but sends ANSWER instead of START

    // Setup mic
    this._mediaStream = await navigator.mediaDevices.getUserMedia({
      audio: { echoCancellation: true, noiseSuppression: true, autoGainControl: true }
    });

    const track = this._mediaStream.getAudioTracks()[0];
    const trackSampleRate = track?.getSettings?.().sampleRate;
    this._audioContext = new (window.AudioContext || window.webkitAudioContext)(
      trackSampleRate ? { sampleRate: trackSampleRate } : undefined
    );
    if (this._audioContext.state === "suspended") await this._audioContext.resume();

    await this._audioContext.audioWorklet.addModule(`/local/intercom-processor.js?v=${INTERCOM_CARD_VERSION}`);
    this._source = this._audioContext.createMediaStreamSource(this._mediaStream);
    this._workletNode = new AudioWorkletNode(this._audioContext, "intercom-processor");
    this._workletNode.port.onmessage = (e) => {
      if (e.data.type === "audio") this._sendAudio(new Int16Array(e.data.buffer));
    };
    this._source.connect(this._workletNode);

    // Setup speaker
    this._playbackContext = new (window.AudioContext || window.webkitAudioContext)();
    this._gainNode = this._playbackContext.createGain();
    this._gainNode.gain.value = 1.0;
    this._gainNode.connect(this._playbackContext.destination);

    // Answer ESP call (sends ANSWER, not START)
    const result = await this._hass.connection.sendMessagePromise({
      type: "intercom_native/answer_esp_call",
      device_id: deviceInfo.device_id,
      host: deviceInfo.host,
    });
    if (!result.success) throw new Error("Answer failed");

    // Subscribe to audio events
    this._unsubscribeAudio = await this._hass.connection.subscribeEvents(
      (e) => this._handleAudioEvent(e), "intercom_audio"
    );

    this._audioStreaming = true;
    this._chunksSent = 0;
    this._chunksReceived = 0;
  }

  async _startBridge(sourceDevice, destinationName) {
    const destDevice = this._availableDevices.find(d => d.name === destinationName);
    if (!destDevice?.host) {
      throw new Error(`Destination "${destinationName}" not available`);
    }

    const result = await this._hass.connection.sendMessagePromise({
      type: "intercom_native/bridge",
      source_device_id: sourceDevice.device_id,
      source_host: sourceDevice.host,
      source_name: sourceDevice.name || "Intercom",
      dest_device_id: destDevice.device_id,
      dest_host: destDevice.host,
      dest_name: destDevice.name || "Intercom",
    });

    if (!result.success) throw new Error(result.error || "Bridge failed");
    this._activeBridgeId = result.bridge_id;
  }

  async _answer() {
    const deviceInfo = await this._getDeviceInfo();
    if (!deviceInfo?.device_id) {
      this._showError("Device not found");
      return;
    }

    this._starting = true;
    this._activeDeviceInfo = deviceInfo;
    this._render();

    try {
      const espState = this._getEspState().toLowerCase();
      const destination = this._getDestination();

      // Check if ESP is calling HA (outgoing + destination = Home Assistant)
      if ((espState === "outgoing" || espState === "calling") && destination === "Home Assistant") {
        // ESP is calling us - answer with proper ANSWER message (not START)
        await this._answerEspCall(deviceInfo);
        this._showError("");
      } else {
        // Normal case: ESP is ringing (we called it), send answer command
        const res = await this._hass.connection.sendMessagePromise({
          type: "intercom_native/answer",
          device_id: deviceInfo.device_id,
        });

        if (!res?.success && this._callButtonEntityId) {
          // Fallback: press call button on ESP
          await this._hass.callService("button", "press", { entity_id: this._callButtonEntityId });
        }
        this._showError("");
      }
    } catch (err) {
      this._showError(err.message || String(err));
      await this._cleanup();
    } finally {
      this._starting = false;
      this._render();
    }
  }

  async _decline() {
    const deviceInfo = await this._getDeviceInfo();
    if (!deviceInfo?.device_id) {
      this._showError("Device not found");
      return;
    }

    this._stopping = true;
    this._render();

    try {
      const espState = this._getEspState().toLowerCase();
      const destination = this._getDestination();

      // Check if ESP is calling HA (outgoing + destination = Home Assistant)
      if ((espState === "outgoing" || espState === "calling") && destination === "Home Assistant") {
        // ESP is calling us - press ESP's decline button to hang up
        if (this._declineButtonEntityId) {
          await this._hass.callService("button", "press", { entity_id: this._declineButtonEntityId });
        } else if (this._callButtonEntityId) {
          // Fallback: call button acts as toggle (hangup when active)
          await this._hass.callService("button", "press", { entity_id: this._callButtonEntityId });
        }
      } else {
        // Normal decline via WS command
        await this._hass.connection.sendMessagePromise({
          type: "intercom_native/decline",
          device_id: deviceInfo.device_id,
        });
      }
      this._showError("");
    } catch (err) {
      this._showError(err.message || String(err));
    } finally {
      this._stopping = false;
      this._render();
    }
  }

  async _hangup() {
    this._stopping = true;
    this._render();

    try {
      if (this._activeBridgeId) {
        await this._hass.connection.sendMessagePromise({
          type: "intercom_native/bridge_stop",
          bridge_id: this._activeBridgeId,
        });
      } else if (this._activeDeviceInfo) {
        await this._hass.connection.sendMessagePromise({
          type: "intercom_native/stop",
          device_id: this._activeDeviceInfo.device_id,
        });
      } else {
        // No active session from card - use decline to find and stop any session
        const deviceInfo = await this._getDeviceInfo();
        if (deviceInfo?.device_id) {
          await this._hass.connection.sendMessagePromise({
            type: "intercom_native/decline",
            device_id: deviceInfo.device_id,
          });
        }
      }
    } catch (err) {
      console.error("Hangup error:", err);
    }

    await this._cleanup();
    this._stopping = false;
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
    this._activeDeviceInfo = null;
    this._activeBridgeId = null;
    this._audioStreaming = false;
  }

  async _getDeviceInfo() {
    try {
      const result = await this._hass.connection.sendMessagePromise({
        type: "intercom_native/list_devices",
      });
      if (result?.devices) {
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

  _sendAudio(int16Array) {
    if (!this._audioStreaming || !this._activeDeviceInfo) return;
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

  _handleAudioEvent(event) {
    if (!event.data || !this._activeDeviceInfo) return;
    if (event.data.device_id !== this._activeDeviceInfo.device_id) return;
    if (!this._audioStreaming || !this._playbackContext) return;

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
      if (this._nextPlayTime - now > 0.2) { this._nextPlayTime = now + 0.02; return; }
      const src = this._playbackContext.createBufferSource();
      src.buffer = buffer;
      src.connect(this._gainNode);
      src.start(this._nextPlayTime);
      this._nextPlayTime += buffer.duration;
    } catch (err) {}
  }

  _updateStats() {
    const el = this.shadowRoot?.getElementById("stats");
    // Show stats when browser audio is active (simple mode or full mode with Home Assistant)
    if (el && this._audioStreaming) {
      el.textContent = `Sent: ${this._chunksSent} | Recv: ${this._chunksReceived}`;
    }
  }

  _showError(msg) {
    const el = this.shadowRoot?.getElementById("err");
    if (el) el.textContent = msg;
  }

  getCardSize() { return 3; }

  static getConfigElement() {
    return document.createElement("intercom-card-editor");
  }

  static getStubConfig() {
    return { name: "Intercom" };
  }
}

// Card editor
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
    if (hass && !this._devicesLoaded) this._loadDevices();
  }

  async _loadDevices() {
    if (!this._hass || this._devicesLoaded) return;
    try {
      const result = await this._hass.connection.sendMessagePromise({
        type: "intercom_native/list_devices",
      });
      if (result?.devices) {
        this._devices = result.devices;
        this._devicesLoaded = true;
        this._render();
      }
    } catch (err) {
      console.error("Failed to load devices:", err);
    }
  }

  _render() {
    const deviceOptions = this._devices.map(d =>
      `<option value="${d.device_id}" ${this._config.entity_id === d.device_id ? 'selected' : ''}>${d.name}</option>`
    ).join('');

    const currentMode = this._config.mode || 'simple';

    this.innerHTML = `
      <style>
        .form-group { margin-bottom: 16px; }
        .form-group label { display: block; margin-bottom: 4px; font-weight: 500; color: var(--primary-text-color); }
        .form-group input, .form-group select {
          width: 100%; padding: 8px; border: 1px solid var(--divider-color, #ccc);
          border-radius: 4px; background: var(--card-background-color, white);
          color: var(--primary-text-color); font-size: 1em; box-sizing: border-box;
        }
        .info { color: var(--secondary-text-color); font-size: 0.85em; margin-top: 8px; }
        .mode-selector { display: flex; gap: 8px; margin-top: 8px; }
        .mode-btn {
          flex: 1; padding: 12px; border: 2px solid var(--divider-color, #ccc);
          border-radius: 8px; background: var(--card-background-color, white);
          cursor: pointer; text-align: center; transition: all 0.2s;
        }
        .mode-btn:hover { border-color: var(--primary-color, #03a9f4); }
        .mode-btn.selected { border-color: var(--primary-color, #03a9f4); background: var(--primary-color, #03a9f4); color: white; }
        .mode-btn .mode-title { font-weight: bold; font-size: 1.1em; }
        .mode-btn .mode-desc { font-size: 0.8em; opacity: 0.8; margin-top: 4px; }
        .mode-info { background: var(--secondary-background-color, #f5f5f5); border-radius: 8px; padding: 12px; margin-top: 16px; }
        .mode-info h4 { margin: 0 0 8px 0; color: var(--primary-text-color); }
        .mode-info p { margin: 0; color: var(--secondary-text-color); font-size: 0.9em; }
      </style>
      <div style="padding: 16px;">
        <div class="form-group">
          <label>Intercom Device</label>
          <select id="entity-select">
            <option value="">-- Select device --</option>
            ${deviceOptions}
          </select>
          <div class="info">${this._devicesLoaded ? (this._devices.length === 0 ? 'No devices found' : 'Select device') : 'Loading...'}</div>
        </div>
        <div class="form-group">
          <label>Card Name (optional)</label>
          <input type="text" id="name-input" value="${this._config.name || ''}" placeholder="Intercom">
        </div>
        <div class="form-group">
          <label>Mode</label>
          <div class="mode-selector">
            <div class="mode-btn ${currentMode === 'simple' ? 'selected' : ''}" id="mode-simple">
              <div class="mode-title">Simple</div>
              <div class="mode-desc">Browser ↔ ESP</div>
            </div>
            <div class="mode-btn ${currentMode === 'full' ? 'selected' : ''}" id="mode-full">
              <div class="mode-title">Full</div>
              <div class="mode-desc">ESP ↔ ESP</div>
            </div>
          </div>
        </div>
        <div class="mode-info">
          ${currentMode === 'simple' ? `
            <h4>Simple Mode</h4>
            <p>Browser audio ↔ ESP device</p>
          ` : `
            <h4>Full Mode</h4>
            <p>ESP ↔ ESP bridged through Home Assistant</p>
          `}
        </div>
      </div>
    `;

    this.querySelector('#entity-select').onchange = (e) => this._valueChanged('entity_id', e.target.value);
    this.querySelector('#name-input').onchange = (e) => this._valueChanged('name', e.target.value);
    this.querySelector('#mode-simple').onclick = () => this._valueChanged('mode', 'simple');
    this.querySelector('#mode-full').onclick = () => this._valueChanged('mode', 'full');
  }

  _valueChanged(key, value) {
    const newConfig = { ...this._config };
    if (value) newConfig[key] = value;
    else delete newConfig[key];
    this.dispatchEvent(new CustomEvent("config-changed", { detail: { config: newConfig }, bubbles: true, composed: true }));
  }
}

customElements.define("intercom-card", IntercomCard);
customElements.define("intercom-card-editor", IntercomCardEditor);

window.customCards = window.customCards || [];
window.customCards.push({
  type: "intercom-card",
  name: "Intercom Card",
  description: "ESP intercom control - mirrors ESP state (Simple and Full modes)",
  preview: true,
});
