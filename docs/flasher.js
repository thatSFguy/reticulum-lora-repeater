// docs/flasher.js — step-wizard firmware flasher for the Reticulum LoRa
// Repeater web configurator. Modelled on the agnostic-lora-net hub: pick a
// board → reboot it into the bootloader automatically → flash → done.
//
// Bootloader entry is "offer both", most-reliable-first:
//   1. the firmware's own `DFU` console command (GPREGRET serial-DFU magic
//      + reset — no host DTR timing to get wrong), then
//   2. the 1200-baud touch as a fallback, then
//   3. manual double-tap of the reset button (always works; documented in
//      the UI), after which the user just clicks "select bootloader port".
//
// Firmware bytes come from the pre-extracted .dfu.json our CI emits
// (scripts/mk_dfu_json.py) — no in-browser unzip. Older releases that
// predate that asset fall back to fetching the .zip and unzipping with
// JSZip. The actual DFU wire protocol lives in dfu.js (window.RLRDfu).
//
// Web Serial note: navigator.serial.requestPort() needs transient user
// activation, which is consumed per click and gone after an await. So each
// port picker sits behind its own button: "Connect & flash" picks the app
// port (to reboot it), and "Select bootloader port & flash" picks the
// bootloader port. A board that's already in the bootloader skips straight
// to the single bootloader pick.

'use strict';

