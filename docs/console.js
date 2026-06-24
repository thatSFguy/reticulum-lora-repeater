// docs/console.js — Web Serial client for the Reticulum LoRa Repeater
// plain-text provisioning protocol. Talks to src/SerialConsole.cpp in
// the firmware repo. No build step, no framework, vanilla JS.
//
// Protocol recap (see src/SerialConsole.cpp for the authoritative
// reference):
//   - One command per line, LF-terminated.
//   - Firmware echoes each character as it's received, then emits zero
//     or more payload lines, then a terminator line: "OK" on success,
//     "ERR: <reason>" on failure.
//   - All CONFIG edits are staged in RAM. CONFIG COMMIT validates,
//     persists, and NVIC-reboots — after which the serial port drops
//     and needs to be reconnected.

'use strict';

// ---------------------------------------------------------------
//  RLRConsole — thin Web Serial wrapper around the line protocol
// ---------------------------------------------------------------
class RLRConsole {
  constructor(logFn) {
    this.port   = null;
    this.reader = null;
    this.writer = null;
    this.readerClosed = null;
    this.writerClosed = null;
    this.lineBuffer = '';
    this.lineQueue = [];
    this.lineResolvers = [];
    this.log = logFn || (() => {});
    this.onDisconnect = null;
    this.onUnsolicited = null;  // called for lines that arrive when no command is pending
  }

  isConnected() { return this.port !== null; }

  async connect() {
    if (!('serial' in navigator)) throw new Error('Web Serial not supported in this browser');
    this.port = await navigator.serial.requestPort();
    await this.port.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none' });

    const decoder = new TextDecoderStream();
    this.readerClosed = this.port.readable.pipeTo(decoder.writable).catch(() => {});
    this.reader = decoder.readable.getReader();

    const encoder = new TextEncoderStream();
    this.writerClosed = encoder.readable.pipeTo(this.port.writable).catch(() => {});
    this.writer = encoder.writable.getWriter();

    this._readLoop();  // fire and forget
  }

  async _readLoop() {
    try {
      while (true) {
        const { value, done } = await this.reader.read();
        if (done) break;
        if (!value) continue;
        this.lineBuffer += value;
        this._processLineBuffer();
      }
    } catch (e) {
      // The reader throws when we cancel it in disconnect() — that's
      // expected. Anything else is a real transport error.
    } finally {
      // If we get here unexpectedly (device unplug, user pulled USB),
      // tear down and notify the UI so it can flip back to the
      // "disconnected" state.
      if (this.port) {
        const cb = this.onDisconnect;
        this._teardown().then(() => { if (cb) cb(); });
      }
    }
  }

  _processLineBuffer() {
    let idx;
    while ((idx = this.lineBuffer.search(/[\r\n]/)) >= 0) {
      const line = this.lineBuffer.slice(0, idx);
      const sep  = this.lineBuffer[idx];
      const jump = (sep === '\r' && this.lineBuffer[idx + 1] === '\n') ? 2 : 1;
      this.lineBuffer = this.lineBuffer.slice(idx + jump);
      if (line.length > 0) this._onLine(line);
    }
  }

  _onLine(line) {
    // Resolver-first dispatch: any send() call blocked on nextLine()
    // wins before anything lands in the buffer queue.
    if (this.lineResolvers.length > 0) {
      const entry = this.lineResolvers.shift();
      entry.resolve(line);
    } else {
      // No command is pending — this is unsolicited output from the
      // firmware (alive markers, Radio: RX lines, RNS debug, etc.).
      // Feed it to the UI's log panel so the user can see it scroll
      // in real time without needing PlatformIO's serial monitor.
      if (this.onUnsolicited) this.onUnsolicited(line);
      this.lineQueue.push(line);
    }
  }

  nextLine(timeoutMs = 5000) {
    if (this.lineQueue.length > 0) return Promise.resolve(this.lineQueue.shift());
    return new Promise((resolve, reject) => {
      const entry = { resolve, reject };
      const timer = setTimeout(() => {
        const i = this.lineResolvers.indexOf(entry);
        if (i >= 0) this.lineResolvers.splice(i, 1);
        reject(new Error('response timeout'));
      }, timeoutMs);
      entry.resolve = (line) => { clearTimeout(timer); resolve(line); };
      this.lineResolvers.push(entry);
    });
  }

