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
        // Split on \r and \n; treat \r\n, \r, or \n all as one break.
        let idx;
        while ((idx = this.lineBuffer.search(/[\r\n]/)) >= 0) {
          const line = this.lineBuffer.slice(0, idx);
          const sep  = this.lineBuffer[idx];
          const jump = (sep === '\r' && this.lineBuffer[idx + 1] === '\n') ? 2 : 1;
          this.lineBuffer = this.lineBuffer.slice(idx + jump);
          if (line.length > 0) this._onLine(line);
        }
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
    await this.writer.write(cmd + '\n');

    // The firmware echoes characters as they arrive and then prints
    // the newline itself, so the first line we get back will be the
    // echo of the command we just sent. Discard it if it matches.
    // Some lines drift in from the RNS stack mid-response ([---] etc.)
    // and we also skip those silently.
    const payload = [];
    let sawEcho = false;
    const deadline = Date.now() + timeoutMs;
    while (true) {
      const remaining = Math.max(50, deadline - Date.now());
      const line = await this.nextLine(remaining);
      // Skip any async noise from the RNS log stream: "[VRB] ...",
      // "[DBG] ...", "[alive] ..." etc. Real responses never start
      // with '[' in the first column.
      if (line.startsWith('[')) continue;
      if (!sawEcho && line.trim() === cmd.trim()) { sawEcho = true; continue; }

      if (line === 'OK') { this.log('ok', 'OK'); return { ok: true, payload }; }
      if (line.startsWith('ERR:')) {
        const err = line.slice(4).trim();
        this.log('err', line);
        return { ok: false, payload, error: err };
      }
      payload.push(line);
      this.log('info', line);
    }
  }

  _drainAsync() {
    // Keep unresolved resolvers, drop queued async lines.
    this.lineQueue = [];
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
  async configGet()   { const r = await this.send('CONFIG GET'); if (!r.ok) throw new Error(r.error); return this.parseKV(r.payload); }
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

  // ----------------------------------------------------------------

  async disconnect() {
    if (!this.port) return;
    await this._teardown();
  }

  async _teardown() {
    // Cancelling the reader causes _readLoop's await to throw, which
    // unblocks the loop so pipeTo can finish before we close the port.
    try { if (this.reader) await this.reader.cancel(); } catch (e) {}
    try { if (this.readerClosed) await this.readerClosed; } catch (e) {}
    try { if (this.writer) { await this.writer.close(); } } catch (e) {}
    try { if (this.writerClosed) await this.writerClosed; } catch (e) {}
    try { if (this.port) await this.port.close(); } catch (e) {}
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
    btnDisconnect.classList.toggle('hidden', !on);
    liveDiv.classList.toggle('hidden', !on);
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

  btnConnect.addEventListener('click', async () => {
    try {
      await con.connect();
      setConnected(true, 'Connecting…');
      log('info', '--- port opened ---');
      // A short delay lets the firmware finish any startup bursts and
      // the alive tick so the first STATUS read is clean.
      await sleep(150);
      await refreshAll();
      setConnected(true, 'Connected');
    } catch (e) {
      log('err', 'connect failed: ' + e.message);
      setConnected(false);
    }
  });

  btnDisconnect.addEventListener('click', async () => {
    await con.disconnect();
    setConnected(false);
    log('info', '--- disconnected ---');
  });

  // Refresh status + config --------------------------------------
  const statusKV = $('status-kv');
  async function refreshStatus() {
    const s = await con.status();
    statusKV.innerHTML = '';
    const order = ['display_name', 'uptime_s', 'radio', 'packets_in', 'packets_out', 'paths', 'destinations', 'battery_raw', 'battery_mv', 'batt_mult'];
    for (const key of order) {
      if (!(key in s)) continue;
      const dt = document.createElement('dt'); dt.textContent = key;
      const dd = document.createElement('dd'); dd.textContent = s[key];
      statusKV.appendChild(dt); statusKV.appendChild(dd);
    }
    // Mirror battery fields into the calibration panel.
    if ('battery_raw' in s) $('bat-raw').value   = s.battery_raw;
    if ('battery_mv'  in s) $('bat-mv').value    = s.battery_mv;
    if ('batt_mult'   in s) $('bat-mult').value  = s.batt_mult;
  }

  let originalCfg = {};
  async function refreshConfig() {
    const c = await con.configGet();
    originalCfg = { ...c };
    $('cfg-display_name').value     = c.display_name || '';
    $('cfg-freq_mhz').value         = c.freq_hz ? (parseInt(c.freq_hz) / 1000000).toFixed(3) : '';
    $('cfg-bw_hz').value            = c.bw_hz || '';
    $('cfg-sf').value               = c.sf || '';
    $('cfg-cr').value               = c.cr || '';
    $('cfg-txp_dbm').value          = c.txp_dbm || '';
    $('cfg-tele_interval_min').value = c.tele_interval_ms ? Math.round(parseInt(c.tele_interval_ms) / 60000) : '';
    $('cfg-lxmf_interval_min').value = c.lxmf_interval_ms ? Math.round(parseInt(c.lxmf_interval_ms) / 60000) : '';
    $('cfg-telemetry').checked      = c.telemetry === '1';
    $('cfg-lxmf').checked           = c.lxmf === '1';
    $('cfg-heartbeat').checked      = c.heartbeat === '1';
  }

  async function refreshAll() {
    try { await refreshStatus(); } catch (e) { log('err', 'status failed: ' + e.message); }
    try { await refreshConfig(); } catch (e) { log('err', 'config get failed: ' + e.message); }
  }

  $('btn-refresh').addEventListener('click', refreshAll);

  // Calibration --------------------------------------------------
  $('btn-calibrate').addEventListener('click', async () => {
    const mv = parseInt($('bat-measured').value, 10);
    if (!Number.isFinite(mv) || mv < 500 || mv > 10000) {
      log('err', 'enter a measured voltage in mV (500..10000)');
      return;
    }
    try {
      const result = await con.calibrateBattery(mv);
      log('ok', `staged batt_mult=${result.batt_mult} (commit to persist)`);
      // Reload the config form so the new multiplier is visible and
      // will be included in the next commit.
      await refreshConfig();
    } catch (e) {
      log('err', 'calibrate failed: ' + e.message);
    }
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
    };
  }

  $('btn-commit').addEventListener('click', async () => {
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
  function setLoadedPackage(pkg, label) {
    selectedPackage = pkg;
    const btnDfuEl = document.getElementById('btn-dfu');
    if (pkg) {
      const appSize  = pkg.firmware.length;
      const initSize = pkg.initPacket.length;
      fwInfo.textContent = `${label} — app ${appSize} B, init ${initSize} B`;
      btnFlash.disabled  = false;
      if (btnDfuEl) btnDfuEl.disabled = false;
      log('info', `package loaded (${label}): app=${appSize} bytes, init=${initSize} bytes`);
    } else {
      fwInfo.textContent = 'no firmware loaded';
      btnFlash.disabled  = true;
      if (btnDfuEl) btnDfuEl.disabled = true;
    }
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
