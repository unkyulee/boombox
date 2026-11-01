#pragma once
#include <Arduino.h>

// Single-page web UI served at "/". Talks to the JSON API in web.cpp.
const char INDEX_HTML[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>MP3 Player</title>
<style>
:root{--bg:#0e1016;--panel:#1a1d26;--line:#2a2f3c;--text:#f0f2f5;--dim:#8c92a0;--accent:#00a8ff;--warn:#ff5f5f;--folder:#ffc450}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--text);font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
main{max-width:900px;margin:0 auto;padding:0 16px 32px}
button,a.btn{background:var(--line);color:var(--text);border:0;border-radius:10px;padding:9px 13px;font:inherit;cursor:pointer;text-decoration:none;display:inline-block}
button:hover,a.btn:hover{filter:brightness(1.25)}
button.primary{background:var(--accent);color:#00131f;font-weight:600}
button.danger{background:transparent;color:var(--warn)}
button.small,a.btn.small{padding:5px 9px;font-size:13px}
button:disabled{opacity:.4;cursor:not-allowed}
input,select{background:var(--bg);color:var(--text);border:1px solid var(--line);border-radius:8px;padding:9px 10px;font:inherit}
h2{font-size:14px;margin:0 0 12px;color:var(--dim);font-weight:600;text-transform:uppercase;letter-spacing:.05em}
section{background:var(--panel);border-radius:14px;padding:16px;margin-bottom:16px}
.muted{color:var(--dim)}

/* player bar */
header{position:sticky;top:0;z-index:5;background:var(--panel);border-bottom:1px solid var(--line)}
.player{max-width:900px;margin:0 auto;padding:12px 16px;display:flex;gap:14px;align-items:center}
.np{flex:1;min-width:0;cursor:pointer}
.np b{display:block;font-size:16px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.np span{color:var(--dim);font-size:13px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;display:block}
.pbtns{display:flex;gap:8px}
.pbtns button{min-width:48px;font-size:16px}
.vol{display:flex;align-items:center;gap:8px;color:var(--dim);font-size:13px}
.vol input{padding:0;width:120px;accent-color:var(--accent)}
.vol span{min-width:18px;text-align:right;font-variant-numeric:tabular-nums}
.bar{height:6px;background:var(--line);border-radius:3px;cursor:pointer;max-width:900px;margin:0 auto}
.bar>div{height:100%;background:var(--accent);border-radius:3px;width:0}
.times{max-width:900px;margin:0 auto;padding:3px 16px 8px;display:flex;justify-content:space-between;color:var(--dim);font-size:12px;font-variant-numeric:tabular-nums}

/* tabs */
nav{display:flex;gap:8px;padding:14px 0}
nav button.on{background:var(--accent);color:#00131f;font-weight:600}

/* files */
.tools{display:flex;gap:8px;flex-wrap:wrap;align-items:center;margin-bottom:12px}
.crumbs{display:flex;flex-wrap:wrap;gap:4px;align-items:center;margin-bottom:10px;font-size:14px}
.crumbs a{color:var(--accent);cursor:pointer;text-decoration:none}
table{width:100%;border-collapse:collapse}
td{padding:9px 4px;border-top:1px solid var(--line);vertical-align:middle}
td.name{overflow-wrap:anywhere;cursor:pointer}
td.size{color:var(--dim);white-space:nowrap;text-align:right;font-variant-numeric:tabular-nums}
td.act{white-space:nowrap;text-align:right}
tr.current td.name{color:var(--accent);font-weight:600}
.ico{display:inline-block;width:22px}
.ico.f{color:var(--folder)}
#drop{border:2px dashed var(--line);border-radius:12px;padding:16px;text-align:center;color:var(--dim);cursor:pointer;margin-top:12px}
#drop.over{border-color:var(--accent);color:var(--text)}
.q{display:flex;gap:10px;justify-content:space-between;padding:3px 0;font-size:13px}
.q .st{color:var(--dim);white-space:nowrap}
.q .err{color:var(--warn)}

/* settings */
.net{display:flex;gap:10px;align-items:center;justify-content:space-between;padding:8px 0;border-top:1px solid var(--line)}
.addnet{display:flex;gap:8px;flex-wrap:wrap;margin-top:12px}
.addnet input{flex:1;min-width:150px}
@media (max-width:560px){td.size{display:none}.player{gap:10px;flex-wrap:wrap}.pbtns button{min-width:44px}
  .vol{order:3;width:100%}.vol input{flex:1;width:auto}}
</style>
</head>
<body>
<header>
  <div class="player">
    <div class="np" onclick="openPlayingFolder()" title="Show this folder">
      <b id="title">-</b><span id="sub"></span>
    </div>
    <div class="pbtns">
      <button onclick="cmd('prev')" title="Previous">&#9198;</button>
      <button class="primary" id="play" onclick="cmd('toggle')" title="Play / pause">&#9654;</button>
      <button onclick="cmd('next')" title="Next">&#9197;</button>
    </div>
    <div class="vol" title="Volume">&#128266;<input type="range" id="volume" min="0" max="21"><span id="volnum"></span></div>
  </div>
  <div class="bar" id="bar"><div id="fill"></div></div>
  <div class="times"><span id="pos">0:00</span><span id="dur">--:--</span></div>
</header>

<main>
<nav>
  <button id="tab-files" class="on" onclick="showTab('files')">Files</button>
  <button id="tab-settings" onclick="showTab('settings')">Settings</button>
</nav>

<div id="view-files">
  <section>
    <div class="crumbs" id="crumbs"></div>
    <div class="tools">
      <button onclick="mkdir()">New folder</button>
      <button onclick="picker.click()">Upload files</button>
      <button onclick="fetchUrl()">Add from URL</button>
      <button onclick="playFolder()">Play this folder</button>
      <span class="muted" id="usage" style="margin-left:auto"></span>
    </div>
    <table><tbody id="files"></tbody></table>
    <p class="muted" id="empty" hidden>This folder is empty.</p>
    <div id="drop">Drop files here to upload into this folder<input type="file" id="picker" multiple hidden></div>
    <div id="queue"></div>
  </section>
</div>

<div id="view-settings" hidden>
  <section>
    <h2>Wi-Fi</h2>
    <div id="wifi-now" class="muted">...</div>
    <div id="wifi-saved"></div>
    <div class="addnet">
      <select id="scan"><option value="">Pick a network...</option></select>
      <button onclick="scan()" id="scanbtn">Scan</button>
    </div>
    <div class="addnet">
      <input id="ssid" placeholder="Network name (SSID)">
      <input id="pass" type="password" placeholder="Password">
      <button class="primary" onclick="addNet()">Save network</button>
    </div>
    <p class="muted">Saved networks are joined automatically at startup, strongest first. If none can be
      joined, the player starts its own access point.</p>
  </section>
  <section>
    <h2>Device</h2>
    <div id="device" class="muted"></div>
    <div class="tools" style="margin-top:12px"><button onclick="restart()">Restart player</button></div>
  </section>
  <section>
    <h2>Firmware</h2>
    <div id="fw-now" class="muted">...</div>
    <div class="tools" style="margin-top:12px">
      <button class="primary" id="fwbtn" onclick="fwpick.click()">Upload firmware</button>
      <input type="file" id="fwpick" accept=".bin" hidden>
      <span id="fw-st" class="muted"></span>
    </div>
    <div class="bar" id="fw-bar" style="margin-top:12px" hidden><div id="fw-fill"></div></div>
    <p class="muted">Pick the <b>firmware.bin</b> from <code>.pio/build/jc4827w543c/</code>. Playback stops
      during the update, and the player restarts into the new firmware when it is done. If anything
      goes wrong, the current firmware stays in place.</p>
  </section>
</div>
</main>

<script>
const $ = id => document.getElementById(id);
const picker = $('picker');
let now = {}, path = '/music', entries = [], usage = {}, sliding = false;

const clock = s => {
  s = Math.max(0, s | 0);
  const h = Math.floor(s / 3600), m = Math.floor(s / 60) % 60, ss = String(s % 60).padStart(2, '0');
  return h ? `${h}:${String(m).padStart(2, '0')}:${ss}` : `${m}:${ss}`;
};
const size = b => b >= 1e9 ? (b / 1e9).toFixed(1) + ' GB' : b >= 1e6 ? (b / 1e6).toFixed(1) + ' MB' : Math.round(b / 1e3) + ' KB';
const join = (dir, name) => (dir === '/' ? '' : dir) + '/' + name;

async function api(url, params) {
  const opts = params ? {method: 'POST', body: new URLSearchParams(params)} : {};
  const r = await fetch(url, opts);
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || r.statusText);
  return j;
}
const fail = e => alert(e.message || e);

/* ---- player ---- */
async function cmd(c, v) {
  try { await api('/api/cmd', v === undefined ? {c} : {c, v}); } catch (e) { fail(e); }
  setTimeout(poll, 250);
}

async function poll() {
  try { now = await api('/api/status'); } catch (e) { return; }
  $('title').textContent = now.count ? (now.title || '-') : 'Nothing to play';
  $('sub').textContent = [now.artist, now.album || now.folder].filter(Boolean).join(' - ');
  $('pos').textContent = clock(now.position);
  $('dur').textContent = now.duration ? clock(now.duration) : '--:--';
  $('fill').style.width = now.duration ? Math.min(100, 100 * now.position / now.duration) + '%' : '0';
  $('play').innerHTML = now.playing ? '&#9208;' : '&#9654;';
  if (!sliding) { $('volume').max = now.volumeMax; $('volume').value = now.volume; }
  $('volnum').textContent = $('volume').value;
  markCurrent();
}

const markCurrent = () =>
  document.querySelectorAll('#files tr').forEach(tr => tr.classList.toggle('current', tr.dataset.path === now.file));

$('volume').oninput = () => { sliding = true; $('volnum').textContent = $('volume').value; };
$('volume').onchange = () => { sliding = false; cmd('volume', $('volume').value); };

$('bar').onclick = e => {
  if (!now.duration) return;
  const r = e.currentTarget.getBoundingClientRect();
  cmd('seek', Math.round(now.duration * (e.clientX - r.left) / r.width));
};

function openPlayingFolder() {
  if (now.folder) { showTab('files'); load(now.folder); }
}

/* ---- tabs ---- */
function showTab(name) {
  $('view-files').hidden = name !== 'files';
  $('view-settings').hidden = name !== 'settings';
  $('tab-files').classList.toggle('on', name === 'files');
  $('tab-settings').classList.toggle('on', name === 'settings');
  if (name === 'settings') { loadWifi(); loadFirmware(); }
}

/* ---- files ---- */
async function load(p) {
  try {
    const j = await api('/api/fs/list?path=' + encodeURIComponent(p || path));
    path = j.path; entries = j.entries; usage = j;
    render();
  } catch (e) { fail(e); }
}

function crumbs() {
  const box = $('crumbs');
  box.replaceChildren();
  const parts = path.split('/').filter(Boolean);
  const add = (label, target) => {
    const a = document.createElement('a');
    a.textContent = label;
    a.onclick = () => load(target);
    box.append(a);
  };
  add('SD card', '/');
  let acc = '';
  parts.forEach(p => {
    acc += '/' + p;
    box.append(document.createTextNode(' / '));
    add(p, acc);
  });
}

function render() {
  crumbs();
  $('usage').textContent = usage.total ? `${size(usage.used)} of ${size(usage.total)} used` : '';
  const body = $('files');
  body.replaceChildren();

  if (path !== '/') {
    const tr = document.createElement('tr');
    tr.innerHTML = '<td class="name" colspan="3"><span class="ico f">&#8617;</span>..</td>';
    tr.querySelector('td').onclick = () => load(usage.parent);
    body.append(tr);
  }

  entries.forEach(e => {
    const full = join(path, e.name);
    const tr = document.createElement('tr');
    tr.dataset.path = full;
    tr.innerHTML = `<td class="name"><span class="ico ${e.dir ? 'f' : ''}">${e.dir ? '&#128193;' : '&#9834;'}</span></td>
      <td class="size"></td><td class="act"></td>`;
    const name = tr.querySelector('.name');
    name.append(document.createTextNode(e.name));
    name.onclick = () => e.dir ? load(full) : play(full);
    tr.querySelector('.size').textContent = e.dir ? '' : size(e.size);

    const act = tr.querySelector('.act');
    const button = (label, cls, fn) => {
      const b = document.createElement('button');
      b.className = 'small ' + cls;
      b.textContent = label;
      b.onclick = fn;
      act.append(b, document.createTextNode(' '));
    };
    button('Play', '', () => play(full));
    button('Rename', '', () => rename(full, e.name));
    button('Move', '', () => move(full, e.name));
    if (!e.dir) {
      const a = document.createElement('a');
      a.className = 'btn small';
      a.href = '/api/fs/download?path=' + encodeURIComponent(full);
      a.download = e.name;
      a.textContent = 'Save';
      act.append(a, document.createTextNode(' '));
    }
    button('Delete', 'danger', () => del(full, e.name));
    body.append(tr);
  });

  $('empty').hidden = entries.length > 0;
  if (usage.truncated) $('empty').hidden = true;
  markCurrent();
}

const play = async p => { try { await api('/api/play', {path: p}); setTimeout(poll, 300); } catch (e) { fail(e); } };
const playFolder = () => play(path);

async function mkdir() {
  const name = prompt('Name of the new folder');
  if (!name) return;
  try { await api('/api/fs/mkdir', {path: join(path, name)}); load(); } catch (e) { fail(e); }
}

async function rename(full, name) {
  const to = prompt('New name', name);
  if (!to || to === name) return;
  try { await api('/api/fs/rename', {path: full, to: join(path, to)}); load(); } catch (e) { fail(e); }
}

async function move(full, name) {
  const dir = prompt('Move to which folder? (full path)', path);
  if (!dir) return;
  try { await api('/api/fs/rename', {path: full, to: join(dir.replace(/\/$/, '') || '/', name)}); load(); }
  catch (e) { fail(e); }
}

async function del(full, name) {
  if (!confirm('Delete ' + name + '?')) return;
  try { await api('/api/fs/delete', {path: full}); load(); } catch (e) { fail(e); }
}

/* ---- uploads, one at a time so playback keeps up ---- */
const pending = [];
let uploading = false;

function enqueue(list) {
  for (const file of list) {
    const row = document.createElement('div');
    row.className = 'q';
    row.innerHTML = '<span></span><span class="st">waiting</span>';
    row.firstChild.textContent = file.name;
    $('queue').append(row);
    pending.push({file, dir: path, st: row.lastChild});
  }
  if (!uploading) next();
}

// Files go up in chunks: the player drops an upload after a second without data, so a big file
// in one piece rarely survives Wi-Fi. A dropped chunk costs a retry, not the whole file.
const CHUNK = 1024 * 1024;
const RETRIES = 6;

function sendChunk(job, offset) {
  const end = Math.min(offset + CHUNK, job.file.size);
  return new Promise(resolve => {
    const xhr = new XMLHttpRequest(), form = new FormData();
    form.append('file', job.file.slice(offset, end), job.file.name);
    xhr.upload.onprogress = e => {
      if (job.file.size) job.st.textContent = Math.floor(100 * (offset + e.loaded) / job.file.size) + '%';
    };
    xhr.onload = () => {
      let j = {};
      try { j = JSON.parse(xhr.responseText); } catch (e) {}
      resolve({status: xhr.status, j, end});
    };
    xhr.onerror = xhr.ontimeout = () => resolve({status: 0, j: {error: 'network error'}, end});
    xhr.timeout = 180000;
    xhr.open('POST', '/api/fs/upload?' + new URLSearchParams({path: job.dir, offset, total: job.file.size}));
    xhr.send(form);
  });
}

async function next() {
  const job = pending.shift();
  if (!job) { uploading = false; load(); return; }
  uploading = true;
  let offset = 0, fails = 0, err = '';
  do {
    const r = await sendChunk(job, offset);
    if (r.status === 200) { offset = r.end; fails = 0; continue; }
    // The card holds less than expected: carry on from what it has.
    if (r.status === 409 && typeof r.j.size === 'number') { offset = r.j.size; continue; }
    err = r.j.error || 'failed';
    if ([400, 404, 507].includes(r.status) || ++fails > RETRIES) break;  // retrying will not help
    job.st.textContent = `${err}, retrying (${fails}/${RETRIES})`;
    await new Promise(res => setTimeout(res, 2000 * fails));
    err = '';
  } while (offset < job.file.size);

  if (err && offset > 0) api('/api/fs/delete', {path: join(job.dir, job.file.name + '.part')}).catch(() => {});
  job.st.textContent = err || 'done';
  job.st.classList.toggle('err', !!err);
  next();
}

/* ---- download straight to the card from a URL ---- */
let fetchTimer = null;

async function fetchUrl() {
  const url = prompt('Address of the file to download into this folder');
  if (!url) return;
  const row = document.createElement('div');
  row.className = 'q';
  row.innerHTML = '<span></span><span class="st">starting</span>';
  row.firstChild.textContent = url;
  $('queue').append(row);
  try {
    const j = await api('/api/fs/fetch', {url, path});
    row.firstChild.textContent = j.name;
  } catch (e) {
    row.lastChild.textContent = e.message;
    row.lastChild.classList.add('err');
    return;
  }
  watchFetch(row);
}

function watchFetch(row) {
  clearInterval(fetchTimer);
  fetchTimer = setInterval(async () => {
    let f;
    try { f = await api('/api/fs/fetch'); } catch (e) { return; }
    const got = size(f.received);
    row.lastChild.textContent = f.active
      ? (f.total > 0 ? Math.round(100 * f.received / f.total) + '%  (' + got + ')' : got)
      : (f.error || 'done - ' + got);
    row.lastChild.classList.toggle('err', !!f.error);
    if (!f.active) { clearInterval(fetchTimer); load(); }
  }, 700);
}

const drop = $('drop');
drop.onclick = () => picker.click();
picker.onchange = e => { enqueue(e.target.files); e.target.value = ''; };
drop.ondragover = e => { e.preventDefault(); drop.classList.add('over'); };
drop.ondragleave = () => drop.classList.remove('over');
drop.ondrop = e => { e.preventDefault(); drop.classList.remove('over'); enqueue(e.dataTransfer.files); };

/* ---- settings ---- */
async function loadWifi() {
  let w;
  try { w = await api('/api/wifi'); } catch (e) { return fail(e); }
  $('wifi-now').textContent = w.mode === 'ap'
    ? `Own access point "${w.ssid}" at ${w.ip}`
    : (w.connected ? `Connected to "${w.ssid}" at ${w.ip}` : 'Not connected');
  $('device').innerHTML = `Address: http://${w.ip} &middot; http://${w.hostname}.local<br>` +
    (usage.total ? `SD card: ${size(usage.used)} of ${size(usage.total)} used` : '');

  const box = $('wifi-saved');
  box.replaceChildren();
  w.saved.forEach(ssid => {
    const row = document.createElement('div');
    row.className = 'net';
    row.innerHTML = '<span></span>';
    row.firstChild.textContent = ssid;
    const b = document.createElement('button');
    b.className = 'small danger';
    b.textContent = 'Forget';
    b.onclick = async () => {
      if (!confirm('Forget ' + ssid + '?')) return;
      try { await api('/api/wifi/remove', {ssid}); loadWifi(); } catch (e) { fail(e); }
    };
    row.append(b);
    box.append(row);
  });
  if (!w.saved.length) box.innerHTML = '<p class="muted">No saved networks yet.</p>';
}

async function scan() {
  const btn = $('scanbtn');
  btn.disabled = true; btn.textContent = 'Scanning...';
  try {
    const nets = await api('/api/wifi/scan');
    const sel = $('scan');
    sel.replaceChildren(new Option('Pick a network...', ''));
    nets.sort((a, b) => b.rssi - a.rssi).forEach(n => {
      if (n.ssid) sel.append(new Option(`${n.ssid}  (${n.rssi} dBm)${n.open ? ' open' : ''}`, n.ssid));
    });
    sel.onchange = () => { $('ssid').value = sel.value; $('pass').focus(); };
  } catch (e) { fail(e); }
  btn.disabled = false; btn.textContent = 'Scan';
}

async function addNet() {
  const ssid = $('ssid').value.trim();
  if (!ssid) return alert('Enter a network name');
  try {
    await api('/api/wifi/add', {ssid, pass: $('pass').value});
    $('pass').value = '';
    loadWifi();
    if (confirm('Saved. Restart now to join it?')) restart();
  } catch (e) { fail(e); }
}

async function restart() {
  try { await api('/api/restart', {}); } catch (e) {}
  alert('Restarting. Reload this page in a few seconds.');
}

/* ---- firmware update ---- */
let fwInfo = {};

async function loadFirmware() {
  try { fwInfo = await api('/api/firmware'); } catch (e) { return; }
  $('fw-now').textContent = `Built ${fwInfo.built} · ${size(fwInfo.size)} of ${size(fwInfo.max)} available`;
}

$('fwpick').onchange = e => {
  const file = e.target.files[0];
  e.target.value = '';
  if (!file) return;
  if (!/\.bin$/i.test(file.name)) return alert('Pick a .bin firmware file');
  if (fwInfo.max && file.size > fwInfo.max) return alert(`${file.name} is ${size(file.size)}, but only ${size(fwInfo.max)} fits`);
  if (!confirm(`Install ${file.name} (${size(file.size)})? The player restarts when it is done.`)) return;

  const st = $('fw-st'), btn = $('fwbtn');
  const done = (msg, err) => {
    st.textContent = msg; st.style.color = err ? 'var(--warn)' : '';
    btn.disabled = false; $('fw-bar').hidden = true;
  };
  btn.disabled = true; $('fw-bar').hidden = false; $('fw-fill').style.width = '0';
  const xhr = new XMLHttpRequest(), form = new FormData();
  form.append('firmware', file, file.name);
  xhr.upload.onprogress = e => {
    const pct = Math.round(100 * e.loaded / e.total);
    $('fw-fill').style.width = pct + '%';
    st.textContent = pct < 100 ? `Uploading ${pct}%` : 'Verifying...';
  };
  xhr.onload = () => {
    if (xhr.status !== 200) {
      let err = 'failed';
      try { err = JSON.parse(xhr.responseText).error; } catch (e) {}
      return done('Update failed: ' + err, true);
    }
    st.textContent = 'Installed. Restarting...';
    waitForRestart();
  };
  xhr.onerror = () => done('Network error during upload', true);
  xhr.open('POST', '/api/firmware');
  xhr.send(form);
};

// Polls until the player answers again, then reloads so the page matches the new firmware.
function waitForRestart() {
  const started = Date.now();
  setTimeout(async function check() {
    try { await api('/api/firmware'); location.reload(); return; } catch (e) {}
    if (Date.now() - started > 60000) {
      $('fw-st').textContent = 'The player has not come back yet. Reload this page in a moment.';
      $('fwbtn').disabled = false;
      return;
    }
    setTimeout(check, 2000);
  }, 4000);
}

load('/music');
poll();
setInterval(poll, 1000);
</script>
</body>
</html>
)rawliteral";
