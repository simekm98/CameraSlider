'use strict';
const $ = id => document.getElementById(id);
const qa = s => document.querySelectorAll(s);

let ws, S = {}, CAM = {}, camTimer = null, maxPos = 600, homed = false, jogTimer = null, saveTimer = null;

// ---------------- WebSocket ----------------
function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.onopen = () => $('conn').classList.add('on');
  ws.onclose = () => { $('conn').classList.remove('on'); holdEnd(); setTimeout(connect, 1500); };
  ws.onmessage = e => {
    let m; try { m = JSON.parse(e.data); } catch (_) { return; }
    if (m.type === 'status') onStatus(m);
    else if (m.type === 'settings') onSettings(m.s, m.cam || {});
    else if (m.type === 'msg') toast(m.text, m.err);
    else if (m.type === 'scan') onScan(m.list);
  };
}
function send(o) {
  if (ws && ws.readyState === 1) ws.send(JSON.stringify(o));
  else toast('Not connected to the slider', true);
}
function toast(t, err) {
  const d = document.createElement('div');
  d.className = 'toast' + (err ? ' err' : '');
  d.textContent = t;
  $('toasts').appendChild(d);
  setTimeout(() => d.remove(), err ? 6000 : 3000);
}

const f1 = v => (Math.round(v * 10) / 10).toFixed(1);
const pct = v => Math.max(0, Math.min(100, v / maxPos * 100)) + '%';

// ---------------- stav ----------------
const ST = { Idle: 'Ready', Homing: 'Homing…', Moving: 'Moving', Jogging: 'Jogging', Program: 'Program running' };
const PH = { ToStart: 'moving to From', ToB: 'moving → To', WaitB: 'waiting at To', ToA: 'moving → From', WaitA: 'waiting at From' };

function onStatus(x) {
  const m = x.m, w = x.w;
  homed = m.homed;
  $('app').classList.toggle('locked', !homed);

  $('pos').textContent = homed ? f1(m.pos) : '?';
  const st = $('state');
  st.textContent = ST[m.state] || m.state;
  st.className = 'badge' + (m.state === 'Idle' ? '' : ' warn');
  $('phase').textContent = m.state === 'Program' ? (m.mode === 'cycle' ? 'Cycling: ' : 'Single cycle: ') + (PH[m.phase] || '') : '';

  const h = $('hdrHome');
  h.textContent = homed ? 'Homed' : (m.state === 'Homing' ? 'Homing…' : 'Not homed');
  h.className = 'badge ' + (homed ? 'ok' : 'err');

  $('spd').textContent = f1(Math.abs(m.speed || 0)) + ' mm/s';
  $('tgt').textContent = homed ? f1(m.target) + ' mm' : '–';
  $('cyc').textContent = m.state === 'Program' ? `${m.done} / ${m.total || '∞'}` : '–';
  $('wait').textContent = m.waitManual ? 'Continue?' : (m.waitLeft > 0 ? f1(m.waitLeft) + ' s' : '–');
  $('bCont').hidden = !m.waitManual;
  onCamStatus(x.c || {}, m);
  $('hall').innerHTML = m.hall ? '<span class="badge warn">ACTIVE</span>' : 'free';
  $('drv').innerHTML = (m.driver ? '<span class="badge ok">OK</span>' : '<span class="badge err">UART error</span>') +
    (m.drvFails ? ` <span class="hint" title="UART read failures since boot">${m.drvFails}×</span>` : '');

  const car = $('vCar');
  car.style.left = pct(homed ? m.pos : 0);
  car.style.opacity = homed ? 1 : .35;
  const tg = $('vTgt');
  tg.style.left = pct(m.target);
  tg.style.display = homed && m.state !== 'Idle' && m.state !== 'Homing' ? 'block' : 'none';

  $('jmpWarn').hidden = x.jmp !== false;

  let wi = `Access point: <b>CameraSlider</b> – http://${w.apIp} (clients: ${w.apCl})<br>`;
  if (w.sta) wi += `Network: <b>${esc(w.ssid)}</b> – http://${w.ip} (${w.rssi} dBm)`;
  else if (w.ssid) wi += `Network: <b>${esc(w.ssid)}</b> – not connected`;
  else wi += 'Network: not configured';
  $('wifiInfo').innerHTML = wi;
}
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

// ---------------- nastavení ----------------
function onSettings(s, cam) {
  S = s;
  CAM = cam;
  maxPos = s.maxPos || 600;
  qa('[data-k]').forEach(el => {
    const k = el.dataset.k;
    const src = el.dataset.g === 'cam' ? cam : s;
    if (!(k in src) || el === document.activeElement) return;
    if (src !== s) { setField(el, src[k]); return; }
    if (el.type === 'checkbox') el.checked = !!s[k];
    else if (el.type === 'radio') el.checked = (el.value === '1') === !!s[k];
    else el.value = s[k];
  });
  if (document.activeElement !== $('wSsid')) $('wSsid').value = s.ssid || '';
  $('vMax').textContent = maxPos + ' mm';
  $('gotoPos').max = maxPos;
  ['rangeFrom', 'rangeTo'].forEach(k => document.querySelector(`[data-k="${k}"]`).max = maxPos);
  drawRange();
  updateSpm();
  updateCountVis();
  camCalc();
}