  // Send a command and collect lines until the firmware emits "OK"
  // or "ERR: <reason>". Returns { ok, payload, error }.
  async send(cmd, timeoutMs = 5000) {
    // Drop any stale lines that slipped in between commands (alive
    // markers, async log callbacks from the RNS stack, etc.) so the
    // response read starts from a clean slate.
    this._drainAsync();
    this.log('tx', '> ' + cmd);

    // Set up the response collector BEFORE writing the command so
    // fast transports (BLE notifications) don't race ahead and dump
    // response lines into onUnsolicited before we're listening.
    const payload = [];
    let sawEcho = false;
    const isBle = !!(this.port && this.port._ble);
    const deadline = Date.now() + timeoutMs;

    const collectPromise = (async () => {
      while (true) {
        const remaining = Math.max(50, deadline - Date.now());
        const line = await this.nextLine(remaining);
        // Skip any async noise from the RNS log stream: "[VRB] ...",
        // "[DBG] ...", "[alive] ..." etc. Real responses never start
        // with '[' in the first column.
        if (line.startsWith('[')) continue;
        // Serial echoes the command back; BLE does not.
        if (!isBle && !sawEcho && line.trim() === cmd.trim()) { sawEcho = true; continue; }

        if (line === 'OK') { this.log('ok', 'OK'); return { ok: true, payload }; }
        if (line.startsWith('ERR:')) {
          const err = line.slice(4).trim();
          this.log('err', line);
          return { ok: false, payload, error: err };
        }
        payload.push(line);
        this.log('info', line);
      }
    })();

    await this.writer.write(cmd + '\n');
    return collectPromise;
  }

  _drainAsync() {
    // Drop queued async lines and any partial data in the line buffer.
    this.lineQueue = [];
    this.lineBuffer = '';
  }

  // Convenience wrappers -------------------------------------------

  parseKV(lines) {
    const out = {};
    for (const line of lines) {
      const i = line.indexOf('=');
      if (i > 0) out[line.slice(0, i).trim()] = line.slice(i + 1).trim();
    }
    return out;
  }

  async ping()        { const r = await this.send('PING');       if (!r.ok) throw new Error(r.error); }
  async version()     { const r = await this.send('VERSION');    if (!r.ok) throw new Error(r.error); return this.parseKV(r.payload); }
  async status()      { const r = await this.send('STATUS');     if (!r.ok) throw new Error(r.error); return this.parseKV(r.payload); }
  // Pipe-delimited field order — must match firmware's print_fields_pipe()
  static get PIPE_FIELDS() {
    return ['display_name','freq_hz','bw_hz','sf','cr','txp_dbm','batt_mult',
            'tele_interval_ms','lxmf_interval_ms','telemetry','lxmf','heartbeat',
            'bt_enabled','bt_pin','latitude','longitude','altitude','log_level','collector',
            'tx_enabled'];
  }

  parsePipe(line) {
    const parts = line.split('|');
    const fields = RLRConsole.PIPE_FIELDS;
    if (parts.length !== fields.length) return null;
    const out = {};
    for (let i = 0; i < fields.length; i++) out[fields[i]] = parts[i];
    return out;
  }

  async configGet() {
    // BLE: atomic GATT read. Serial: text command.
    if (this.port && this.port._ble) {
      return await this.bleConfigRead();
    }
    const r = await this.send('CONFIG GETP');
    if (!r.ok) throw new Error(r.error || 'CONFIG GETP failed');
    const parsed = this.parsePipe(r.payload.join(''));
    if (!parsed) throw new Error('failed to parse pipe response: ' + r.payload.join(''));
    return parsed;
  }
  async configSet(k, v) { const r = await this.send(`CONFIG SET ${k} ${v}`); if (!r.ok) throw new Error(r.error); }
  async configReset() { const r = await this.send('CONFIG RESET');  if (!r.ok) throw new Error(r.error); }
  async configRevert(){ const r = await this.send('CONFIG REVERT'); if (!r.ok) throw new Error(r.error); }
  async configCommit(){ const r = await this.send('CONFIG COMMIT'); if (!r.ok) throw new Error(r.error); }
  async calibrateBattery(mv) {
    const r = await this.send(`CALIBRATE BATTERY ${mv}`);
    if (!r.ok) throw new Error(r.error);
    return this.parseKV(r.payload);
  }
  async reboot()      { const r = await this.send('REBOOT');     if (!r.ok) throw new Error(r.error); }
  async announce()    { const r = await this.send('ANNOUNCE');   if (!r.ok) throw new Error(r.error); }
  async dfu()         { const r = await this.send('DFU');        if (!r.ok) throw new Error(r.error); }

