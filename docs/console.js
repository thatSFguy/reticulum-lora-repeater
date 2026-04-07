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
    // Over BLE, wait briefly for any in-flight notifications from
    // a previous command to arrive and get drained.
    if (this.port && this.port._ble) {
      await new Promise(r => setTimeout(r, 150));
      this._drainAsync();
    }
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
            'bt_enabled','bt_pin','latitude','longitude','altitude'];
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
    const timeout = (this.port && this.port._ble) ? 10000 : 5000;
    const r = await this.send('CONFIG GETP', timeout);
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

  async connectBle() {
    if (!('bluetooth' in navigator)) throw new Error('Web Bluetooth not supported in this browser');
    const NUS_SERVICE  = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
    const NUS_RX_CHAR  = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // phone writes to device
    const NUS_TX_CHAR  = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // device notifies phone

    const device = await navigator.bluetooth.requestDevice({
      filters: [{ services: [NUS_SERVICE] }],
    });

    // Connect with retry — GATT can disconnect during service discovery
    // if pairing/bonding needs to settle. Give it up to 3 attempts.
    let server, service, txChar, rxChar;
    for (let attempt = 1; attempt <= 3; attempt++) {
      try {
        server  = await device.gatt.connect();
        // Small delay after GATT connect to let pairing/encryption settle
        await new Promise(r => setTimeout(r, 500));
        if (!server.connected) throw new Error('GATT disconnected during pairing');
        service = await server.getPrimaryService(NUS_SERVICE);
        txChar  = await service.getCharacteristic(NUS_TX_CHAR);
        rxChar  = await service.getCharacteristic(NUS_RX_CHAR);
        break;
      } catch (e) {
        if (attempt === 3) throw e;
        this.log && this.log('info', `BLE connect attempt ${attempt} failed: ${e.message}, retrying...`);
        await new Promise(r => setTimeout(r, 1000));
      }
    }

    await txChar.startNotifications();
    this._bleDevice = device;
    this._bleRxChar = rxChar;

    // Wire TX notifications into the same _onLine pipeline as serial
    const decoder = new TextDecoder();
    txChar.addEventListener('characteristicvaluechanged', (ev) => {
      const chunk = decoder.decode(ev.target.value, { stream: true });
      this.lineBuffer += chunk;
      this._processLineBuffer();
    });

    // Replace the writer with a BLE-backed write function
    this.writer = {
      write: async (text) => {
        const enc = new TextEncoder();
        const data = enc.encode(text);
        // Chunk into 20-byte MTU segments
        for (let i = 0; i < data.length; i += 20) {
          await rxChar.writeValueWithResponse(data.slice(i, Math.min(i + 20, data.length)));
        }
      },
      close: async () => {},
    };

    // Mark as connected (port is used for isConnected check)
    this.port = { _ble: true };

    device.addEventListener('gattserverdisconnected', () => {
      this._teardown().then(() => {
        if (this.onDisconnect) this.onDisconnect();
      });
    });
  }

  // ----------------------------------------------------------------

  async disconnect() {
    if (!this.port) return;
    await this._teardown();
  }

  async _teardown() {
    // BLE path: disconnect GATT, clean up device reference
    if (this._bleDevice) {
      try { if (this._bleDevice.gatt.connected) this._bleDevice.gatt.disconnect(); } catch (e) {}
      this._bleDevice = null;
      this._bleRxChar = null;
    }
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
      await sleep(500);
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

    return errs;
  }

  $('btn-commit').addEventListener('click', async () => {
    const validationErrors = validateForm();
    if (validationErrors.length > 0) {
      for (const e of validationErrors) log('err', 'Validation: ' + e);
      return;
    }
    const vals = formValues();
    // Only push fields that actually changed — quieter log, fewer
    // round-trips, and it plays nicely with firmware-managed fields
    // like batt_mult that the user edited via CALIBRATE BATTERY.
    const changes = [];
    for (const [k, v] of Object.entries(vals)) {
      if (String(originalCfg[k]) !== String(v)) changes.push([k, v]);
    }
    if (changes.length === 0) {
      log('info', 'no form changes — committing in case CALIBRATE was staged');
    } else {
      log('info', `applying ${changes.length} change(s)`);
    }
    try {
      for (const [k, v] of changes) {
        await con.configSet(k, v);
      }
      await con.configCommit();
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
      const r = await con.send('STATUS');
      if (r.ok) {
        for (const line of r.payload) log('info', line);
        log('ok', 'status retrieved');
      } else {
        log('err', 'status failed: ' + r.error);
      }
    } catch (e) {
      log('err', 'status failed: ' + e.message);
    }
  });

  $('btn-announce').addEventListener('click', async () => {
    try {
      await con.announce();
      log('ok', 'announce sent');
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
      log('ok', 'config imported from ' + f.name + ' — edit display_name if needed, then Commit');
    } catch (e) {
      log('err', 'import failed: ' + e.message);
    }
    // Reset the file input so re-importing the same file triggers change again
    $('cfg-import-file').value = '';
  });

  function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

  // Wait for a USB re-enumeration event after a DFU touch. Resolves
  // to 'disconnected' if the touched port drops, 'connected' if a
  // new port appears, or 'timeout' if neither happens in time. Both
  // events clean up their listeners on resolution so we don't leak
  // handlers into subsequent flash attempts.
  function waitForReenumeration(touchedPort, timeoutMs) {
    return new Promise((resolve) => {
      let resolved = false;
      const finish = (outcome) => {
        if (resolved) return;
        resolved = true;
        navigator.serial.removeEventListener('connect',    onConnect);
        navigator.serial.removeEventListener('disconnect', onDisconnect);
        clearTimeout(timer);
        resolve(outcome);
      };
      const onConnect    = ()   => finish('connected');
      const onDisconnect = (ev) => {
        // Any disconnect during the wait window counts — we don't
        // try to match the event to the touched port because the
        // port object identity can be unstable across re-enumeration
        // on some OSes.
        finish('disconnected');
      };
      navigator.serial.addEventListener('connect',    onConnect);
      navigator.serial.addEventListener('disconnect', onDisconnect);
      const timer = setTimeout(() => finish('timeout'), timeoutMs);
    });
  }

  // ---------------------------------------------------------------
  //  Flash panel — picks a firmware.zip, opens a SECOND serial port
  //  (the bootloader CDC, not the application CDC the rest of this
  //  page talks to), and drives dfu.js through an APPLICATION update.
  // ---------------------------------------------------------------
  const fwFile  = $('fw-file');
  const fwInfo  = $('fw-info');
  const btnFlash = $('btn-flash');
  const btnUf2   = $('btn-download-uf2');
  const fwBar   = $('fw-progress-bar');
  const fwStage = $('fw-stage');

  // Release picker elements.
  const relVersion     = $('rel-version');
  const relBoard       = $('rel-board');
  const relStatus      = $('rel-status');
  const btnLoadRelease = $('btn-load-release');

  let selectedPackage = null;

  // Shared "package is loaded" callback — reused by both the local
  // file path and the release-download path so the UI stays in one
  // place and the two sources can't drift. Also gates the DFU and
  // Flash buttons on having a package, because there's no point
  // putting the board into bootloader mode if we have nothing to
  // flash.
  let currentUf2Path = null;  // set when release is loaded

  function setLoadedPackage(pkg, label) {
    selectedPackage = pkg;
    const btnDfuEl = document.getElementById('btn-dfu');
    if (pkg) {
      const appSize  = pkg.firmware.length;
      const initSize = pkg.initPacket.length;
      fwInfo.textContent = `${label} — app ${appSize} B, init ${initSize} B`;
      btnFlash.disabled  = false;
      if (btnDfuEl) btnDfuEl.disabled = false;
      if (btnUf2) btnUf2.disabled = !currentUf2Path;
      log('info', `package loaded (${label}): app=${appSize} bytes, init=${initSize} bytes`);
    } else {
      fwInfo.textContent = 'no firmware loaded';
      btnFlash.disabled  = true;
      if (btnDfuEl) btnDfuEl.disabled = true;
      if (btnUf2) btnUf2.disabled = true;
    }
  }

  // UF2 download — triggers a browser download of the .uf2 file for
  // boards with UF2 bootloaders (XIAO, etc.). User double-taps reset
  // to get the XIAOBOOT/NICENANO USB drive, then drags the .uf2 onto it.
  if (btnUf2) {
    btnUf2.addEventListener('click', async () => {
      if (!currentUf2Path) { log('err', 'no UF2 file available for this release'); return; }
      try {
        const res = await fetch(currentUf2Path);
        if (!res.ok) throw new Error(`HTTP ${res.status}`);
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = currentUf2Path.split('/').pop();
        a.click();
        URL.revokeObjectURL(url);
        log('ok', `downloaded ${a.download} — drag to bootloader USB drive`);
      } catch (e) {
        log('err', 'UF2 download failed: ' + e.message);
      }
    });
  }

  fwFile.addEventListener('change', async () => {
    const f = fwFile.files && fwFile.files[0];
    if (!f) { setLoadedPackage(null); return; }
    try {
      const pkg = await RLRDfu.DfuPackage.fromFile(f);
      setLoadedPackage(pkg, f.name);
    } catch (e) {
      setLoadedPackage(null);
      fwInfo.textContent = 'error: ' + e.message;
      log('err', 'package parse failed: ' + e.message);
    }
  });

  // ---------------------------------------------------------------
  //  Firmware manifest integration (same-origin, no CORS)
  // ---------------------------------------------------------------
  //
  // Previously we fetched the release list from the GitHub Releases
  // API and downloaded asset bytes via browser_download_url (or the
  // api.github.com asset endpoint). Both paths are redirect chains
  // that end at release-assets.githubusercontent.com, which does
  // NOT emit Access-Control-Allow-Origin headers, so the browser
  // blocks fetch() from reading the response body even when the
  // 200 OK comes through at the network layer.
  //
  // Instead we publish the built firmware into docs/firmware/<tag>/
  // from the CI release workflow, alongside a regenerated
  // docs/firmware/manifest.json that lists every available tag +
  // board combination. Both live on the same origin as this page
  // (github.io Pages), so a relative-URL fetch is same-origin and
  // the CORS check doesn't apply at all.
  //
  // Manifest schema is documented in scripts/gen_firmware_manifest.py.

  const MANIFEST_URL = 'firmware/manifest.json';

  let releases = [];  // parsed manifest.releases, newest first

  async function fetchReleases() {
    relStatus.textContent = 'Loading firmware manifest…';
    try {
      // cache: 'no-cache' asks the browser to revalidate against
      // the Pages CDN so a freshly-released tag shows up within a
      // single refresh instead of waiting for cache expiry.
      const res = await fetch(MANIFEST_URL, { cache: 'no-cache' });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const manifest = await res.json();

      releases = (manifest.releases || []).filter(r => (r.boards || []).length > 0);

      if (releases.length === 0) {
        relStatus.textContent = 'No firmware releases published yet — use local upload instead.';
        relVersion.innerHTML = '<option>no releases</option>';
        return;
      }

      // Manifest is already sorted newest-first by the generator,
      // but do it again defensively in case someone hand-edits it.
      relVersion.innerHTML = '';
      for (const r of releases) {
        const opt = document.createElement('option');
        opt.value = r.tag;
        opt.textContent = r.prerelease ? `${r.tag} (prerelease)` : r.tag;
        relVersion.appendChild(opt);
      }

      // Default selection: the newest non-prerelease if any exist,
      // otherwise just the newest overall (first entry).
      const stable = releases.find(r => !r.prerelease);
      relVersion.value = (stable || releases[0]).tag;
      relVersion.disabled = false;
      repopulateBoardDropdown();
      relStatus.textContent = `Loaded ${releases.length} release(s) from manifest.`;
    } catch (e) {
      relStatus.textContent = 'Could not load manifest: ' + e.message + ' — use local upload instead.';
      log('err', 'manifest fetch failed: ' + e.message);
      relVersion.innerHTML = '<option>unavailable</option>';
    }
  }

  function repopulateBoardDropdown() {
    const r = releases.find(x => x.tag === relVersion.value);
    relBoard.innerHTML = '';
    if (!r) { relBoard.disabled = true; btnLoadRelease.disabled = true; return; }
    for (const b of r.boards) {
      const opt = document.createElement('option');
      opt.value = b.name;
      opt.textContent = b.name;
      relBoard.appendChild(opt);
    }
    relBoard.disabled       = false;
    btnLoadRelease.disabled = false;
  }

  relVersion.addEventListener('change', repopulateBoardDropdown);

  btnLoadRelease.addEventListener('click', async () => {
    const r = releases.find(x => x.tag === relVersion.value);
    if (!r) return;
    const board = r.boards.find(b => b.name === relBoard.value);
    if (!board || !board.zip_path) return;

    btnLoadRelease.disabled = true;
    relStatus.textContent = `Fetching ${board.zip_path}…`;
    try {
      // Same-origin relative fetch — no CORS involved at all.
      const res = await fetch(board.zip_path);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);

      // Stream the response so we can show download progress.
      // Pages sends Content-Length for static files so the progress
      // bar is determinate; fall back to manifest size if not.
      const total   = parseInt(res.headers.get('Content-Length') || '0', 10) || board.zip_size || 0;
      const reader  = res.body.getReader();
      const chunks  = [];
      let received  = 0;
      while (true) {
        const { done, value } = await reader.read();
        if (done) break;
        if (!value) continue;
        chunks.push(value);
        received += value.length;
        if (total > 0) {
          const pct = Math.floor(100 * received / total);
          relStatus.textContent = `Downloading: ${received} / ${total} B (${pct}%)`;
        } else {
          relStatus.textContent = `Downloading: ${received} B`;
        }
      }

      const buf = new Uint8Array(received);
      let offset = 0;
      for (const c of chunks) { buf.set(c, offset); offset += c.length; }

      const pkg = await RLRDfu.DfuPackage.fromArrayBuffer(buf.buffer);
      // Set UF2 path for the download button (if manifest includes it)
      currentUf2Path = board.uf2_path || null;
      setLoadedPackage(pkg, `${board.name} ${r.tag}`);
      relStatus.textContent = `Loaded ${board.zip_path.split('/').pop()} (${received} B).`;
    } catch (e) {
      relStatus.textContent = 'Download failed: ' + e.message;
      log('err', 'release download failed: ' + e.message);
    } finally {
      btnLoadRelease.disabled = false;
    }
  });

  // Kick off the release fetch as soon as the page loads. If the
  // user's browser can't reach GitHub (offline, CORS quirk, rate
  // limit), we log the error and leave the local-file path as the
  // working fallback.
  fetchReleases();

  // ---------------------------------------------------------------
  //  Enter DFU mode via the 1200-baud touch
  // ---------------------------------------------------------------
  //
  // The Adafruit nRF52 bootloader listens for a host-initiated
  // "open CDC at 1200 bps, then close" sequence. When it sees that,
  // it sets the 1200bps_touch flag and reboots into the bootloader
  // instead of the application on the next power-up. Same mechanism
  // as Arduino IDE's auto-reset and adafruit-nrfutil --touch 1200.
  //
  // If the user already has a console session open on the app port
  // we reuse that port object (saves a second port-picker click); if
  // not, we prompt requestPort() for the currently-running app port.
  // Either way the outcome is the same: device reboots into the
  // bootloader CDC, which will appear as a different port — the
  // user selects that new port when they click Flash.

  const btnDfu = $('btn-dfu');

  btnDfu.addEventListener('click', async () => {
    btnDfu.disabled = true;
    try {
      let dfuTouchPort = null;
      if (con.isConnected()) {
        // Capture the reference BEFORE disconnect() nulls it out in
        // _teardown, then close the console cleanly so the port's
        // reader/writer locks are released.
        dfuTouchPort = con.port;
        log('info', '--- closing console session to issue DFU touch ---');
        await con.disconnect();
        setConnected(false);
      } else {
        log('info', 'Pick the RUNNING application port (not the bootloader) to send the DFU touch...');
        dfuTouchPort = await navigator.serial.requestPort();
      }

      // Open at 1200 baud and close. The open() may fail briefly on
      // Windows because the port was literally just closed by
      // disconnect() above — a short retry wins that race.
      let opened = false;
      for (let i = 0; i < 5 && !opened; i++) {
        try {
          await dfuTouchPort.open({ baudRate: 1200 });
          opened = true;
        } catch (e) {
          if (i === 4) throw e;
          await sleep(150);
        }
      }

      // Explicitly drop DTR and RTS so the host driver sends a
      // definite "line state = inactive" signal before we tear the
      // port down. Web Serial asserts DTR by default on open(); some
      // bootloaders use the DTR-low transition (not just the 1200
      // baud rate itself) to decide whether this is a real touch or
      // a casual port probe. Wrap in try/catch because not every
      // platform supports setSignals and we don't want an exception
      // here to abort a touch that would otherwise work.
      try {
        await dfuTouchPort.setSignals({
          dataTerminalReady: false,
          requestToSend:     false,
        });
      } catch (e) {
        log('info', 'setSignals not supported or failed (ignored): ' + e.message);
      }

      // Hold long enough for the host's USB driver to propagate the
      // SET_LINE_CODING(1200) + DTR-low state to the device before
      // we disconnect it. 50 ms was too short (silent touch
      // failures); Arduino IDE and ESP Web Tools both use ~250 ms.
      await sleep(250);
      try { await dfuTouchPort.close(); } catch (e) {}

      log('ok', '--- DFU touch sent — waiting for the board to re-enumerate as a bootloader device ---');

      // Actively wait for the USB re-enumeration so the user gets
      // positive confirmation before clicking Flash. We listen for
      // both the 'disconnect' event on the touched port (board
      // reset) AND the 'connect' event for any new port (bootloader
      // enumerated). Either signals success; a timeout means the
      // touch didn't trigger a reboot.
      const reenum = await waitForReenumeration(dfuTouchPort, 4000);
      if (reenum === 'connected') {
        log('ok', '--- new USB device appeared — bootloader is ready, click Flash ---');
      } else if (reenum === 'disconnected') {
        log('ok', '--- board disconnected — give it a moment, then click Flash ---');
      } else {
        log('err', '--- no re-enumeration observed within 4 s ---');
        log('info', 'The 1200-baud touch did not trigger a reboot. Try double-tapping the reset button on the board instead.');
      }
    } catch (e) {
      log('err', 'DFU touch failed: ' + e.message);
    } finally {
      btnDfu.disabled = false;
    }
  });

  btnFlash.addEventListener('click', async () => {
    if (!selectedPackage) return;
    if (!('serial' in navigator)) { log('err', 'Web Serial not available'); return; }

    // If the console is currently connected, warn — it's almost
    // certainly the application port, which is NOT the bootloader
    // port. Force the user to disconnect so the port list in the
    // picker is clean.
    if (con.isConnected()) {
      if (!confirm('Disconnect the console session before flashing? The bootloader is a different USB port than the application.')) return;
      try { await con.disconnect(); } catch (e) {}
      setConnected(false);
    }

    btnFlash.disabled = true;
    fwFile.disabled   = true;
    fwBar.style.width = '0%';
    fwStage.textContent = 'Select the BOOTLOADER port in the picker...';

    let dfuPort = null;
    try {
      dfuPort = await navigator.serial.requestPort();
      await dfuPort.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none' });
      log('info', '--- bootloader port opened ---');

      await RLRDfu.dfuFlash(dfuPort, selectedPackage, {
        onStage: (text) => {
          fwStage.textContent = text;
          log('info', '[dfu] ' + text);
        },
        onProgress: (sent, total) => {
          const pct = Math.floor(100 * sent / total);
          fwBar.style.width = pct + '%';
        },
        log,
      });

      fwBar.style.width = '100%';
      fwStage.textContent = 'Flash complete — board is rebooting into the new firmware.';
      log('ok', '--- flash complete ---');
    } catch (e) {
      fwStage.textContent = 'Flash failed: ' + e.message;
      log('err', 'flash error: ' + e.message);
    } finally {
      try { if (dfuPort && dfuPort.readable) await dfuPort.close(); } catch (e) {}
      btnFlash.disabled = false;
      fwFile.disabled   = false;
    }
  });
})();
