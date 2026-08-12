#pragma once

#include <Arduino.h>

// Mobile-first control page served directly by the touchscreen ESP32.
// Everything is self-contained so the phone never needs internet access.
const char PHONE_CONTROL_PAGE_HTML[] PROGMEM = R"PHONEPAGE(
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="theme-color" content="#06131e">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <meta name="apple-mobile-web-app-title" content="Weighing Scale">
  <title>Dual Load-Cell Control</title>
  <style>
    :root {
      color-scheme:dark;
      --bg:#06131e; --panel:#0b1f2b; --panel2:#102a38;
      --line:#29495d; --text:#f4f8fb; --muted:#8aa1b2;
      --cyan:#159cf3; --cyan2:#55c7ff; --green:#3edc73;
      --orange:#f2b435; --red:#f04444;
    }
    * { box-sizing:border-box; -webkit-tap-highlight-color:transparent; }
    body {
      margin:0; min-height:100vh; background:var(--bg);
      color:var(--text); font:14px/1.4 system-ui,-apple-system,Segoe UI,sans-serif;
    }
    .wrap { width:min(980px,100%); margin:auto; padding:0 12px 34px; }
    header { display:flex; justify-content:space-between; gap:12px; align-items:center; min-height:70px; border-bottom:1px solid var(--line); padding:12px 2px; }
    h1 { font-size:1.12rem; margin:0 0 2px; letter-spacing:.08em; }
    h2 { font-size:.88rem; margin:0 0 10px; letter-spacing:.07em; }
    .sub,.small { color:var(--muted); font-size:.82rem; }
    .connection { display:flex; align-items:center; gap:7px; white-space:nowrap; color:var(--muted); font-size:.82rem; }
    .dot { width:9px; height:9px; border-radius:50%; background:var(--orange); }
    .dot.ok { background:var(--green); color:var(--green); }
    .dot.bad { background:var(--red); color:var(--red); }
    .card { background:var(--panel); border:1px solid var(--line); border-radius:8px; padding:14px; }
    .total { border-color:var(--cyan); margin:14px 0 10px; text-align:center; }
    .eyebrow { color:var(--muted); font-size:.76rem; letter-spacing:.13em; text-transform:uppercase; }
    .bigweight { color:var(--cyan2); font-size:clamp(3.5rem,18vw,5.3rem); font-weight:720; line-height:1; letter-spacing:-.05em; margin:10px 0 7px; font-variant-numeric:tabular-nums; }
    .unit { color:var(--muted); font-size:1rem; margin-left:5px; letter-spacing:0; }
    .row { display:flex; align-items:center; justify-content:space-between; gap:9px; }
    .grid { display:grid; gap:12px; }
    .scales { grid-template-columns:minmax(0,1fr) minmax(0,1fr); margin-bottom:10px; gap:10px; }
    .scale-card { border-width:1px; min-width:0; }
    .scale-weight { color:var(--cyan2); font-size:clamp(1.75rem,9vw,2.7rem); font-weight:680; line-height:1; margin:12px 0; font-variant-numeric:tabular-nums; }
    .pill { border:1px solid var(--line); border-radius:5px; padding:3px 7px; color:var(--muted); font-size:.68rem; font-weight:750; letter-spacing:.04em; }
    .pill.good { color:var(--green); border-color:var(--green); }
    .pill.warn { color:var(--orange); border-color:var(--orange); }
    .pill.bad { color:var(--red); border-color:var(--red); }
    .pill.locked { color:var(--cyan); border-color:var(--cyan); }
    dl { display:grid; grid-template-columns:1fr auto; gap:6px 8px; margin:10px 0 13px; padding-top:10px; border-top:1px solid var(--line); font-size:.77rem; }
    dt { color:var(--muted); } dd { margin:0; text-align:right; }
    .controls { display:grid; grid-template-columns:minmax(0,1fr) minmax(0,1fr); gap:9px; }
    button,input,textarea { font:inherit; min-width:0; max-width:100%; }
    button {
      min-height:46px; border-radius:6px; border:1px solid var(--cyan);
      min-width:0; background:#0c6fc4; color:var(--text); font-weight:750; padding:8px 7px; cursor:pointer; letter-spacing:.025em; overflow-wrap:anywhere;
    }
    button.secondary { border-color:var(--line); background:transparent; }
    button.warning { border-color:var(--orange); background:#4b3510; }
    button:disabled { opacity:.38; cursor:not-allowed; }
    .wide { width:100%; }
    .actions { grid-template-columns:repeat(2,minmax(0,1fr)); margin-bottom:12px; }
    .section-grid { grid-template-columns:1fr; margin-bottom:12px; }
    .cal-row { display:grid; grid-template-columns:1fr auto; gap:9px; align-items:end; margin-top:10px; }
    label { display:block; color:var(--muted); font-size:.8rem; }
    input[type=number],input[type=text],input[type=search],textarea {
      width:100%; min-height:44px; margin-top:5px; border-radius:5px; border:1px solid var(--line);
      background:var(--bg); color:var(--text); padding:8px 10px; font-size:1rem;
    }
    textarea { min-height:112px; resize:vertical; }
    input[type=range] { width:100%; accent-color:var(--cyan); }
    .settings-row { display:grid; grid-template-columns:1fr auto; align-items:center; gap:14px; margin:17px 0; }
    .cpu { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
    .mode-controls { display:grid; grid-template-columns:repeat(3,1fr); gap:7px; }
    #message { display:none; position:sticky; bottom:12px; z-index:4; margin-top:12px; padding:12px 14px; border-radius:11px; background:#163943; border:1px solid var(--cyan); box-shadow:0 8px 28px #0008; }
    #message.bad { border-color:var(--red); color:#ffd9dc; }
    .tabs { position:sticky; bottom:0; z-index:8; display:grid; grid-template-columns:repeat(4,1fr); gap:0; margin:16px -12px 0; border-top:1px solid var(--line); background:#071722f5; padding-bottom:env(safe-area-inset-bottom); }
    .tabs button { min-height:59px; border:0; border-radius:0; background:transparent; color:var(--muted); padding:7px 2px; font-size:.72rem; }
    .tabs button.active { color:var(--cyan2); border-top:2px solid var(--cyan); }
    .tabs button::before { display:block; font-size:1.1rem; line-height:1.2; margin-bottom:3px; }
    #controlTab::before { content:'\2696'; } #historyTab::before { content:'\2637'; }
    #diagnosticsTab::before { content:'\223F'; } #settingsTab::before { content:'\2699'; }
    .hidden { display:none!important; }
    .history-tools { display:grid; grid-template-columns:1fr auto; gap:8px; margin:10px 0; }
    .history-list { display:grid; gap:9px; margin:12px 0; }
    .history-item { background:var(--panel); border:1px solid var(--line); border-radius:7px; padding:12px; }
    .history-name { font-weight:750; overflow-wrap:anywhere; }
    .history-meta { color:var(--muted); font-size:.78rem; margin-top:4px; }
    .history-weight { color:var(--cyan); font-size:1.35rem; font-weight:750; }
    .history-footer { display:grid; grid-template-columns:1fr 1fr; gap:8px; }
    .danger { border-color:var(--red)!important; color:#ffd9dc; }
    .modal { position:fixed; inset:0; z-index:20; display:grid; place-items:center; padding:14px; background:#000b; }
    .dialog { width:min(560px,100%); max-height:92vh; overflow:auto; background:var(--panel); border:1px solid var(--cyan); border-radius:8px; padding:16px; box-shadow:0 20px 60px #000b; }
    .dialog-actions { display:grid; grid-template-columns:1fr 1fr; gap:8px; margin-top:14px; }
    .detail-grid { display:grid; grid-template-columns:1fr auto; gap:7px 12px; margin:12px 0; font-size:.84rem; }
    .detail-grid span:nth-child(odd) { color:var(--muted); }
    .detail-grid span:nth-child(even) { text-align:right; overflow-wrap:anywhere; }
    .view-title { display:flex; justify-content:space-between; align-items:end; gap:8px; margin:16px 2px 10px; }
    .view-title h2 { margin:0; font-size:1.05rem; }
    .diagnostic-heading { margin:14px 2px 10px; }
    footer { text-align:center; color:var(--muted); margin:18px 0 4px; font-size:.72rem; }
    body[data-view="diagnostics"] .dashboard-only { display:none!important; }
    body:not([data-view="diagnostics"]) .diagnostics-only { display:none!important; }
    body[data-view="diagnostics"] .scale-control { display:none!important; }
    body[data-view="settings"] .dashboard-only,
    body[data-view="settings"] .scales { display:none!important; }
    @media (min-width:720px) {
      .scales,.section-grid { grid-template-columns:1fr 1fr; }
      .wrap { padding-top:8px; }
      .tabs { position:static; margin:16px 0 0; border:1px solid var(--line); border-radius:7px; overflow:hidden; }
      .tabs button { min-height:48px; }
      .tabs button::before { display:inline; margin-right:6px; }
    }
    @media (max-width:370px) {
      .scales { grid-template-columns:1fr; }
      .bigweight { font-size:3.7rem; }
    }
    @media (max-width:520px) {
      .scales { grid-template-columns:calc((100vw - 34px)/2) calc((100vw - 34px)/2); }
      .scale-card { padding:12px 10px; }
      .scale-card .row { align-items:flex-start; flex-direction:column; }
      .scale-card .controls { grid-template-columns:1fr; }
      .scale-card button { min-height:42px; }
      .connection { font-size:0; }
    }
    @media (orientation:landscape) and (max-height:520px) {
      .wrap { width:min(1100px,100%); }
      header { min-height:54px; }
      .total { float:left; width:34%; margin-right:10px; }
      .scales { grid-template-columns:1fr 1fr; }
      .actions { clear:both; grid-template-columns:repeat(4,1fr); }
    }
  </style>
</head>
<body>
  <main class="wrap">
    <header>
      <div><h1>DUAL LOAD CELL SCALE</h1><div class="sub">School of Engineering &bull; Temasek Polytechnic</div></div>
      <div class="connection"><span id="netDot" class="dot"></span><span id="netText">Connecting...</span></div>
    </header>

    <div id="controlView">

    <section class="card total dashboard-only">
      <div class="row"><span class="eyebrow">Total</span><span id="lockPill" class="pill warn">WAITING</span></div>
      <div id="totalWeight" class="bigweight">--.--<span class="unit">kg</span></div>
      <div id="combinedMode" class="small">Waiting for scale data</div>
    </section>

    <section class="grid scales">
      <article class="card scale-card" id="cardA">
        <div class="row"><h2>LOAD CELL A</h2><span id="statusA" class="pill warn">SEARCHING</span></div>
        <div id="weightA" class="scale-weight">--.-- <span class="unit">kg</span></div>
        <dl>
          <dt>Bluetooth</dt><dd id="linkA">--</dd>
          <dt>Load-cell ADC</dt><dd id="adcA">--</dd>
          <dt>OLED</dt><dd id="oledA">--</dd>
          <dt>Weight data</dt><dd id="dataA">--</dd>
          <dt>Data age</dt><dd id="ageA">--</dd>
          <dt>Command</dt><dd id="pendingA">Idle</dd>
        </dl>
        <div class="controls scale-control"><button id="tareA" onclick="tare('a')">TARE A</button><button id="calA" class="warning" onclick="calibrate('a')">CALIBRATE A</button></div>
        <label class="scale-control" style="margin-top:10px">Reference weight (kg)<input id="kgA" type="number" min="0.1" max="100" step="0.1" value="1.0" inputmode="decimal"></label>
      </article>

      <article class="card scale-card" id="cardB">
        <div class="row"><h2>LOAD CELL B</h2><span id="statusB" class="pill warn">SEARCHING</span></div>
        <div id="weightB" class="scale-weight">--.-- <span class="unit">kg</span></div>
        <dl>
          <dt>Bluetooth</dt><dd id="linkB">--</dd>
          <dt>Load-cell ADC</dt><dd id="adcB">--</dd>
          <dt>OLED</dt><dd id="oledB">--</dd>
          <dt>Weight data</dt><dd id="dataB">--</dd>
          <dt>Data age</dt><dd id="ageB">--</dd>
          <dt>Command</dt><dd id="pendingB">Idle</dd>
        </dl>
        <div class="controls scale-control"><button id="tareB" onclick="tare('b')">TARE B</button><button id="calB" class="warning" onclick="calibrate('b')">CALIBRATE B</button></div>
        <label class="scale-control" style="margin-top:10px">Reference weight (kg)<input id="kgB" type="number" min="0.1" max="100" step="0.1" value="1.0" inputmode="decimal"></label>
      </article>
    </section>

    <section class="grid actions dashboard-only">
      <button id="tareAll" onclick="tare('all')">TARE BOTH</button>
      <button id="unlock" class="warning" onclick="unlockWeight()">UNLOCK</button>
      <button class="secondary" onclick="checkLinks()">CHECK LINKS</button>
      <button id="saveLog" onclick="openSaveLog()">SAVE LOG</button>
    </section>

    <section id="settingsPanel" class="grid section-grid hidden">
      <div class="view-title"><h2>SETTINGS</h2><span class="small">Display master</span></div>
      <article class="card">
        <h2>DISPLAY BRIGHTNESS</h2>
        <div class="settings-row"><input id="brightness" type="range" min="10" max="100" step="10" value="100" oninput="byId('brightnessText').textContent=this.value+'%'" onchange="setBrightness(this.value)"><strong id="brightnessText">100%</strong></div>
        <div class="small">Lower brightness reduces display heat and power use.</div>
      </article>
      <article class="card">
        <h2>ESP32 CPU CLOCK</h2>
        <div class="cpu"><button id="cpu160" class="secondary" onclick="setCpu(160)">160 MHz</button><button id="cpu240" class="secondary" onclick="setCpu(240)">240 MHz</button></div>
        <div class="small" style="margin-top:11px">160 MHz generally runs cooler while keeping weighing active.</div>
      </article>
      <article class="card">
        <h2>OPERATING MODE</h2>
        <div class="mode-controls"><button id="modeLocal" class="secondary" onclick="setOperatingMode('local')">LOCAL</button><button id="modeWeb" class="secondary" onclick="setOperatingMode('web')">WEB</button><button id="modeHybrid" class="secondary" onclick="setOperatingMode('hybrid')">HYBRID</button></div>
        <div id="modeText" class="small" style="margin-top:11px">Hybrid keeps touchscreen and phone control active.</div>
      </article>
    </section>
    </div>

    <section id="historyView" class="hidden">
      <div class="view-title"><div><h2>WEIGHT HISTORY</h2><div id="historyCount" class="small">0 of 100 entries</div></div><button class="secondary" onclick="loadHistory()">REFRESH</button></div>
      <div class="card">
      <div class="history-tools"><input id="historySearch" type="search" maxlength="50" placeholder="Search ID or name" onkeydown="if(event.key==='Enter')searchHistory()"><button onclick="searchHistory()">SEARCH</button></div>
      <div class="controls"><button class="secondary" onclick="exportHistory()">EXPORT CSV</button><button class="danger" onclick="clearHistory()">CLEAR ALL</button></div>
      <div id="historyList" class="history-list"><div class="small">Open History to load saved entries.</div></div>
      <div class="history-footer"><button id="historyPrevious" class="secondary" onclick="changeHistoryPage(-1)">PREVIOUS</button><button id="historyNext" class="secondary" onclick="changeHistoryPage(1)">NEXT</button></div>
      </div>
    </section>

    <nav class="tabs">
      <button id="controlTab" class="active" onclick="switchView('control')">SCALE</button>
      <button id="historyTab" onclick="switchView('history')">HISTORY (0)</button>
      <button id="diagnosticsTab" onclick="switchView('diagnostics')">DIAGNOSTICS</button>
      <button id="settingsTab" onclick="switchView('settings')">SETTINGS</button>
    </nav>

    <div id="message"></div>
    <footer>Connect directly to WEIGHING_SCALE &bull; no internet required</footer>
  </main>

  <div id="saveModal" class="modal hidden">
    <section class="dialog" role="dialog" aria-modal="true" aria-labelledby="saveTitle">
      <div class="row"><h2 id="saveTitle">SAVE LOCKED READING</h2><button class="secondary" onclick="closeModal('saveModal')">CLOSE</button></div>
      <div id="savePreview" class="card total small"></div>
      <label>Name<input id="logName" type="text" maxlength="50" placeholder="e.g. Batch 24" autocomplete="off"></label>
      <label style="margin-top:10px">Notes<textarea id="logNotes" maxlength="500" placeholder="Add details about this reading"></textarea></label>
      <div id="saveTime" class="small" style="margin-top:9px"></div>
      <div class="dialog-actions"><button class="secondary" onclick="closeModal('saveModal')">CANCEL</button><button id="confirmSaveLog" onclick="saveHistoryLog()">SAVE LOG</button></div>
    </section>
  </div>

  <div id="detailModal" class="modal hidden">
    <section class="dialog" role="dialog" aria-modal="true" aria-labelledby="detailTitle">
      <div class="row"><div><h2 id="detailTitle">LOG DETAILS</h2><div id="detailId" class="pill locked"></div></div><button class="secondary" onclick="closeModal('detailModal')">CLOSE</button></div>
      <div id="detailGrid" class="detail-grid"></div>
      <label>Name<input id="detailName" type="text" maxlength="50" autocomplete="off"></label>
      <label style="margin-top:10px">Notes<textarea id="detailNotes" maxlength="500"></textarea></label>
      <div class="dialog-actions"><button class="danger" onclick="deleteHistoryEntry()">DELETE</button><button onclick="updateHistoryEntry()">SAVE CHANGES</button></div>
    </section>
  </div>

  <script>
    const byId = id => document.getElementById(id);
    let currentState = null;
    let requestRunning = false;
    let messageTimer = 0;
    let lastMasterNotice = '';
    let currentView = 'control';
    let historyOffset = 0;
    const historyLimit = 10;
    let historyHasMore = false;
    let historyQuery = '';
    let activeDetail = null;
    let saveLockToken = 0;

    function weightText(grams, valid) {
      if (!valid || grams === null || grams === undefined) return '--.-- <span class="unit">kg</span>';
      return (grams / 1000).toFixed(2) + '<span class="unit">kg</span>';
    }
    function pillClass(text, connected, usable) {
      if (text === 'LOCKED') return 'pill locked';
      if (usable || text === 'READY') return 'pill good';
      if (!connected || /ERROR|NO SENSOR|STALE/.test(text)) return 'pill bad';
      return 'pill warn';
    }
    function renderScale(scale, suffix) {
      byId('weight' + suffix).innerHTML = weightText(scale.weightGrams, scale.weightValid);
      const status = byId('status' + suffix);
      status.textContent = scale.system;
      status.className = pillClass(scale.system, scale.connected, scale.usable);
      byId('link' + suffix).textContent = scale.link;
      byId('adc' + suffix).textContent = scale.adc;
      byId('oled' + suffix).textContent = scale.oled;
      byId('data' + suffix).textContent = scale.data;
      byId('age' + suffix).textContent = scale.age;
      byId('pending' + suffix).textContent = scale.pending;
      byId('tare' + suffix).disabled = requestRunning || !scale.canTare;
      byId('cal' + suffix).disabled = requestRunning || !scale.canCalibrate;
    }
    function render(s) {
      currentState = s;
      byId('totalWeight').innerHTML = weightText(s.totalGrams, s.totalValid);
      byId('combinedMode').textContent = s.modeLabel;
      const lock = byId('lockPill');
      lock.textContent = s.lock;
      lock.className = s.lock === 'LOCKED' ? 'pill locked' : (s.lock === 'LOCKING' ? 'pill warn' : 'pill good');
      renderScale(s.scales[0], 'A');
      renderScale(s.scales[1], 'B');
      byId('tareAll').disabled = requestRunning || !s.canTareBoth;
      byId('unlock').disabled = requestRunning || s.lock === 'LIVE';
      const history = s.history || {ready:false,count:0,capacity:100,canSaveCurrentLock:false};
      byId('historyTab').textContent = 'HISTORY (' + history.count + ')';
      byId('historyTab').disabled = !history.ready;
      byId('saveLog').disabled = requestRunning || !history.canSaveCurrentLock;
      const operatingMode = s.operatingMode || 'hybrid';
      const brightness = byId('brightness');
      if (document.activeElement !== brightness) {
        brightness.value = s.display.brightness;
        const effectiveBrightness = s.display.effectiveBrightness ?? s.display.brightness;
        byId('brightnessText').textContent =
          operatingMode === 'web' && effectiveBrightness !== s.display.brightness
            ? effectiveBrightness + '% active (' + s.display.brightness + '% saved)'
            : s.display.brightness + '%';
      }
      brightness.disabled = requestRunning;
      byId('cpu160').className = s.display.cpuMHz === 160 ? '' : 'secondary';
      byId('cpu240').className = s.display.cpuMHz === 240 ? '' : 'secondary';
      byId('cpu160').disabled = requestRunning;
      byId('cpu240').disabled = requestRunning;
      ['local','web','hybrid'].forEach(mode => {
        const button = byId('mode' + mode.charAt(0).toUpperCase() + mode.slice(1));
        button.className = operatingMode === mode ? '' : 'secondary';
        button.disabled = requestRunning;
      });
      byId('modeText').textContent =
        operatingMode === 'local'
          ? 'Touchscreen control only. Wi-Fi is off for lower heat and power.'
          : operatingMode === 'web'
              ? 'Phone control active. The status-only touchscreen is capped at 20% brightness.'
              : 'Touchscreen and phone controls are both active.';
      if (s.notice && s.notice !== lastMasterNotice) {
        showMessage(s.notice, /failed|error|timed out|not connected|busy/i.test(s.notice));
      }
      lastMasterNotice = s.notice || '';
    }
    function networkState(ok, text) {
      byId('netDot').className = ok ? 'dot ok' : 'dot bad';
      byId('netText').textContent = text;
    }
    function showMessage(text, bad=false) {
      clearTimeout(messageTimer);
      const el = byId('message');
      el.textContent = text;
      el.className = bad ? 'bad' : '';
      el.style.display = 'block';
      messageTimer = setTimeout(() => el.style.display = 'none', 4500);
    }
    async function refresh() {
      try {
        const response = await fetch('/api/v1/status', {cache:'no-store'});
        if (!response.ok) throw new Error('Status ' + response.status);
        const state = await response.json();
        networkState(true, state.wifi.clients + ' phone' + (state.wifi.clients === 1 ? '' : 's'));
        render(state);
      } catch (error) {
        networkState(false, 'Connection lost');
      }
    }
    async function post(path, values={}, confirmation='', refreshAfter=true) {
      if (requestRunning) return {ok:false,cancelled:true};
      if (confirmation && !confirm(confirmation)) return {ok:false,cancelled:true};
      requestRunning = true;
      if (currentState) render(currentState);
      let outcome = {ok:false};
      try {
        const response = await fetch(path, {
          method:'POST',
          headers:{'Content-Type':'application/x-www-form-urlencoded','X-Scale-Control':'1'},
          body:new URLSearchParams(values)
        });
        const result = await response.json().catch(() => ({message:'Invalid response'}));
        showMessage(result.message || (response.ok ? 'Command accepted' : 'Command failed'), !response.ok);
        outcome = {ok:response.ok,result};
      } catch (error) {
        showMessage('Touchscreen connection lost', true);
        outcome = {ok:false,error};
      } finally {
        requestRunning = false;
        if (refreshAfter) await refresh();
        else if (currentState) render(currentState);
      }
      return outcome;
    }
    function tare(target) {
      const name = target === 'all' ? 'both scales' : 'Load Cell ' + target.toUpperCase();
      post('/api/v1/tare', {target}, 'Remove all weight from ' + name + ' before taring. Continue?');
    }
    function calibrate(target) {
      const input = byId('kg' + target.toUpperCase());
      const kg = Number(input.value);
      if (!Number.isFinite(kg) || kg < 0.1 || kg > 100) {
        showMessage('Reference weight must be between 0.1 and 100 kg', true); return;
      }
      const name = 'Load Cell ' + target.toUpperCase();
      post('/api/v1/calibrate', {target, kg:kg.toFixed(3)}, name + ' must be tared empty first. Place exactly ' + kg.toFixed(1) + ' kg on it, keep it still, then press OK.');
    }
    function unlockWeight() { post('/api/v1/unlock'); }
    function checkLinks() { post('/api/v1/reconnect', {target:'all'}); }
    function setBrightness(percent) { post('/api/v1/display/brightness', {percent}); }
    function setCpu(mhz) { post('/api/v1/display/cpu', {mhz}); }
    async function setOperatingMode(mode) {
      const local = mode === 'local';
      const confirmation = local
        ? 'Local mode turns off this Wi-Fi connection. Continue?'
        : 'Switch the touchscreen to ' + mode.toUpperCase() + ' mode?';
      const outcome = await post('/api/v1/mode', {mode}, confirmation, !local);
      if (outcome.ok && local) {
        networkState(false, 'Local mode - Wi-Fi off');
        showMessage('Local mode enabled. This Wi-Fi network will now close.');
      }
    }

    function switchView(view) {
      currentView = view;
      document.body.dataset.view = view;
      const controlVisible = view === 'control' || view === 'diagnostics' || view === 'settings';
      byId('controlView').classList.toggle('hidden', !controlVisible);
      byId('historyView').classList.toggle('hidden', view !== 'history');
      byId('settingsPanel').classList.toggle('hidden', view !== 'settings');
      ['control','history','diagnostics','settings'].forEach(name => {
        byId(name + 'Tab').classList.toggle('active', view === name);
      });
      if (view === 'history') loadHistory(true);
    }
    function closeModal(id) { byId(id).classList.add('hidden'); }
    function plainKg(grams) {
      return grams === null || grams === undefined ? 'Not included' : (grams / 1000).toFixed(2) + ' kg';
    }
    function utf8Length(value) {
      return window.TextEncoder ? new TextEncoder().encode(value).length : unescape(encodeURIComponent(value)).length;
    }
    function defaultLogName(date) {
      const p = number => String(number).padStart(2, '0');
      return 'Reading ' + date.getFullYear() + '-' + p(date.getMonth()+1) + '-' + p(date.getDate()) + ' ' + p(date.getHours()) + ':' + p(date.getMinutes()) + ':' + p(date.getSeconds());
    }
    function openSaveLog() {
      if (!currentState || !currentState.history || !currentState.history.canSaveCurrentLock) {
        showMessage('Wait for a new locked weight before saving', true); return;
      }
      saveLockToken = currentState.history.lockToken;
      const now = new Date();
      byId('logName').value = defaultLogName(now);
      byId('logNotes').value = '';
      const a = currentState.scales[0];
      const b = currentState.scales[1];
      byId('savePreview').textContent = 'Total ' + plainKg(currentState.totalGrams) + ' • A ' + plainKg(a.weightValid ? a.weightGrams : null) + ' • B ' + plainKg(b.weightValid ? b.weightGrams : null);
      byId('saveTime').textContent = 'Date and time from this iPhone: ' + now.toLocaleString();
      byId('saveModal').classList.remove('hidden');
      byId('logName').focus();
    }
    async function saveHistoryLog() {
      const name = byId('logName').value.trim();
      const notes = byId('logNotes').value.trim();
      if (!name || utf8Length(name) > 50 || utf8Length(notes) > 500) {
        showMessage('Name or notes are too long', true); return;
      }
      let timezone = 'Unknown';
      try { timezone = Intl.DateTimeFormat().resolvedOptions().timeZone || 'Unknown'; } catch (_) {}
      const now = new Date();
      const outcome = await post('/api/v1/history/save', {
        lockToken:String(saveLockToken), name, notes,
        timestampMs:String(now.getTime()),
        timezoneOffsetMinutes:String(now.getTimezoneOffset()), timezone
      });
      if (outcome.ok) {
        closeModal('saveModal');
        switchView('history');
      }
    }
    function formatLogTime(record) {
      const date = new Date(Number(record.loggedAtMs));
      try {
        if (record.timezone && record.timezone !== 'Unknown') {
          return new Intl.DateTimeFormat(undefined, {dateStyle:'medium',timeStyle:'medium',timeZone:record.timezone}).format(date) + ' (' + record.timezone + ')';
        }
      } catch (_) {}
      const local = new Date(Number(record.loggedAtMs) - Number(record.timezoneOffsetMinutes || 0) * 60000);
      return local.toISOString().replace('T',' ').replace('.000Z','') + ' local';
    }
    async function loadHistory(reset=false) {
      if (reset) historyOffset = 0;
      const list = byId('historyList');
      list.textContent = 'Loading history...';
      try {
        const query = new URLSearchParams({offset:String(historyOffset),limit:String(historyLimit),q:historyQuery});
        const response = await fetch('/api/v1/history?' + query, {cache:'no-store'});
        const data = await response.json();
        if (!response.ok) throw new Error(data.message || 'History failed');
        historyHasMore = data.hasMore;
        byId('historyCount').textContent = data.matchingCount + (historyQuery ? ' matching • ' : ' of ') + data.count + ' saved • capacity ' + data.capacity;
        list.textContent = '';
        if (!data.records.length) {
          const empty = document.createElement('div'); empty.className = 'small';
          empty.textContent = historyQuery ? 'No matching entries.' : 'No history entries yet. Lock a weight and press SAVE LOG.';
          list.appendChild(empty);
        }
        data.records.forEach(record => {
          const item = document.createElement('article'); item.className = 'history-item';
          const top = document.createElement('div'); top.className = 'row';
          const title = document.createElement('div');
          const name = document.createElement('div'); name.className = 'history-name'; name.textContent = record.name;
          const id = document.createElement('div'); id.className = 'history-meta'; id.textContent = record.displayId + ' • ' + formatLogTime(record);
          title.append(name,id);
          const weight = document.createElement('div'); weight.className = 'history-weight'; weight.textContent = plainKg(record.totalGrams);
          top.append(title,weight);
          const bottom = document.createElement('div'); bottom.className = 'row'; bottom.style.marginTop = '9px';
          const mode = document.createElement('span'); mode.className = 'small'; mode.textContent = record.mode;
          const details = document.createElement('button'); details.className = 'secondary'; details.textContent = 'DETAILS'; details.onclick = () => openHistoryDetail(record.id);
          bottom.append(mode,details); item.append(top,bottom); list.appendChild(item);
        });
        byId('historyPrevious').disabled = historyOffset === 0;
        byId('historyNext').disabled = !historyHasMore;
      } catch (error) {
        list.textContent = 'History unavailable: ' + error.message;
        byId('historyPrevious').disabled = true;
        byId('historyNext').disabled = true;
      }
    }
    function searchHistory() {
      historyQuery = byId('historySearch').value.trim();
      loadHistory(true);
    }
    function changeHistoryPage(direction) {
      historyOffset = Math.max(0, historyOffset + direction * historyLimit);
      loadHistory(false);
    }
    function addDetail(label, value) {
      const left = document.createElement('span'); left.textContent = label;
      const right = document.createElement('span'); right.textContent = value;
      byId('detailGrid').append(left,right);
    }
    async function openHistoryDetail(id) {
      try {
        const response = await fetch('/api/v1/history/detail?id=' + encodeURIComponent(id), {cache:'no-store'});
        const data = await response.json();
        if (!response.ok) throw new Error(data.message || 'Details failed');
        activeDetail = data.record;
        byId('detailId').textContent = activeDetail.displayId;
        byId('detailName').value = activeDetail.name;
        byId('detailNotes').value = activeDetail.notes || '';
        const grid = byId('detailGrid'); grid.textContent = '';
        addDetail('Logged', formatLogTime(activeDetail));
        addDetail('Time source', activeDetail.timeSource);
        addDetail('Combined weight', plainKg(activeDetail.totalGrams));
        addDetail('Reading mode', activeDetail.mode);
        activeDetail.scales.forEach((scale,index) => {
          const letter = index === 0 ? 'A' : 'B';
          addDetail('Load Cell ' + letter, plainKg(scale.grams));
          addDetail(letter + ' link/status', (scale.connected ? 'Connected' : 'Disconnected') + ' • ' + scale.status);
          addDetail(letter + ' hardware', 'Sensor ' + (scale.sensor?'OK':'NO') + ' • ADC ' + (scale.adc?'OK':'NO') + ' • OLED ' + (scale.oled?'OK':'NO'));
        });
        if (activeDetail.updatedAtMs !== activeDetail.loggedAtMs) addDetail('Details updated', new Date(Number(activeDetail.updatedAtMs)).toLocaleString());
        byId('detailModal').classList.remove('hidden');
      } catch (error) { showMessage(error.message, true); }
    }
    async function updateHistoryEntry() {
      if (!activeDetail) return;
      const name = byId('detailName').value.trim();
      const notes = byId('detailNotes').value.trim();
      if (!name || utf8Length(name)>50 || utf8Length(notes)>500) { showMessage('Name or notes are too long',true); return; }
      const outcome = await post('/api/v1/history/update', {id:String(activeDetail.id),revision:String(activeDetail.revision),name,notes,timestampMs:String(Date.now())});
      if (outcome.ok) {
        activeDetail = outcome.result.record;
        closeModal('detailModal');
        loadHistory(false);
      }
    }
    async function deleteHistoryEntry() {
      if (!activeDetail || !confirm('Delete ' + activeDetail.displayId + '? This cannot be undone.')) return;
      const outcome = await post('/api/v1/history/delete', {id:String(activeDetail.id)});
      if (outcome.ok) { closeModal('detailModal'); activeDetail=null; loadHistory(true); }
    }
    async function clearHistory() {
      if (!confirm('Delete ALL saved history? This cannot be undone.')) return;
      if (prompt('Type DELETE ALL to confirm') !== 'DELETE ALL') { showMessage('History was not deleted'); return; }
      const outcome = await post('/api/v1/history/clear', {confirm:'DELETE_ALL'});
      if (outcome.ok) loadHistory(true);
    }
    function exportHistory() { window.location.href = '/api/v1/history/export.csv'; }

    async function pollLoop() {
      await refresh();
      setTimeout(pollLoop, document.hidden ? 3000 : 1000);
    }
    document.body.dataset.view = 'control';
    pollLoop();
    document.addEventListener('visibilitychange', refresh);
  </script>
</body>
</html>
)PHONEPAGE";