  // ---- Web Bluetooth transport -----------------------------------

  // Custom RLR Service UUIDs (must match firmware Ble.cpp)
  static get RLR_SERVICE()    { return '6c720001-0000-7272-4c52-a5a500000000'; }
  static get RLR_CONFIG_CHR() { return '6c720002-0000-7272-4c52-a5a500000000'; }
  static get RLR_COMMIT_CHR() { return '6c720003-0000-7272-4c52-a5a500000000'; }
  static get RLR_CMD_CHR()    { return '6c720004-0000-7272-4c52-a5a500000000'; }
  // NUS for log stream
  static get NUS_SERVICE()    { return '6e400001-b5a3-f393-e0a9-e50e24dcca9e'; }
  static get NUS_TX_CHAR()    { return '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; }

  async connectBle() {
    if (!('bluetooth' in navigator)) throw new Error('Web Bluetooth not supported in this browser');

    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [RLRConsole.RLR_SERVICE] }],
      optionalServices: [RLRConsole.NUS_SERVICE],
    });

    // Connect with retry
    let server;
    for (let attempt = 1; attempt <= 3; attempt++) {
      try {
        server = await device.gatt.connect();
        await new Promise(r => setTimeout(r, 500));
        if (!server.connected) throw new Error('GATT disconnected during pairing');
        break;
      } catch (e) {
        if (attempt === 3) throw e;
        this.log && this.log('info', `BLE attempt ${attempt} failed: ${e.message}, retrying...`);
        await new Promise(r => setTimeout(r, 1000));
      }
    }

    // Discover custom RLR service
    const rlrService = await server.getPrimaryService(RLRConsole.RLR_SERVICE);
    this._bleConfigChr = await rlrService.getCharacteristic(RLRConsole.RLR_CONFIG_CHR);
    this._bleCommitChr = await rlrService.getCharacteristic(RLRConsole.RLR_COMMIT_CHR);
    this._bleCmdChr    = await rlrService.getCharacteristic(RLRConsole.RLR_CMD_CHR);

    // Discover NUS for log stream (optional — may not be available)
    try {
      const nusService = await server.getPrimaryService(RLRConsole.NUS_SERVICE);
      const nusTx = await nusService.getCharacteristic(RLRConsole.NUS_TX_CHAR);
      await nusTx.startNotifications();
      this._bleTxChar = nusTx;
      const decoder = new TextDecoder();
      this._bleNotifyHandler = (ev) => {
        const chunk = decoder.decode(ev.target.value, { stream: true });
        this.lineBuffer += chunk;
        this._processLineBuffer();
      };
      nusTx.addEventListener('characteristicvaluechanged', this._bleNotifyHandler);
    } catch (e) {
      this.log && this.log('info', 'NUS log stream not available: ' + e.message);
    }

    // Set up a dummy writer for send() compatibility (commands use _bleCmdChr)
    this.writer = {
      write: async (text) => {
        const enc = new TextEncoder();
        await this._bleCmdChr.writeValueWithResponse(enc.encode(text));
      },
      close: async () => {},
    };

    this._bleDevice = device;
    this.port = { _ble: true };

    device.addEventListener('gattserverdisconnected', () => {
      this._teardown().then(() => {
        if (this.onDisconnect) this.onDisconnect();
      });
    });
  }

  // Read config atomically via GATT characteristic (no line parsing)
  async bleConfigRead() {
    if (!this._bleConfigChr) throw new Error('not connected via BLE');
    const value = await this._bleConfigChr.readValue();
    const text = new TextDecoder().decode(value);
    const parsed = this.parsePipe(text);
    if (!parsed) throw new Error('failed to parse config: ' + text);
    return parsed;
  }

  // Write config atomically via GATT characteristic
  async bleConfigWrite(pipeString) {
    if (!this._bleConfigChr) throw new Error('not connected via BLE');
    const enc = new TextEncoder();
    await this._bleConfigChr.writeValueWithResponse(enc.encode(pipeString));
  }

  // Commit config via GATT characteristic (0x01 = save + reboot)
  async bleCommit() {
    if (!this._bleCommitChr) throw new Error('not connected via BLE');
    await this._bleCommitChr.writeValueWithResponse(new Uint8Array([0x01]));
  }

  // Send a command via GATT command characteristic
  async bleCommand(cmd) {
    if (!this._bleCmdChr) throw new Error('not connected via BLE');
    const enc = new TextEncoder();
    await this._bleCmdChr.writeValueWithResponse(enc.encode(cmd));
  }

  // ----------------------------------------------------------------

  async disconnect() {
    if (!this.port) return;
    await this._teardown();
  }

  async _teardown() {
    // BLE path: remove notification listener, disconnect GATT
    if (this._bleTxChar && this._bleNotifyHandler) {
      try { this._bleTxChar.removeEventListener('characteristicvaluechanged', this._bleNotifyHandler); } catch (e) {}
    }
    if (this._bleDevice) {
      try { if (this._bleDevice.gatt.connected) this._bleDevice.gatt.disconnect(); } catch (e) {}
    }
    this._bleDevice = null;
    this._bleTxChar = null;
    this._bleNotifyHandler = null;
    this._bleConfigChr = null;
    this._bleCommitChr = null;
    this._bleCmdChr = null;
    // Serial path: cancel reader/writer streams, close port
    try { if (this.reader) await this.reader.cancel(); } catch (e) {}
    try { if (this.readerClosed) await this.readerClosed; } catch (e) {}
    try { if (this.writer) { await this.writer.close(); } } catch (e) {}
    try { if (this.writerClosed) await this.writerClosed; } catch (e) {}
    try { if (this.port && !this.port._ble) await this.port.close(); } catch (e) {}
    this.port = null;
    this.reader = null;
    this.writer = null;
    this.lineBuffer = '';
    this.lineQueue = [];
    // Reject any pending resolvers so callers unblock instead of
    // hanging forever on a dead port.
    for (const entry of this.lineResolvers) {
      try { entry.reject && entry.reject(new Error('disconnected')); } catch (e) {}
    }
    this.lineResolvers = [];
  }
}