function setField(el, v) {
  if (el.type === 'checkbox') el.checked = !!v;
  else el.value = v;
}

// ---------------- kamera ----------------
const BLE = { off: ['Off', ''], unpaired: ['Not paired', 'err'], pairing: ['Pairing…', 'warn'], connecting: ['Connecting…', 'warn'],
  connected: ['Connected', 'ok'], disconnected: ['Disconnected', 'err'] };

function onCamStatus(c, m) {
  const trig = +(CAM.trigger || 0);
  const b = BLE[c.ble] || [c.ble || '–', ''];
  $('bleSt').textContent = b[0];
  $('bleSt').className = 'badge ' + b[1];
  $('bleAddr').textContent = c.addr || '';
  $('bleCnt').textContent = (c.bleShots || c.bleFails) ? `sent ${c.bleShots || 0} · failed ${c.bleFails || 0}` : '';

  let st;
  if (!c.enabled) st = '<span class="hint">photos off</span>';
  else if (trig === 1) st = '<span class="badge ok">Wired</span>';
  else st = `<span class="badge ${b[1]}">${b[0]}</span>`;
  $('camSt').innerHTML = st;

  $('camShots').innerHTML = c.enabled && c.passes
    ? `${c.pShots} / ${c.pPlanned} <span class="hint">· pass ${c.passes}</span>` + (c.failed ? ` <span class="badge err">${c.failed} failed</span>` : '')
    : '–';
  $('logInfo').textContent = c.session ? `· session ${c.session}: ${c.passes} passes, ${c.shots} photos` : '';
}

function camCalc() {
  const c = collect('cam'), mo = collect('motion');
  const box = $('camCalc');
  if (!c.spacing || !mo.speedOut || !mo.speedIn) { box.innerHTML = ''; return; }
  const a = mo.rangeFrom || 0, b = mo.rangeTo || 0, len = Math.abs(b - a);
  const n = Math.max(0, Math.floor((len - (c.startOffset || 0)) / c.spacing + 1e-3) + 1);
  const vFwd = b >= a ? mo.speedOut : mo.speedIn, vBack = b >= a ? mo.speedIn : mo.speedOut;
  const iv = v => c.spacing / v * 1000;
  const cls = ms => ms < (c.minIntervalMs || 0) ? 'bad' : ms < 500 ? 'warn' : '';
  const part = (lbl, ms, on) => on ? `<span>${lbl}: <b class="${cls(ms)}">${Math.round(ms)} ms</b> (${(1000 / ms).toFixed(1)} /s)</span>` : '';
  const dm = +c.dirMode;
  box.innerHTML = `<span>Photos per pass: <b>${n}</b></span>` +
    part('From→To', iv(vFwd), dm !== 2) + part('To→From', iv(vBack), dm !== 1) +
    `<span>Motion blur at 1/200 s without flash: <b>${(vFwd / 200).toFixed(2)} mm</b></span>`;
}

function saveCam() {
  clearTimeout(camTimer);
  send({ cmd: 'camSave', s: collect('cam') });
  $('camSaved').textContent = '· saved ✓';
  setTimeout(() => $('camSaved').textContent = '', 1500);
}

function collect(group) {
  const o = {};
  qa(`[data-g="${group}"][data-k]`).forEach(el => {
    const k = el.dataset.k;
    if (el.type === 'radio') { if (el.checked) o[k] = el.value === '1'; }
    else if (el.type === 'checkbox') o[k] = el.checked;
    else { const v = parseFloat(el.value); if (!isNaN(v)) o[k] = v; }
  });
  return o;
}

function saveMotion() {
  clearTimeout(saveTimer);
  send({ cmd: 'saveMotion', s: collect('motion') });
  $('saved').textContent = '· saved ✓';
  setTimeout(() => $('saved').textContent = '', 1500);
}

function drawRange() {
  const a = parseFloat(document.querySelector('[data-k="rangeFrom"]').value) || 0;
  const b = parseFloat(document.querySelector('[data-k="rangeTo"]').value) || 0;
  const lo = Math.min(a, b), hi = Math.max(a, b);
  const r = $('vRng');
  r.style.left = pct(lo);
  r.style.width = Math.max(0, Math.min(hi, maxPos) - lo) / maxPos * 100 + '%';
  $('vRngLbl').textContent = `From ${f1(a)} → To ${f1(b)}`;
}

function updateSpm() {
  const c = collect('config');
  if (!c.steps100 || !c.microsteps) return;
  const spm = c.steps100 * c.microsteps / 100;
  $('spmInfo').textContent = `${spm.toFixed(2)} microsteps/mm · resolution ${(1000 / spm).toFixed(1)} µm`;
}

function updateCountVis() {
  $('cntWrap').style.display = $('ci1').checked ? 'none' : '';
}

