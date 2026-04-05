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
    $('cfg-freq_hz').value          = c.freq_hz || '';
    $('cfg-bw_hz').value            = c.bw_hz || '';
    $('cfg-sf').value               = c.sf || '';
    $('cfg-cr').value               = c.cr || '';
    $('cfg-txp_dbm').value          = c.txp_dbm || '';
    $('cfg-tele_interval_ms').value = c.tele_interval_ms || '';
    $('cfg-lxmf_interval_ms').value = c.lxmf_interval_ms || '';
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
      freq_hz:          $('cfg-freq_hz').value,
      bw_hz:            $('cfg-bw_hz').value,
      sf:               $('cfg-sf').value,
      cr:               $('cfg-cr').value,
      txp_dbm:          $('cfg-txp_dbm').value,
      tele_interval_ms: $('cfg-tele_interval_ms').value,
      lxmf_interval_ms: $('cfg-lxmf_interval_ms').value,
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
  // place and the two sources can't drift.
  function setLoadedPackage(pkg, label) {
    selectedPackage = pkg;
    if (pkg) {
      const appSize  = pkg.firmware.length;
      const initSize = pkg.initPacket.length;
      fwInfo.textContent = `${label} — app ${appSize} B, init ${initSize} B`;
      btnFlash.disabled  = false;
      log('info', `package loaded (${label}): app=${appSize} bytes, init=${initSize} bytes`);
    } else {
      fwInfo.textContent = 'no firmware loaded';
      btnFlash.disabled  = true;
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
  //  GitHub Releases integration
  // ---------------------------------------------------------------
  //
  // On page load we hit the public Releases API and populate the
  // version + board dropdowns from whatever's there. Asset filename
  // convention matches the CI workflow's staging step:
  //
  //   reticulum-lora-repeater-<board>-<tag>.zip
  //
  // so parsing "board" out of an asset name is a single regex. Both
  // version and board dropdowns are selection-driven: changing the
  // version repopulates the board list; clicking Download fetches
  // the selected asset with progress and hands the bytes to the
  // shared setLoadedPackage() path.

  const REPO_OWNER = 'thatSFguy';
  const REPO_NAME  = 'reticulum-lora-repeater';
  const RELEASE_API = `https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/releases`;
  const ASSET_NAME_RE = /^reticulum-lora-repeater-([^-]+)-(v.+)\.zip$/;

  let releases = [];  // parsed API response, filtered to those with at least one matching .zip

  async function fetchReleases() {
    relStatus.textContent = 'Fetching releases from GitHub…';
    try {
      const res = await fetch(RELEASE_API, { headers: { 'Accept': 'application/vnd.github+json' } });
      if (!res.ok) throw new Error(`HTTP ${res.status}`);
      const raw = await res.json();

      // Keep only releases that actually have a flashable asset for
      // at least one board. Drafts are hidden from unauthenticated
      // callers automatically, so no extra filter needed there.
      releases = raw
        .map(r => {
          const assets = (r.assets || [])
            .map(a => {
              const m = ASSET_NAME_RE.exec(a.name);
              return m ? { name: a.name, url: a.browser_download_url, size: a.size, board: m[1] } : null;
            })
            .filter(Boolean);
          return { tag: r.tag_name, name: r.name, prerelease: r.prerelease, published: r.published_at, assets };
        })
        .filter(r => r.assets.length > 0);

      if (releases.length === 0) {
        relStatus.textContent = 'No flashable releases found on this repo.';
        relVersion.innerHTML = '<option>no releases</option>';
        return;
      }

      // Populate version dropdown, newest first. Prefer non-prereleases
      // at the top but keep them all in the list so testers can grab
      // -rc / -test builds without switching flags.
      releases.sort((a, b) => (b.published || '').localeCompare(a.published || ''));
      relVersion.innerHTML = '';
      for (const r of releases) {
        const opt = document.createElement('option');
        opt.value = r.tag;
        const date = (r.published || '').slice(0, 10);
        opt.textContent = r.prerelease ? `${r.tag} (prerelease, ${date})` : `${r.tag} (${date})`;
        relVersion.appendChild(opt);
      }

      // Default selection: the newest non-prerelease if any exist,
      // otherwise just the newest overall.
      const stable = releases.find(r => !r.prerelease);
      relVersion.value = (stable || releases[0]).tag;
      relVersion.disabled = false;
      repopulateBoardDropdown();
      relStatus.textContent = `Loaded ${releases.length} release(s).`;
    } catch (e) {
      relStatus.textContent = 'Could not fetch releases: ' + e.message + ' — use local upload instead.';
      log('err', 'release fetch failed: ' + e.message);
      relVersion.innerHTML = '<option>unavailable</option>';
    }
  }

  function repopulateBoardDropdown() {
    const r = releases.find(x => x.tag === relVersion.value);
    relBoard.innerHTML = '';
    if (!r) { relBoard.disabled = true; btnLoadRelease.disabled = true; return; }
    for (const a of r.assets) {
      const opt = document.createElement('option');
      opt.value = a.board;
      opt.textContent = a.board;
      relBoard.appendChild(opt);
    }
    relBoard.disabled       = false;
    btnLoadRelease.disabled = false;
  }

  relVersion.addEventListener('change', repopulateBoardDropdown);

  btnLoadRelease.addEventListener('click', async () => {
    const r = releases.find(x => x.tag === relVersion.value);
    if (!r) return;
    const asset = r.assets.find(a => a.board === relBoard.value);
    if (!asset) return;

    btnLoadRelease.disabled = true;
    relStatus.textContent = `Downloading ${asset.name}…`;
    try {
      const res = await fetch(asset.url);
      if (!res.ok) throw new Error(`HTTP ${res.status}`);

      // Stream the response so we can show download progress.
      // Content-Length may be absent after the S3 redirect on some
      // CDNs — we fall back to an indeterminate byte counter in
      // that case rather than failing.
      const total   = parseInt(res.headers.get('Content-Length') || '0', 10) || asset.size || 0;
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
          relStatus.textContent = `Downloading ${asset.name}: ${received} / ${total} B (${pct}%)`;
        } else {
          relStatus.textContent = `Downloading ${asset.name}: ${received} B`;
        }
      }

      const buf = new Uint8Array(received);
      let offset = 0;
      for (const c of chunks) { buf.set(c, offset); offset += c.length; }

      const pkg = await RLRDfu.DfuPackage.fromArrayBuffer(buf.buffer);
      setLoadedPackage(pkg, `${asset.board} ${r.tag}`);
      relStatus.textContent = `Loaded ${asset.name} (${received} B).`;
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