// ---------------------------------------------------------------
//  UI glue
// ---------------------------------------------------------------
(function () {
  const $ = (id) => document.getElementById(id);

  // Environment checks --------------------------------------------
  if (!('serial' in navigator)) {
    $('unsupported').classList.remove('hidden');
    $('btn-connect').disabled = true;
  }
  if (!('bluetooth' in navigator)) {
    $('btn-connect-ble').disabled = true;
    $('ble-unsupported').classList.remove('hidden');
  }
  if (location.protocol !== 'https:' && location.hostname !== 'localhost' && location.protocol !== 'file:') {
    $('http-warn').classList.remove('hidden');
  }

  // Log panel -----------------------------------------------------
  const logEl = $('log');
  function log(kind, msg) {
    const span = document.createElement('span');
    span.className = kind;
    span.textContent = msg + '\n';
    logEl.appendChild(span);
    logEl.scrollTop = logEl.scrollHeight;
    // Trim to last ~500 lines to keep the DOM cheap.
    while (logEl.childNodes.length > 500) logEl.removeChild(logEl.firstChild);
  }

  // Connection state ---------------------------------------------
  const con = new RLRConsole(log);
  const dot = $('conn-dot');
  const txt = $('conn-text');
  const btnConnect    = $('btn-connect');
  const btnDisconnect = $('btn-disconnect');
  const liveDiv       = $('live');

  function setConnected(on, label) {
    dot.classList.toggle('on', on);
    dot.classList.toggle('err', false);
    txt.textContent = label || (on ? 'Connected' : 'Disconnected');
    btnConnect.classList.toggle('hidden', on);
    $('btn-connect-ble').classList.toggle('hidden', on);
    btnDisconnect.classList.toggle('hidden', !on);
    liveDiv.classList.toggle('hidden', !on);
    if (!on) $('config-panel').classList.add('hidden');
  }

  // Stream unsolicited firmware output (alive markers, Radio: RX
  // lines, RNS debug) into the Log panel in real time so the user
  // gets a live serial monitor without needing PlatformIO.
  con.onUnsolicited = (line) => {
    log('info', line);
  };

  con.onDisconnect = () => {
    log('info', '--- port disconnected ---');
    setConnected(false);
  };

  // All action buttons that should be disabled during loading
  const actionButtons = ['btn-commit', 'btn-revert', 'btn-reset', 'btn-export',
    'btn-import', 'btn-reboot', 'btn-status', 'btn-announce', 'btn-calibrate'];

  function setLoading(on) {
    const overlay = $('loading-overlay');
    if (overlay) overlay.classList.toggle('hidden', !on);
    for (const id of actionButtons) {
      const el = $(id);
      if (el) el.disabled = on;
    }
  }

  btnConnect.addEventListener('click', async () => {
    try {
      await con.connect();
      setConnected(true, 'Connecting…');
      setLoading(true);
      log('info', '--- port opened ---');
      await sleep(150);
      await refreshConfig();
      setLoading(false);
      setConnected(true, 'Connected');
    } catch (e) {
      log('err', 'connect failed: ' + e.message);
      setLoading(false);
      setConnected(false);
    }
  });

  $('btn-connect-ble').addEventListener('click', async () => {
    try {
      await con.connectBle();
      setConnected(true, 'Connecting (BLE)…');
      setLoading(true);
      log('info', '--- BLE connected ---');
      await refreshConfig();
      setLoading(false);
      setConnected(true, 'Connected (BLE)');
    } catch (e) {
      log('err', 'BLE connect failed: ' + e.message);
      setLoading(false);
      setConnected(false);
    }
  });

  btnDisconnect.addEventListener('click', async () => {
    await con.disconnect();
    setConnected(false);
    log('info', '--- disconnected ---');
  });

  // Config loading ------------------------------------------------

  let originalCfg = {};
  async function refreshConfig() {
    const c = await con.configGet();
    originalCfg = { ...c };
    $('cfg-display_name').value     = c.display_name || '';
    $('cfg-freq_mhz').value         = c.freq_hz ? (Number(c.freq_hz) / 1000000).toFixed(3) : '';
    $('cfg-bw_hz').value            = String(c.bw_hz || '');
    $('cfg-sf').value               = String(c.sf || '');
    $('cfg-cr').value               = String(c.cr || '');
    $('cfg-txp_dbm').value          = String(c.txp_dbm || '');
    $('cfg-tx_enabled').checked     = Number(c.tx_enabled) === 1;
    $('cfg-tele_interval_min').value = c.tele_interval_ms ? Math.round(Number(c.tele_interval_ms) / 60000) : '';
    $('cfg-lxmf_interval_min').value = c.lxmf_interval_ms ? Math.round(Number(c.lxmf_interval_ms) / 60000) : '';
    $('cfg-telemetry').checked      = Number(c.telemetry) === 1;
    $('cfg-lxmf').checked           = Number(c.lxmf) === 1;
    $('cfg-heartbeat').checked      = Number(c.heartbeat) === 1;
    $('cfg-bt_enabled').checked     = Number(c.bt_enabled) === 1;
    $('cfg-bt_pin').value           = String(c.bt_pin || '0');
    $('cfg-latitude').value         = String(c.latitude || '0.000000');
    $('cfg-longitude').value        = String(c.longitude || '0.000000');
    $('cfg-altitude').value         = String(c.altitude || '0');
    $('cfg-log_level').value        = String(c.log_level || '1');
    $('cfg-collector').value        = c.collector || '';
    // Normalize blank collector to 'none' for change-detection so an
    // unset field doesn't spuriously diff against the blank→'none'
    // mapping in formValues() (firmware rejects an empty SET value).
    originalCfg.collector           = c.collector || 'none';
    // No battery fields to populate — calibration panel is self-contained.
    // Show config panel now that data is loaded
    $('config-panel').classList.remove('hidden');
  }

  // Calibration --------------------------------------------------
  $('btn-calibrate').addEventListener('click', async () => {
    const volts = parseFloat($('bat-measured').value);
    if (!Number.isFinite(volts) || volts < 0.5 || volts > 10) {
      log('err', 'enter a measured voltage in volts (0.5..10)');
      return;
    }
    const mv = Math.round(volts * 1000);
    try {
      const result = await con.calibrateBattery(mv);
      log('ok', `staged batt_mult=${result.batt_mult} (commit to persist)`);
      await refreshConfig();
    } catch (e) {
      log('err', 'calibrate failed: ' + e.message);
    }
  });

  // Geolocation ---------------------------------------------------
  $('btn-geolocate').addEventListener('click', () => {
    if (!('geolocation' in navigator)) {
      log('err', 'Geolocation not available in this browser');
      return;
    }
    log('info', 'Requesting location...');
    navigator.geolocation.getCurrentPosition(
      (pos) => {
        $('cfg-latitude').value  = pos.coords.latitude.toFixed(6);
        $('cfg-longitude').value = pos.coords.longitude.toFixed(6);
        if (pos.coords.altitude !== null) {
          $('cfg-altitude').value = Math.round(pos.coords.altitude);
        }
        log('ok', `location: ${pos.coords.latitude.toFixed(6)}, ${pos.coords.longitude.toFixed(6)}` +
            (pos.coords.altitude !== null ? `, ${Math.round(pos.coords.altitude)}m MSL` : ' (no altitude)'));
      },
      (err) => {
        log('err', 'Location failed: ' + err.message);
      },
      { enableHighAccuracy: true, timeout: 10000 }
    );
  });

  // Config commit ------------------------------------------------
  function formValues() {
    return {
      display_name:     $('cfg-display_name').value,
      freq_hz:          $('cfg-freq_mhz').value ? String(Math.round(parseFloat($('cfg-freq_mhz').value) * 1000000)) : '',
      bw_hz:            $('cfg-bw_hz').value,
      sf:               $('cfg-sf').value,
      cr:               $('cfg-cr').value,
      txp_dbm:          $('cfg-txp_dbm').value,
      tx_enabled:       $('cfg-tx_enabled').checked ? '1' : '0',
      tele_interval_ms: $('cfg-tele_interval_min').value ? String(Math.round(parseFloat($('cfg-tele_interval_min').value) * 60000)) : '',
      lxmf_interval_ms: $('cfg-lxmf_interval_min').value ? String(Math.round(parseFloat($('cfg-lxmf_interval_min').value) * 60000)) : '',
      telemetry:        $('cfg-telemetry').checked ? '1' : '0',
      lxmf:             $('cfg-lxmf').checked      ? '1' : '0',
      heartbeat:        $('cfg-heartbeat').checked ? '1' : '0',
      bt_enabled:       $('cfg-bt_enabled').checked ? '1' : '0',
      bt_pin:           $('cfg-bt_pin').value || '0',
      latitude:         $('cfg-latitude').value || '0',
      longitude:        $('cfg-longitude').value || '0',
      altitude:         $('cfg-altitude').value || '0',
      log_level:        $('cfg-log_level').value || '1',
      // Blank → 'none' so set_field() clears it (an empty value is
      // rejected by the serial CONFIG SET path).
      collector:        ($('cfg-collector').value.trim() || 'none'),
    };
  }

  // Client-side validation — mirrors firmware set_field() ranges.
  // Returns an array of error strings, empty if all OK.
  function validateForm() {
    const errs = [];
    const name = $('cfg-display_name').value;
    if (!name || name.length === 0)  errs.push('Display name must not be empty');
    if (name.length > 31)            errs.push('Display name max 31 characters');
    if (name.includes('|'))          errs.push('Display name must not contain "|"');

    const freq = parseFloat($('cfg-freq_mhz').value);
    if (isNaN(freq) || freq < 100 || freq > 1100) errs.push('Frequency must be 100..1100 MHz');

    const bw = parseInt($('cfg-bw_hz').value);
    if (isNaN(bw) || bw < 7800 || bw > 500000) errs.push('Bandwidth must be 7800..500000 Hz');

    const sf = parseInt($('cfg-sf').value);
    if (isNaN(sf) || sf < 7 || sf > 12) errs.push('Spreading factor must be 7..12');

    const cr = parseInt($('cfg-cr').value);
    if (isNaN(cr) || cr < 5 || cr > 8) errs.push('Coding rate must be 5..8');

    const txp = parseInt($('cfg-txp_dbm').value);
    if (isNaN(txp) || txp < -9 || txp > 22) errs.push('TX power must be -9..22 dBm');

    const tele = parseFloat($('cfg-tele_interval_min').value);
    if (isNaN(tele) || tele < 0) errs.push('Telemetry interval must be >= 0 minutes');

    const lxmf = parseFloat($('cfg-lxmf_interval_min').value);
    if (isNaN(lxmf) || lxmf < 0) errs.push('LXMF interval must be >= 0 minutes');

    const pin = parseInt($('cfg-bt_pin').value);
    if (isNaN(pin) || pin < 0 || pin > 999999) errs.push('BT PIN must be 0..999999');

    const lat = parseFloat($('cfg-latitude').value);
    if (isNaN(lat) || lat < -90 || lat > 90) errs.push('Latitude must be -90..90');

    const lon = parseFloat($('cfg-longitude').value);
    if (isNaN(lon) || lon < -180 || lon > 180) errs.push('Longitude must be -180..180');

    const alt = parseInt($('cfg-altitude').value);
    if (isNaN(alt) || alt < -100000 || alt > 100000) errs.push('Altitude must be -100000..100000 m');

    const collector = $('cfg-collector').value.trim();
    if (collector && !/^[0-9a-fA-F]{32}$/.test(collector)) {
      errs.push('Collector must be 32 hex characters (16-byte destination hash) or blank');
    }

    return errs;
  }

  // Build pipe string from form values (matches firmware field order)
  function buildPipeString(vals) {
    const fields = RLRConsole.PIPE_FIELDS;
    return fields.map(f => String(vals[f] || '')).join('|');
  }

  $('btn-commit').addEventListener('click', async () => {
    const validationErrors = validateForm();
    if (validationErrors.length > 0) {
      for (const e of validationErrors) log('err', 'Validation: ' + e);
      return;
    }
    const vals = formValues();
    try {
      const isBle = con.port && con.port._ble;
      if (isBle) {
        // BLE: write full config as pipe string, then commit
        const pipe = buildPipeString(vals);
        log('info', 'writing config via BLE...');
        await con.bleConfigWrite(pipe);
        await con.bleCommit();
      } else {
        // Serial: push changed fields individually
        const changes = [];
        for (const [k, v] of Object.entries(vals)) {
          if (String(originalCfg[k]) !== String(v)) changes.push([k, v]);
        }
        if (changes.length === 0) {
          log('info', 'no form changes — committing in case CALIBRATE was staged');
        } else {
          log('info', `applying ${changes.length} change(s)`);
        }
        for (const [k, v] of changes) {
          await con.configSet(k, v);
        }
        await con.configCommit();
      }
      log('ok', '--- committed, device rebooting ---');
      // The device NVIC-resets right after it prints OK; the serial
      // port will drop within a few hundred ms. Tear down the local
      // state and wait for the user to reconnect.
      setTimeout(async () => {
        try { await con.disconnect(); } catch (e) {}
        setConnected(false, 'Rebooted — click Connect');
      }, 500);
    } catch (e) {
      log('err', 'commit failed: ' + e.message);
    }
  });

  $('btn-revert').addEventListener('click', async () => {
    try {
      await con.configRevert();
      await refreshConfig();
      log('ok', 'reverted staging to live config');
    } catch (e) {
      log('err', 'revert failed: ' + e.message);
    }
  });

  $('btn-reset').addEventListener('click', async () => {
    if (!confirm('Reset staging to board defaults? (does not touch flash until you Commit)')) return;
    try {
      await con.configReset();
      await refreshConfig();
      log('ok', 'staging reseeded from defaults');
    } catch (e) {
      log('err', 'reset failed: ' + e.message);
    }
  });

  $('btn-reboot').addEventListener('click', async () => {
    if (!confirm('Reboot the device now? Any uncommitted edits will be lost.')) return;
    try {
      await con.reboot();
      setTimeout(async () => {
        try { await con.disconnect(); } catch (e) {}
        setConnected(false, 'Rebooted — click Connect');
      }, 500);
    } catch (e) {
      log('err', 'reboot failed: ' + e.message);
    }
  });

  $('btn-status').addEventListener('click', async () => {
    try {
      if (con.port && con.port._ble) {
        await con.bleCommand('STATUS');
        log('ok', 'status requested (see log stream)');
      } else {
        const r = await con.send('STATUS');
        if (r.ok) {
          for (const line of r.payload) log('info', line);
          log('ok', 'status retrieved');
        } else {
          log('err', 'status failed: ' + r.error);
        }
      }
    } catch (e) {
      log('err', 'status failed: ' + e.message);
    }
  });

  $('btn-announce').addEventListener('click', async () => {
    try {
      if (con.port && con.port._ble) {
        await con.bleCommand('ANNOUNCE');
        log('ok', 'announce requested');
      } else {
        await con.announce();
        log('ok', 'announce sent');
      }
    } catch (e) {
      log('err', 'announce failed: ' + e.message);
    }
  });

  // ---------------------------------------------------------------
  //  Config export / import
  // ---------------------------------------------------------------

  // Export: read form values (in user-friendly units) and download
  // as a JSON file. The file uses the same field names as the
  // firmware's CONFIG GET but with human-friendly values (MHz, min).
  $('btn-export').addEventListener('click', () => {
    const cfg = {
      display_name:     $('cfg-display_name').value,
      freq_mhz:         parseFloat($('cfg-freq_mhz').value) || 0,
      bw_hz:            parseInt($('cfg-bw_hz').value) || 0,
      sf:               parseInt($('cfg-sf').value) || 0,
      cr:               parseInt($('cfg-cr').value) || 0,
      txp_dbm:          parseInt($('cfg-txp_dbm').value) || 0,
      tx_enabled:       $('cfg-tx_enabled').checked,
      tele_interval_min: parseFloat($('cfg-tele_interval_min').value) || 0,
      lxmf_interval_min: parseFloat($('cfg-lxmf_interval_min').value) || 0,
      telemetry:        $('cfg-telemetry').checked,
      lxmf:             $('cfg-lxmf').checked,
      heartbeat:        $('cfg-heartbeat').checked,
      bt_enabled:       $('cfg-bt_enabled').checked,
      bt_pin:           parseInt($('cfg-bt_pin').value) || 0,
      latitude:         parseFloat($('cfg-latitude').value) || 0,
      longitude:        parseFloat($('cfg-longitude').value) || 0,
      altitude:         parseInt($('cfg-altitude').value) || 0,
      log_level:        parseInt($('cfg-log_level').value) || 1,
      collector:        $('cfg-collector').value.trim(),
    };
    const json = JSON.stringify(cfg, null, 2);
    const blob = new Blob([json], { type: 'application/json' });
    const url  = URL.createObjectURL(blob);
    const a    = document.createElement('a');
    a.href     = url;
    a.download = 'rlr-config.json';
    a.click();
    URL.revokeObjectURL(url);
    log('ok', 'config exported to rlr-config.json');
  });

  // Import: read a JSON file and populate the form. The user still
  // needs to click Commit to push it to the device — import only
  // fills the form, it doesn't touch the serial port.
  $('btn-import').addEventListener('click', () => {
    $('cfg-import-file').click();
  });

  $('cfg-import-file').addEventListener('change', async () => {
    const f = $('cfg-import-file').files && $('cfg-import-file').files[0];
    if (!f) return;
    try {
      const text = await f.text();
      const cfg  = JSON.parse(text);
      if (cfg.display_name !== undefined)      $('cfg-display_name').value     = cfg.display_name;
      if (cfg.freq_mhz !== undefined)          $('cfg-freq_mhz').value         = cfg.freq_mhz;
      if (cfg.bw_hz !== undefined)             $('cfg-bw_hz').value            = String(cfg.bw_hz);
      if (cfg.sf !== undefined)                $('cfg-sf').value               = String(cfg.sf);
      if (cfg.cr !== undefined)                $('cfg-cr').value               = String(cfg.cr);
      if (cfg.txp_dbm !== undefined)           $('cfg-txp_dbm').value          = cfg.txp_dbm;
      if (cfg.tx_enabled !== undefined)        $('cfg-tx_enabled').checked     = !!cfg.tx_enabled;
      if (cfg.tele_interval_min !== undefined)  $('cfg-tele_interval_min').value = cfg.tele_interval_min;
      if (cfg.lxmf_interval_min !== undefined)  $('cfg-lxmf_interval_min').value = cfg.lxmf_interval_min;
      if (cfg.telemetry !== undefined)         $('cfg-telemetry').checked      = !!cfg.telemetry;
      if (cfg.lxmf !== undefined)              $('cfg-lxmf').checked           = !!cfg.lxmf;
      if (cfg.heartbeat !== undefined)         $('cfg-heartbeat').checked      = !!cfg.heartbeat;
      if (cfg.bt_enabled !== undefined)        $('cfg-bt_enabled').checked     = !!cfg.bt_enabled;
      if (cfg.bt_pin !== undefined)            $('cfg-bt_pin').value           = cfg.bt_pin;
      if (cfg.latitude !== undefined)          $('cfg-latitude').value         = cfg.latitude;
      if (cfg.longitude !== undefined)         $('cfg-longitude').value        = cfg.longitude;
      if (cfg.altitude !== undefined)          $('cfg-altitude').value         = cfg.altitude;
      if (cfg.log_level !== undefined)        $('cfg-log_level').value        = cfg.log_level;
      if (cfg.collector !== undefined)        $('cfg-collector').value        = cfg.collector;
      log('ok', 'config imported from ' + f.name + ' — edit display_name if needed, then Commit');
    } catch (e) {
      log('err', 'import failed: ' + e.message);
    }
    // Reset the file input so re-importing the same file triggers change again
    $('cfg-import-file').value = '';
  });

  function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }
})();