(function () {
  const $ = (id) => document.getElementById(id);
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

  // ---- tab switching (Flash | Configure) -------------------------------
  window.showTab = function (t) {
    for (const x of ['flash', 'configure']) {
      const sec = $('tab-' + x);
      const nav = $('nav-' + x);
      if (sec) sec.classList.toggle('hidden', x !== t);
      if (nav) nav.classList.toggle('on', x === t);
    }
  };

  // ---- environment gates ----------------------------------------------
  const hasSerial = 'serial' in navigator;

  // ---- flash log -------------------------------------------------------
  const flogEl = $('flash-log');
  function flog(kind, msg) {
    if (!flogEl) return;
    const span = document.createElement('span');
    span.className = kind;
    span.textContent = msg + '\n';
    flogEl.appendChild(span);
    flogEl.scrollTop = flogEl.scrollHeight;
    while (flogEl.childNodes.length > 400) flogEl.removeChild(flogEl.firstChild);
  }
  // dfu.js calls log(kind, msg); adapt its (stage) callbacks too.
  const dfuLog = (kind, msg) => flog(kind, msg);

  // ---- wizard step machine --------------------------------------------
  function setStep(n) {
    for (let i = 1; i <= 3; i++) {
      const ind = $('st-' + i);
      const card = $('flash-step' + i);
      if (ind) { ind.classList.toggle('on', i === n); ind.classList.toggle('done', i < n); }
      if (card) card.classList.toggle('hidden', i !== n);
    }
  }
  function setState(text, kind) {
    const el = $('fl-state');
    if (!el) return;
    el.textContent = text;
    el.className = 'pill' + (kind ? ' ' + kind : '');
  }
  function setProgress(pct) {
    const bar = $('fl-progress');
    if (bar) bar.style.width = pct + '%';
  }

  // ---- firmware manifest ----------------------------------------------
  const MANIFEST_URL = 'firmware/manifest.json';
  let releases = [];        // newest-first
  let localPkg = null;      // DfuPackage from a local .zip upload (overrides manifest)
  let currentUf2Path = null;

  const elVersion = $('fl-version');
  const elBoard = $('fl-board');
  const elRelStatus = $('fl-rel-status');

  async function fetchReleases() {
    if (!elVersion) return;
    elRelStatus.textContent = 'Loading firmware manifest…';
    try {
      const res = await fetch(MANIFEST_URL, { cache: 'no-cache' });
      if (!res.ok) throw new Error('HTTP ' + res.status);
      const manifest = await res.json();
      releases = (manifest.releases || []).filter((r) => (r.boards || []).length > 0);
      if (releases.length === 0) {
        elRelStatus.textContent = 'No published releases — use “upload a local firmware.zip” below.';
        elVersion.innerHTML = '<option>no releases</option>';
        return;
      }
      elVersion.innerHTML = '';
      for (const r of releases) {
        const opt = document.createElement('option');
        opt.value = r.tag;
        opt.textContent = r.prerelease ? r.tag + ' (prerelease)' : r.tag;
        elVersion.appendChild(opt);
      }
      const stable = releases.find((r) => !r.prerelease);
      elVersion.value = (stable || releases[0]).tag;
      elVersion.disabled = false;
      repopulateBoards();
      elRelStatus.textContent = 'Loaded ' + releases.length + ' release(s).';
    } catch (e) {
      elRelStatus.textContent = 'Could not load manifest: ' + e.message + ' — use a local upload instead.';
      elVersion.innerHTML = '<option>unavailable</option>';
    }
  }

  function repopulateBoards() {
    const r = releases.find((x) => x.tag === elVersion.value);
    elBoard.innerHTML = '';
    if (!r) { elBoard.disabled = true; return; }
    for (const b of r.boards) {
      const opt = document.createElement('option');
      opt.value = b.name;
      opt.textContent = b.name;
      elBoard.appendChild(opt);
    }
    elBoard.disabled = false;
  }

  function selectedRecord() {
    const r = releases.find((x) => x.tag === elVersion.value);
    if (!r) return null;
    return r.boards.find((b) => b.name === elBoard.value) || null;
  }

  // Fetch + parse the firmware for the current selection (or the local
  // upload). Prefers the pre-extracted .dfu.json; falls back to the .zip.
  async function loadFirmware() {
    if (localPkg) { currentUf2Path = null; return localPkg; }
    const rec = selectedRecord();
    if (!rec) throw new Error('no firmware selected');
    currentUf2Path = rec.uf2_path || null;

    if (rec.dfu_json_path) {
      setState('fetching firmware (.dfu.json)…');
      const res = await fetch(rec.dfu_json_path, { cache: 'no-cache' });
      if (!res.ok) throw new Error('HTTP ' + res.status + ' fetching .dfu.json');
      const obj = await res.json();
      flog('info', 'firmware ' + rec.name + ' ' + (rec.dfu_json_size || '') + 'B json, app ' + obj.app_size + 'B');
      return RLRDfu.DfuPackage.fromDfuJson(obj);
    }

    // Older release: stream the .zip and unzip in-browser.
    if (!rec.zip_path) throw new Error('release has no .dfu.json or .zip for this board');
    setState('fetching firmware (.zip)…');
    const res = await fetch(rec.zip_path);
    if (!res.ok) throw new Error('HTTP ' + res.status + ' fetching .zip');
    const buf = await res.arrayBuffer();
    flog('info', 'firmware ' + rec.name + ' ' + buf.byteLength + 'B zip (legacy path)');
    return RLRDfu.DfuPackage.fromArrayBuffer(buf);
  }

  // ---- re-enumeration watcher -----------------------------------------
  // After a reboot-to-bootloader, wait for the USB device to drop and/or a
  // new one to appear. Either is good news; a timeout just means we proceed
  // and let the user double-tap reset if needed.
  function waitForReenumeration(timeoutMs) {
    return new Promise((resolve) => {
      let done = false;
      const finish = (outcome) => {
        if (done) return;
        done = true;
        navigator.serial.removeEventListener('connect', onC);
        navigator.serial.removeEventListener('disconnect', onD);
        clearTimeout(timer);
        resolve(outcome);
      };
      const onC = () => finish('connected');
      const onD = () => finish('disconnected');
      navigator.serial.addEventListener('connect', onC);
      navigator.serial.addEventListener('disconnect', onD);
      const timer = setTimeout(() => finish('timeout'), timeoutMs);
    });
  }

  // ---- reboot the running app into its bootloader (offer both) ---------
  async function rebootToBootloader(appPort) {
    setState('rebooting into bootloader…');
    let rebooted = false;
    try {
      await appPort.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none' });
      flog('info', 'sending DFU command to the running firmware…');
      rebooted = await RLRDfu.dfuCommandReboot(appPort, { log: dfuLog });
    } catch (e) {
      flog('info', 'DFU command path unavailable: ' + e.message);
    } finally {
      try { await appPort.close(); } catch (e) {}
    }

    if (!rebooted) {
      // Firmware didn't acknowledge — maybe an older build without the DFU
      // command, or a noisy port. Fall back to the 1200-baud touch.
      flog('info', 'no DFU ack — trying the 1200-baud touch…');
      try {
        await RLRDfu.dfuTouch1200(appPort, { log: dfuLog });
        flog('ok', '1200-baud touch sent');
      } catch (e) {
        flog('err', '1200-baud touch failed: ' + e.message);
      }
    }

    const outcome = await waitForReenumeration(4000);
    if (outcome === 'connected') flog('ok', 'a new USB device appeared — bootloader is ready');
    else if (outcome === 'disconnected') flog('ok', 'board reset — give it a moment, then flash');
    else flog('info', 'no re-enumeration seen — if the board did not reboot, double-tap its reset button, then flash');
  }

  // ---- the actual flash over the bootloader port -----------------------
  async function doFlash(bootPort, pkg) {
    setStep(2);
    showUf2Fallback(false);
    setProgress(0);
    setState('flashing…');
    let ok = false;
    try {
      await bootPort.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none' });
      flog('info', '--- bootloader port opened ---');
      await RLRDfu.dfuFlash(bootPort, pkg, {
        onStage: (text) => { flog('info', '[dfu] ' + text); },
        onProgress: (sent, total) => {
          const pct = Math.floor((100 * sent) / total);
          setProgress(pct);
          setState('flashing… ' + pct + '%');
        },
        log: dfuLog,
      });
      ok = true;
    } catch (e) {
      flog('err', 'flash error: ' + e.message);
      setState('flash failed — ' + e.message, 'bad');
      showUf2Fallback(true);
    } finally {
      try { if (bootPort.readable) await bootPort.close(); } catch (e) {}
    }
    if (ok) {
      setProgress(100);
      setState('flashed ✓', 'ok');
      flog('ok', '--- flash complete — board rebooting into the new firmware ---');
      setStep(3);
    }
  }

  // ---- UF2 fallback panel ---------------------------------------------
  function showUf2Fallback(on) {
    const panel = $('fl-uf2-fallback');
    if (panel) panel.classList.toggle('hidden', !on);
    const dl = $('fl-dl-uf2');
    if (dl) dl.disabled = !currentUf2Path;
  }

  // ---- "Select bootloader port & flash" (second gesture) ---------------
  let pendingPkg = null;
  function armFlashNow(pkg) {
    pendingPkg = pkg;
    const btn = $('fl-flash-now');
    if (btn) { btn.classList.remove('hidden'); btn.disabled = false; }
    showUf2Fallback(true);  // also reveal manual double-tap + UF2 download
    setState('ready to flash — click “select bootloader port & flash”', 'warn');
  }

  // =====================================================================
  //  Button wiring
  // =====================================================================

  if (elVersion) elVersion.addEventListener('change', repopulateBoards);

  // Local firmware.zip upload (developers building their own).
  const elFile = $('fl-file');
  if (elFile) {
    elFile.addEventListener('change', async () => {
      const f = elFile.files && elFile.files[0];
      if (!f) { localPkg = null; return; }
      try {
        localPkg = await RLRDfu.DfuPackage.fromFile(f);
        elRelStatus.textContent = 'Using local file: ' + f.name + ' (app ' + localPkg.firmware.length + ' B)';
        flog('info', 'local package loaded: ' + f.name);
      } catch (e) {
        localPkg = null;
        elRelStatus.textContent = 'Local file error: ' + e.message;
        flog('err', 'local package parse failed: ' + e.message);
      }
    });
  }

  // Primary "Connect & flash" button.
  const btnStart = $('fl-start');
  if (btnStart) {
    btnStart.addEventListener('click', async () => {
      if (!hasSerial) { flog('err', 'Web Serial not supported — use Chrome/Edge, or download the UF2.'); return; }
      btnStart.disabled = true;
      setStep(2);
      showUf2Fallback(false);
      const flashNowBtn = $('fl-flash-now');
      if (flashNowBtn) flashNowBtn.classList.add('hidden');
      setProgress(0);

      const boardLabel = localPkg ? 'local firmware' : (elBoard ? elBoard.value : '');
      const nameEl = $('fl-board-name');
      if (nameEl) nameEl.textContent = boardLabel;

      const alreadyBoot = $('fl-isboot') && $('fl-isboot').checked;

      try {
        // Fetch + parse the firmware FIRST so the bootloader isn't sitting
        // idle (and possibly timing back out to the app) while we download.
        // This stays within the click's user-activation window for the
        // requestPort() call that follows.
        const pkg = await loadFirmware();

        if (alreadyBoot) {
          // Single gesture: the board is already in its bootloader, so the
          // first (and only) port pick is the bootloader port.
          setState('select the bootloader port…');
          const bootPort = await navigator.serial.requestPort();
          await doFlash(bootPort, pkg);
        } else {
          // Pick the RUNNING app port now (gesture), reboot it into the
          // bootloader, then arm the second-gesture flash button.
          setState('select the board’s current (app) port to reboot it…');
          let appPort;
          try {
            appPort = await navigator.serial.requestPort();
          } catch (e) {
            setStep(1); btnStart.disabled = false; return;  // user cancelled
          }
          await rebootToBootloader(appPort);
          armFlashNow(pkg);
        }
      } catch (e) {
        flog('err', e.message);
        setState('failed — ' + e.message, 'bad');
        showUf2Fallback(true);
      } finally {
        btnStart.disabled = false;
      }
    });
  }

  // Second gesture: pick the bootloader port and flash.
  const btnFlashNow = $('fl-flash-now');
  if (btnFlashNow) {
    btnFlashNow.addEventListener('click', async () => {
      if (!pendingPkg) return;
      btnFlashNow.disabled = true;
      try {
        setState('select the bootloader port…');
        const bootPort = await navigator.serial.requestPort();
        await doFlash(bootPort, pendingPkg);
      } catch (e) {
        flog('err', e.message);
        setState('failed — ' + e.message, 'bad');
        btnFlashNow.disabled = false;
      }
    });
  }

  // UF2 download (manual drag-drop fallback).
  const btnUf2 = $('fl-dl-uf2');
  if (btnUf2) {
    btnUf2.addEventListener('click', async () => {
      if (!currentUf2Path) { flog('err', 'no UF2 available for this selection'); return; }
      try {
        const res = await fetch(currentUf2Path);
        if (!res.ok) throw new Error('HTTP ' + res.status);
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = currentUf2Path.split('/').pop();
        a.click();
        URL.revokeObjectURL(url);
        flog('ok', 'downloaded ' + a.download + ' — double-tap reset, drag onto the bootloader USB drive');
      } catch (e) {
        flog('err', 'UF2 download failed: ' + e.message);
      }
    });
  }

  // Restart the wizard.
  const btnRestart = $('fl-restart');
  if (btnRestart) {
    btnRestart.addEventListener('click', () => {
      pendingPkg = null;
      const fn = $('fl-flash-now');
      if (fn) fn.classList.add('hidden');
      setStep(1);
      setState('');
    });
  }

  // Kick things off.
  setStep(1);
  fetchReleases();
})();