// ---------------- WiFi scan ----------------
function onScan(list) {
  const n = $('nets');
  n.innerHTML = '';
  if (!list.length) { n.innerHTML = '<div class="hint">No networks found</div>'; return; }
  list.sort((a, b) => b.rssi - a.rssi).forEach(e => {
    const b = document.createElement('button');
    b.innerHTML = `<span>${e.open ? '' : '🔒 '}${esc(e.ssid)}</span><span class="hint">${e.rssi} dBm</span>`;
    b.onclick = () => { $('wSsid').value = e.ssid; $('wPass').focus(); };
    n.appendChild(b);
  });
}

// ---------------- ruční posuv ----------------
function holdStart(dir, btn) {
  holdEnd();
  btn.classList.add('active');
  send({ cmd: 'jogHold', dir });
  jogTimer = setInterval(() => send({ cmd: 'jogHold', dir }), 200);
}
function holdEnd() {
  qa('.hold.active').forEach(b => b.classList.remove('active'));
  if (jogTimer) {
    clearInterval(jogTimer);
    jogTimer = null;
    if (ws && ws.readyState === 1) ws.send('{"cmd":"jogRelease"}');
  }
}

// ---------------- události ----------------
$('bHome').onclick = () => send({ cmd: 'home' });
$('bStop').onclick = () => send({ cmd: 'stop' });
$('bOff').onclick = () => { if (confirm('Release motor? Position will be lost and homing will be required.')) send({ cmd: 'disable' }); };
$('bGoto').onclick = () => {
  const v = parseFloat($('gotoPos').value);
  if (isNaN(v)) return toast('Enter a target position', true);
  send({ cmd: 'goto', pos: v });
};
qa('[data-d]').forEach(b => b.onclick = () => send({ cmd: 'jogStep', d: parseFloat(b.dataset.d) }));
qa('.hold').forEach(b => {
  b.addEventListener('pointerdown', e => { e.preventDefault(); b.setPointerCapture(e.pointerId); holdStart(+b.dataset.dir, b); });
  ['pointerup', 'pointercancel', 'lostpointercapture'].forEach(ev => b.addEventListener(ev, holdEnd));
  b.addEventListener('contextmenu', e => e.preventDefault());
});
window.addEventListener('blur', holdEnd);
document.addEventListener('visibilitychange', holdEnd);

$('rail').onclick = e => {
  const r = $('rail').getBoundingClientRect();
  const mm = Math.max(0, Math.min(maxPos, (e.clientX - r.left) / r.width * maxPos));
  $('gotoPos').value = f1(mm);
  toast(`Target ${f1(mm)} mm – press “Go to position”`);
};

qa('[data-g="motion"]').forEach(el => el.addEventListener('change', () => {
  clearTimeout(saveTimer);
  saveTimer = setTimeout(saveMotion, 300);
}));
qa('[data-k="rangeFrom"],[data-k="rangeTo"]').forEach(el => el.addEventListener('input', drawRange));
qa('[data-g="cam"]').forEach(el => el.addEventListener('change', () => {
  clearTimeout(camTimer);
  camTimer = setTimeout(saveCam, 300);
}));
qa('[data-g="cam"],[data-g="motion"]').forEach(el => el.addEventListener('input', camCalc));
$('bCont').onclick = () => send({ cmd: 'continue' });
$('bPair').onclick = () => send({ cmd: 'camPair' });
$('bTest').onclick = () => send({ cmd: 'camTest' });
$('bForget').onclick = () => { if (confirm('Forget the paired camera?')) send({ cmd: 'camForget' }); };
qa('[name="ci"]').forEach(el => el.addEventListener('change', updateCountVis));
qa('[data-g="config"]').forEach(el => el.addEventListener('input', updateSpm));

$('bCycle').onclick = () => { saveMotion(); saveCam(); send({ cmd: 'cycle' }); };
$('bSingle').onclick = () => { saveMotion(); saveCam(); send({ cmd: 'single' }); };

$('bCfg').onclick = () => send({ cmd: 'saveConfig', s: collect('config') });
$('bFactory').onclick = () => { if (confirm('Restore factory settings? Everything including WiFi will be erased.')) send({ cmd: 'factoryReset' }); };

$('bScan').onclick = () => { $('nets').innerHTML = '<div class="hint">Scanning…</div>'; send({ cmd: 'scan' }); };
$('bWifi').onclick = () => {
  const ssid = $('wSsid').value.trim();
  if (!ssid) return toast('Enter SSID', true);
  send({ cmd: 'saveWifi', ssid, pass: $('wPass').value });
};
$('bWifiForget').onclick = () => { if (confirm('Forget the WiFi network?')) send({ cmd: 'saveWifi', ssid: '', pass: '' }); };

// Space / Esc = STOP (outside input fields)
document.addEventListener('keydown', e => {
  if ((e.key === ' ' || e.key === 'Escape') && !['INPUT', 'SELECT', 'TEXTAREA'].includes(document.activeElement.tagName)) {
    e.preventDefault();
    send({ cmd: 'stop' });
  }
});

connect();
