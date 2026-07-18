/* Calibrator dashboard
 *  - Uses native WebSocket where available, falls back to polling HTTP.
 *  - All four ESP32 boards are addressed by board id (esp1..esp4).
 *  - The sensor history is bounded so memory does not grow without limit.
 */
'use strict';

/* ----------------------- Tiny utilities ----------------------- */
const $ = (id) => document.getElementById(id);
const logEntries = [];

function classifyLog(message) {
  const value = String(message).toLowerCase();
  if (value.includes('error') || value.includes('failed') || value.includes('exception')) return 'error';
  if (value.includes('warn') || value.includes('retry')) return 'warning';
  if (value.includes('connect') || value.includes('ws open') || value.includes('ws closed')) return 'connection';
  if (value.includes('saved') || value.includes(' ok') || value.includes('ready')) return 'success';
  return 'system';
}

function renderLogView() {
  const view = $('log-view');
  if (!view) return;
  const search = ($('log-search')?.value || '').trim().toLowerCase();
  const board = $('log-board-filter')?.value || 'all';
  const type = $('log-type-filter')?.value || 'all';
  const filtered = logEntries.filter((entry) => {
    const searchMatch = !search || entry.message.toLowerCase().includes(search);
    const boardMatch = board === 'all' || entry.board === board;
    const typeMatch = type === 'all' || entry.type === type;
    return searchMatch && boardMatch && typeMatch;
  });
  view.textContent = filtered.map((entry) => `[${entry.timestamp}] ${entry.message}`).join('\n') + (filtered.length ? '\n' : '');
  const count = $('log-count');
  if (count) count.textContent = `${filtered.length} event${filtered.length === 1 ? '' : 's'}`;
  if (!$('log-pause')?.checked) view.scrollTop = view.scrollHeight;
}

function log(line) {
  const ts = new Date().toISOString().replace('T', ' ').slice(0, 19);
  const message = String(line);
  const match = message.match(/\b(esp[1-4])\b/i);
  const entry = {
    timestamp: ts,
    message,
    board: match ? match[1].toLowerCase() : 'system',
    type: classifyLog(message),
  };
  logEntries.push(entry);
  while (logEntries.length > 1000) logEntries.shift();
  renderLogView();
  document.dispatchEvent(new CustomEvent('aquagrow:log', { detail: entry }));
}
function badge(card, status, text) {
  if (!card) return;
  const el = card.querySelector('.badge');
  if (!el) return;
  el.className = 'badge ' + status;
  el.textContent = text;
}

/* ----------------------- Connection manager ----------------------- */
const LS_KEY = 'calibrator.ips';
const DEFAULT_ADDRESSES = {
  esp1: 'calibrator-esp32-1.local',
  esp2: 'calibrator-esp32-2.local',
  esp3: 'calibrator-esp32-3.local',
  esp4: 'calibrator-esp32-4.local',
};
const LEGACY_DEVICE_IDS = {
  esp32_1_sensors: 'esp1',
  esp32_2_relays16: 'esp2',
  esp32_3_relays8_4: 'esp3',
  esp32_4_extras: 'esp4',
};
let savedIps = {};
try { savedIps = JSON.parse(localStorage.getItem(LS_KEY) || '{}'); } catch (_) { savedIps = {}; }
['esp1','esp2','esp3','esp4'].forEach((b) => {
  $('ip-'+b).value = savedIps[b] || DEFAULT_ADDRESSES[b];
});

function normalizeAddress(value) {
  return String(value || '')
    .trim()
    .replace(/^https?:\/\//i, '')
    .replace(/^wss?:\/\//i, '')
    .replace(/\/.*$/, '')
    .replace(/:80$/, '')
    .replace(/:81$/, '');
}

function collectAddresses() {
  return Object.fromEntries(Object.keys(boards).map((boardId) => [
    boardId,
    normalizeAddress($('ip-'+boardId).value),
  ]));
}

function duplicateAddressMessage(addresses) {
  const owners = {};
  for (const [boardId, address] of Object.entries(addresses)) {
    if (!address) continue;
    const key = address.toLowerCase();
    (owners[key] ||= []).push(boardId);
  }
  const duplicate = Object.entries(owners).find(([, boardIds]) => boardIds.length > 1);
  if (!duplicate) return '';
  return `Address ${duplicate[0]} is assigned to ${duplicate[1].join(' and ')}. Each ESP32 must have a unique address.`;
}

const boards = {
  esp1: { name: 'ESP32 #1 (sensors)',  ws: null, httpBase: '', wsBase: '', interval: null },
  esp2: { name: 'ESP32 #2 (16 relay)', ws: null, httpBase: '', wsBase: '', interval: null },
  esp3: { name: 'ESP32 #3 (8+4 relay)',ws: null, httpBase: '', wsBase: '', interval: null },
  esp4: { name: 'ESP32 #4 (extras)',   ws: null, httpBase: '', wsBase: '', interval: null },
};

$('btn-save-ips').addEventListener('click', () => {
  const ips = collectAddresses();
  const duplicateError = duplicateAddressMessage(ips);
  if (duplicateError) { log(duplicateError); return; }
  for (const [boardId, address] of Object.entries(ips)) $('ip-'+boardId).value = address;
  localStorage.setItem(LS_KEY, JSON.stringify(ips));
  log('Saved board addresses: ' + JSON.stringify(ips));
});

$('btn-connect-all').addEventListener('click', () => {
  const duplicateError = duplicateAddressMessage(collectAddresses());
  if (duplicateError) { log(duplicateError); return; }
  for (const b of Object.keys(boards)) connectBoard(b);
});
$('btn-disconnect-all').addEventListener('click', () => {
  for (const b of Object.keys(boards)) disconnectBoard(b);
});

function connectBoard(boardId) {
  const ip = normalizeAddress($('ip-'+boardId).value);
  if (!ip) { log(`${boardId}: no hostname or IP set`); return; }
  $('ip-'+boardId).value = ip;
  const duplicateError = duplicateAddressMessage(collectAddresses());
  if (duplicateError) { log(duplicateError); return; }
  const card = document.querySelector(`.conn-card[data-board="${boardId}"]`);
  const b = boards[boardId];
  if (b.interval) clearInterval(b.interval);
  if (b.ws) {
    b.manualClose = true;
    try { b.ws.close(); } catch (_) {}
  }
  b.httpBase = `http://${ip}`;
  b.wsBase   = `ws://${ip}:81`;
  b.manualClose = false;
  b.identityError = false;
  b.verified = false;
  badge(card, 'connecting', 'connecting');
  card.querySelector('small').textContent = ip;
  try {
    b.ws = new WebSocket(b.wsBase);
    b.ws.onopen    = () => {
      badge(card, 'connecting', 'verifying');
      log(`${boardId} socket open; verifying device identity`);
    };
    b.ws.onmessage = (ev) => handleMessage(boardId, ev.data);
    b.ws.onerror   = () => { badge(card, 'error', 'error'); log(`${boardId} WS error; REST fallback remains available`); };
    b.ws.onclose   = () => {
      badge(card, b.identityError ? 'error' : 'unknown', b.identityError ? 'wrong board' : 'disconnected');
      log(`${boardId} WS closed`);
      if (!b.manualClose) document.dispatchEvent(new CustomEvent('calibrator:closed', { detail: boardId }));
    };
    // Keep-alive for installations that explicitly enable the optional
    // communication-loss fail-safe in the relay firmware.
    b.interval = setInterval(() => {
      if (b.ws && b.ws.readyState === 1) b.ws.send(JSON.stringify({ type: 'pong' }));
    }, 5000);
  } catch (e) {
    badge(card, 'error', 'failed');
    log(`${boardId} WS exception ${e}`);
  }
}

function disconnectBoard(boardId) {
  const b = boards[boardId];
  if (b.interval) clearInterval(b.interval);
  b.interval = null;
  b.manualClose = true;
  if (b.ws) try { b.ws.close(); } catch (e) {}
  b.ws = null;
  const card = document.querySelector(`.conn-card[data-board="${boardId}"]`);
  badge(card, 'unknown', 'disconnected');
}

function sendWs(boardId, obj) {
  const b = boards[boardId];
  if (b.ws && b.ws.readyState === 1) { b.ws.send(JSON.stringify(obj)); return true; }
  if (!b.httpBase) return false;

  // Firmware exposes REST equivalents for every command. Keep controls
  // useful when port 81 is blocked but the board's HTTP server is reachable.
  let endpoint = '/cmd';
  let method = 'POST';
  if (boardId === 'esp1') {
    if (obj.type === 'get_sensors' || obj.type === 'get_state') { endpoint = '/sensors'; method = 'GET'; }
    else if (obj.type === 'set_cal') endpoint = '/calibration';
    else if (obj.type === 'reset_cal') endpoint = '/reset';
  } else if (obj.type === 'get_state') {
    endpoint = '/state'; method = 'GET';
  }
  const options = method === 'GET' ? {} : {
    method,
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(obj),
  };
  fetch(b.httpBase + endpoint, options)
    .then((response) => {
      if (!response.ok) throw new Error(`HTTP ${response.status}`);
      return response.text();
    })
    .then((raw) => {
      if (!raw) return;
      try { handleMessage(boardId, raw); } catch (_) {}
    })
    .catch((error) => log(`${boardId} REST error: ${error.message}`));
  return true;
}

function requestState(boardId) {
  sendWs(boardId, { type: boardId === 'esp1' ? 'get_sensors' : 'get_state' });
}

/* ----------------------- Board routing ----------------------- */
function handleMessage(boardId, raw) {
  let msg; try { msg = JSON.parse(raw); } catch (e) { return; }
  const t = msg.type;
  if (t === 'hello') {
    const actualId = msg.device_id || LEGACY_DEVICE_IDS[msg.board] || '';
    const b = boards[boardId];
    const card = document.querySelector(`.conn-card[data-board="${boardId}"]`);
    if (actualId && actualId !== boardId) {
      b.identityError = true;
      b.manualClose = true;
      badge(card, 'error', 'wrong board');
      log(`${boardId} identity mismatch: address belongs to ${actualId}`);
      try { b.ws.close(1008, 'Wrong ESP32 identity'); } catch (_) {}
      return;
    }
    b.verified = true;
    badge(card, 'connected', 'connected');
    log(`${boardId} identity verified at ${normalizeAddress($('ip-'+boardId).value)}`);
    document.dispatchEvent(new CustomEvent('calibrator:open', { detail: boardId }));
    requestState(boardId);
    return;
  }
  if (t === 'sensors') return renderSensors(msg);
  if (t === 'state') {
    if (boardId === 'esp2') return renderRelay16(msg);
    if (boardId === 'esp3') return renderRelay84(msg);
    if (boardId === 'esp4') return renderExtras(msg);
  }
  if (t === 'ack')    log(`${boardId} ack ${msg.cmd}: ${msg.ok ? 'ok' : 'FAILED'}`);
  if (t === 'error')  log(`${boardId} ERROR: ${msg.msg}`);
  if (t === 'cal_result') log(`${boardId} cal ${msg.name} ${msg.ok ? 'saved' : 'failed'}`);
}

/* ----------------------- Sensors panel ----------------------- */
const SENSOR_HISTORY_LIMIT = 60;   // ~1 minute at 1Hz
const sensorHistory = [];          // [{t, v: {id: value}}]
const sensorMeta    = [];          // [{id,name,cal,...}]
const sensorColors  = ['#20C5D8','#42C77A','#8BD34A','#F4B942','#7B8CE8','#1AAFC1','#EF7B8A','#67B7DC','#36B37E','#A0C83C'];
const sensorConfig = [
  { unit: '\u00B0C', gpio: '4',     fields: ['slope', 'offset'], hint: 'Compare with a trusted thermometer, then adjust slope or offset.' },
  { unit: '\u00B0C', gpio: '5',     fields: ['slope', 'offset'], hint: 'Use the second temperature probe as an independent reference.' },
  { unit: 'ppm',     gpio: '34',    fields: ['tds_k'], hint: 'Immerse in a known TDS solution and tune the cell constant.' },
  { unit: 'ppm',     gpio: '35',    fields: ['tds_k'], hint: 'Rinse the probe before using a known TDS reference solution.' },
  { unit: 'mg/L',    gpio: '33',    fields: ['slope', 'offset'], hint: 'Calibrate in air-saturated water at a known temperature.' },
  { unit: 'pH',      gpio: '32',    fields: ['slope', 'offset'], hint: 'Use pH 7 and pH 4 buffer solutions for a two-point calibration.' },
  { unit: 'ratio',   gpio: '36/25', fields: ['mq_b'], hint: 'Warm up the MQ137, then save its clean-air baseline.' },
  { unit: 'cm',      gpio: '16/17', fields: ['us_off'], hint: 'Measure a fixed target distance and adjust the ultrasonic offset.' },
  { unit: 'cm',      gpio: '18/19', fields: ['us_off'], hint: 'Keep the sensor perpendicular to the water surface while calibrating.' },
  { unit: 'cm',      gpio: '21/22', fields: ['us_off'], hint: 'Verify the empty and target water levels before saving the offset.' },
];
const calibrationFields = {
  slope:  { label: 'Slope',             step: '0.0001', fallback: 1 },
  offset: { label: 'Offset',            step: '0.0001', fallback: 0 },
  us_off: { label: 'Ultrasonic offset', step: '0.01',   fallback: 0 },
  tds_k:  { label: 'TDS constant',      step: '0.001',  fallback: 1 },
  mq_b:   { label: 'MQ baseline',       step: '0.001',  fallback: 1 },
};

function finiteNumber(value) {
  if (value === null || value === undefined || value === '') return null;
  const number = Number(value);
  return Number.isFinite(number) ? number : null;
}

function numberText(value, digits = 2) {
  const number = finiteNumber(value);
  return number === null ? '--' : number.toFixed(digits);
}

function calibrationInputs(config, cal, sensorId) {
  return (config.fields || ['slope', 'offset']).map((key) => {
    const field = calibrationFields[key];
    const value = cal[key] ?? field.fallback;
    return `<label>${field.label}<input type="number" step="${field.step}" value="${value}" data-cal="${key}" data-id="${sensorId}"></label>`;
  }).join('');
}

function renderSensors(msg) {
  if (!msg || !Array.isArray(msg.data)) return;
  const list = $('sensor-list');
  const section = $('tab-sensors');
  section?.classList.add('has-data');
  if (!sensorMeta.length) {
    msg.data.forEach(d => sensorMeta.push(d));
    list.innerHTML = '';
    msg.data.forEach((d, index) => {
      const config = sensorConfig[d.id] || sensorConfig[index] || { unit: '', gpio: '--', hint: 'Follow the sensor manufacturer calibration procedure.' };
      const cal = d.cal || {};
      const card = document.createElement('div');
      card.className = 'card sensor-cal-card';
      card.dataset.sensorId = d.id;
      card.innerHTML = `
        <div class="sensor-cal-head">
          <div><span class="sensor-number">S${index + 1}</span><h4>${d.name}</h4><small>GPIO ${config.gpio}</small></div>
          <span id="st-${d.id}" class="sensor-state muted">Waiting</span>
        </div>
        <div class="sensor-live-value"><strong id="val-${d.id}">--</strong><span>${config.unit}</span><small>Raw ADC / input: <code id="raw-${d.id}">--</code></small></div>
        <svg class="sensor-mini-chart" data-full-chart="${d.id}" viewBox="0 0 220 54" preserveAspectRatio="none" aria-label="Recent ${d.name} readings"><path d="M0 40 L220 40"></path></svg>
        <details>
          <summary>Calibration controls</summary>
          <p class="cal-hint">${config.hint}</p>
          <div class="cal-grid">${calibrationInputs(config, cal, d.id)}</div>
          <div class="row"><button data-reset="${d.id}">Reset calibration</button><button data-apply="${d.id}" class="primary">Save calibration</button></div>
        </details>`;
      list.appendChild(card);
    });
    const series = $('sensor-chart-series');
    if (series) {
      series.innerHTML = msg.data.map((sensor) => `<option value="${sensor.id}">${sensor.name}</option>`).join('');
      series.addEventListener('change', drawChart);
    }
    list.addEventListener('click', (e) => {
      const resetId = e.target.getAttribute('data-reset');
      const applyId = e.target.getAttribute('data-apply');
      if (resetId !== null) sendWs('esp1', { type: 'reset_cal', id: Number(resetId) });
      if (applyId !== null) {
        const cal = {};
        document.querySelectorAll(`input[data-cal][data-id="${applyId}"]`)
          .forEach(i => cal[i.getAttribute('data-cal')] = Number(i.value));
        sendWs('esp1', { type: 'set_cal', id: Number(applyId), ...cal });
      }
    });
  }

  // Update existing fields in place on every message, including the first.
  msg.data.forEach(d => {
    if ($('raw-'+d.id)) $('raw-'+d.id).textContent = numberText(d.raw, 3);
    if ($('val-'+d.id)) $('val-'+d.id).textContent = numberText(d.value, 2);
    const st = $('st-'+d.id);
    if (st) {
      st.textContent = d.present ? (d.error ? 'Needs attention' : 'Calibrated') : 'Not detected';
      st.className = `sensor-state ${d.present ? (d.error ? 'err' : 'present') : 'absent'}`;
    }
  });

  // Update rolling history.
  const row = { t: Date.now() };
  msg.data.forEach(d => row[d.id] = finiteNumber(d.value));
  sensorHistory.push(row);
  while (sensorHistory.length > SENSOR_HISTORY_LIMIT) sensorHistory.shift();
  drawChart();
  drawSensorMiniCharts();
  const detected = msg.data.filter((sensor) => sensor.present).length;
  const attention = msg.data.filter((sensor) => sensor.present && sensor.error).length;
  const timestamp = new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  if ($('sensor-configured-count')) $('sensor-configured-count').textContent = String(msg.data.length);
  if ($('sensor-detected-count')) $('sensor-detected-count').textContent = `${detected} / ${msg.data.length}`;
  if ($('sensor-health-label')) $('sensor-health-label').textContent = attention ? 'Needs attention' : (detected === msg.data.length ? 'All online' : 'Board online');
  if ($('sensor-health-note')) $('sensor-health-note').textContent = attention ? `${attention} sensor${attention === 1 ? '' : 's'} need calibration` : `${detected} sensor${detected === 1 ? '' : 's'} detected`;
  if ($('sensor-last-sample')) $('sensor-last-sample').textContent = timestamp;
  const status = $('sensor-page-status');
  if (status) status.textContent = `Live · updated ${timestamp}`;
  document.dispatchEvent(new CustomEvent('aquagrow:sensors', { detail: { data: msg.data, history: sensorHistory.slice() } }));
}

function drawSensorMiniCharts() {
  sensorMeta.forEach((meta) => {
    const svg = document.querySelector(`[data-full-chart="${meta.id}"]`);
    if (!svg) return;
    const values = sensorHistory.map((row) => finiteNumber(row[meta.id])).filter((value) => value !== null);
    if (values.length < 2) return;
    const lo = Math.min(...values), hi = Math.max(...values);
    const span = hi - lo || 1;
    const points = values.map((value, index) => `${(index / Math.max(values.length - 1, 1)) * 220},${48 - ((value - lo) / span) * 38}`).join(' ');
    svg.innerHTML = `<polyline points="${points}"></polyline>`;
  });
}

function drawChart() {
  const c = $('sensorChart');
  if (!c || !sensorMeta.length) return;
  const w = Math.floor(c.clientWidth), h = Math.floor(c.clientHeight);
  if (!w || !h) return;
  const ratio = Math.min(window.devicePixelRatio || 1, 2);
  const pixelWidth = Math.floor(w * ratio), pixelHeight = Math.floor(h * ratio);
  if (c.width !== pixelWidth || c.height !== pixelHeight) {
    c.width = pixelWidth;
    c.height = pixelHeight;
  }
  const ctx = c.getContext('2d');
  ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  ctx.clearRect(0, 0, w, h);
  ctx.strokeStyle = 'rgba(32, 122, 145, 0.12)'; ctx.lineWidth = 1;
  for (let i = 1; i < 5; ++i) { const y = h * i / 5; ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke(); }

  const selectedId = $('sensor-chart-series')?.value ?? String(sensorMeta[0].id);
  const metaIndex = Math.max(0, sensorMeta.findIndex((meta) => String(meta.id) === String(selectedId)));
  const meta = sensorMeta[metaIndex];
  const values = sensorHistory.map((row) => finiteNumber(row[meta.id])).filter((value) => value !== null);
  const range = $('sensor-chart-range');
  if (!values.length) {
    if (range) range.textContent = `Waiting for ${meta.name} samples`;
    return;
  }
  let lo = Math.min(...values), hi = Math.max(...values);
  if (lo === hi) { lo -= 1; hi += 1; }
  const span = hi - lo;
  ctx.strokeStyle = sensorColors[metaIndex % sensorColors.length];
  ctx.lineWidth = 2.25;
  ctx.lineJoin = 'round';
  ctx.lineCap = 'round';
  ctx.beginPath();
  values.forEach((value, index) => {
    const x = values.length === 1 ? w / 2 : (index / (values.length - 1)) * w;
    const y = h - 12 - ((value - lo) / span) * (h - 24);
    if (index === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
  });
  ctx.stroke();
  if (values.length === 1) {
    ctx.fillStyle = sensorColors[metaIndex % sensorColors.length];
    ctx.beginPath(); ctx.arc(w / 2, h / 2, 4, 0, Math.PI * 2); ctx.fill();
  }
  const config = sensorConfig[meta.id] || sensorConfig[metaIndex] || { unit: '' };
  if (range) range.textContent = `${meta.name}: ${Math.min(...values).toFixed(2)}–${Math.max(...values).toFixed(2)} ${config.unit} · ${values.length} sample${values.length === 1 ? '' : 's'}`;
}

/* ----------------------- Relay 16 panel ----------------------- */
let relay16State = [];
function buildRelayGrid(container, count, onAction, isOn) {
  container.innerHTML = '';
  for (let i = 1; i <= count; ++i) {
    const el = document.createElement('div');
    el.className = 'relay';
    el.id = `${container.id}-ch${i}`;
    el.dataset.ch = String(i);
    el.innerHTML = `
      <div class="row"><span class="ch">Ch ${i}</span><span class="state">--</span></div>
      <div class="name">--</div>
      <div class="gpio">GPIO --</div>
      <div class="row">
        <button data-on="${i}">ON</button>
        <button data-off="${i}" class="danger">OFF</button>
        <button data-test="${i}" title="Turns ON briefly, then automatically turns OFF">Pulse 0.8s</button>
      </div>`;
    container.appendChild(el);
  }
  container.addEventListener('click', (e) => {
    const on  = e.target.getAttribute('data-on');
    const off = e.target.getAttribute('data-off');
    const t   = e.target.getAttribute('data-test');
    if (on)  onAction('set', Number(on), true);
    if (off) onAction('set', Number(off), false);
    if (t)   onAction('test', Number(t));
  });
}

function renderRelay16(msg) {
  if (!msg || !Array.isArray(msg.relays)) return;
  relay16State = msg.relays;
  msg.relays.forEach(r => {
    const el = $('r16-grid-ch' + r.ch);
    if (!el) return;
    el.classList.toggle('on', !!r.on);
    el.querySelector('.name').textContent = r.name;
    el.querySelector('.gpio').textContent = 'GPIO ' + r.gpio;
    const st = el.querySelector('.state'); st.textContent = r.on ? 'ON' : 'OFF';
    st.classList.toggle('on', !!r.on);
  });
  document.dispatchEvent(new CustomEvent('aquagrow:relay16', { detail: msg.relays }));
}
buildRelayGrid($('r16-grid'), 16, (type, ch, on=true) => {
  if (type === 'set')   sendWs('esp2', { type: 'set', channel: ch, on });
  if (type === 'test')  sendWs('esp2', { type: 'test_channel', channel: ch, duration_ms: 800 });
}, () => false);
$('r16-all-off').addEventListener('click', () => sendWs('esp2', { type: 'all_off' }));
$('r16-test-seq').addEventListener('click', () => sendWs('esp2', {
  type: 'test_sequential',
  duration_ms: Number($('r16-test-dur').value),
  gap_ms:      Number($('r16-test-gap').value),
}));
$('r16-all-on').addEventListener('click', () => {
  if (confirm('This will turn ON ALL 16 relays. Continue?')) {
    sendWs('esp2', { type: 'all_on', confirm: true });
  }
});
$('r16-emergency').addEventListener('click', () => {
  if (confirm('EMERGENCY STOP - turn OFF every relay on ESP32 #2?')) {
    sendWs('esp2', { type: 'emergency_stop' });
  }
});

/* ----------------------- Relay 8+4 panel ----------------------- */
function renderRelay84(msg) {
  if (!msg || !Array.isArray(msg.relays)) return;
  msg.relays.forEach(r => {
    const sel = r.board === '8ch' ? 'r84-grid-8' : 'r84-grid-4';
    const localChannel = r.board === '8ch' ? r.ch : r.ch - 8;
    const el  = document.getElementById(`${sel}-ch${localChannel}`);
    if (!el) return;
    el.classList.toggle('on', !!r.on);
    el.querySelector('.name').textContent = r.name;
    el.querySelector('.gpio').textContent = 'GPIO ' + r.gpio;
    const st = el.querySelector('.state'); st.textContent = r.on ? 'ON' : 'OFF';
    st.classList.toggle('on', !!r.on);
  });
  document.dispatchEvent(new CustomEvent('aquagrow:relay84', { detail: msg.relays }));
}
buildRelayGrid($('r84-grid-8'), 8,  (type, ch, on=true) => {
  if (type === 'set')  sendWs('esp3', { type: 'set', channel: ch, on });
  if (type === 'test') sendWs('esp3', { type: 'test_channel', channel: ch, duration_ms: 800 });
}, () => false);
buildRelayGrid($('r84-grid-4'), 4,  (type, ch, on=true) => {
  if (type === 'set')  sendWs('esp3', { type: 'set', channel: ch + 8, on });
  if (type === 'test') sendWs('esp3', { type: 'test_channel', channel: ch + 8, duration_ms: 800 });
}, () => false);
$('r84-board8-off').addEventListener('click', () => sendWs('esp3', { type: 'all_off_board', board: '8ch' }));
$('r84-board4-off').addEventListener('click', () => sendWs('esp3', { type: 'all_off_board', board: '4ch' }));
$('r84-all-off').addEventListener('click',    () => sendWs('esp3', { type: 'all_off' }));
$('r84-emergency').addEventListener('click',  () => {
  if (confirm('EMERGENCY STOP - turn OFF every relay on ESP32 #3?')) {
    sendWs('esp3', { type: 'emergency_stop' });
  }
});

/* ----------------------- Extras panel ----------------------- */
let extrasState = null;
function renderExtras(msg) {
  extrasState = msg;
  if (msg.servos) {
    $('pan-min').value   = msg.servos.pan.min;
    $('pan-center').value= msg.servos.pan.center;
    $('pan-max').value   = msg.servos.pan.max;
    $('tilt-min').value  = msg.servos.tilt.min;
    $('tilt-center').value=msg.servos.tilt.center;
    $('tilt-max').value  = msg.servos.tilt.max;
  }
  if (msg.stepper) {
    $('step-pos').textContent = msg.stepper.position;
  }
  if (msg.buzzer) {
    const state = $('dash-buzzer-state');
    if (state) { state.textContent = msg.buzzer.on ? 'ON' : 'OFF'; state.classList.toggle('on', !!msg.buzzer.on); }
  }
  const extrasStatus = $('extras-page-status');
  if (extrasStatus) extrasStatus.textContent = 'Live state received';
  document.dispatchEvent(new CustomEvent('aquagrow:extras', { detail: msg }));
}
function bindServo(prefix) {
  const slider = document.getElementById(prefix + '-angle');
  const val    = document.getElementById(prefix + '-angle-v');
  slider.addEventListener('input',  () => { val.textContent = slider.value; });
  slider.addEventListener('change', () => sendWs('esp4', { type: 'servo_set', servo: prefix, angle: Number(slider.value) }));
  document.getElementById(prefix + '-center-btn').addEventListener('click', () => {
    sendWs('esp4', { type: 'servo_set', servo: prefix, angle: Number(document.getElementById(prefix + '-center').value) });
  });
  document.getElementById(prefix + '-sweep').addEventListener('click', () => sendWs('esp4', { type: 'servo_sweep', servo: prefix }));
  document.getElementById(prefix + '-save').addEventListener('click', () => sendWs('esp4', {
    type: 'servo_cal', servo: prefix,
    min:    Number(document.getElementById(prefix + '-min').value),
    center: Number(document.getElementById(prefix + '-center').value),
    max:    Number(document.getElementById(prefix + '-max').value),
  }));
}
['pan','tilt'].forEach(bindServo);

$('buzzer-on').addEventListener('click',     () => sendWs('esp4', { type: 'buzzer', on: true,  duration_ms: Number($('buzzer-dur').value) }));
$('buzzer-off').addEventListener('click',    () => sendWs('esp4', { type: 'buzzer', on: false }));
$('buzzer-pattern').addEventListener('click',() => sendWs('esp4', { type: 'buzzer_pattern' }));

$('red-on').addEventListener('click',    () => sendWs('esp4', { type: 'led', led: 'red',   on: true,  interval_ms: Number($('red-interval').value) }));
$('red-off').addEventListener('click',   () => sendWs('esp4', { type: 'led', led: 'red',   on: false }));
$('red-blink').addEventListener('click', () => sendWs('esp4', { type: 'led_blink_test', led: 'red' }));
$('green-on').addEventListener('click',  () => sendWs('esp4', { type: 'led', led: 'green', on: true,  interval_ms: Number($('green-interval').value) }));
$('green-off').addEventListener('click', () => sendWs('esp4', { type: 'led', led: 'green', on: false }));
$('green-blink').addEventListener('click',()=> sendWs('esp4', { type: 'led_blink_test', led: 'green' }));

$('step-enable').addEventListener('click', () => sendWs('esp4', { type: 'stepper_enable', on: true }));
$('step-disable').addEventListener('click',() => sendWs('esp4', { type: 'stepper_enable', on: false }));
$('step-move').addEventListener('click',   () => {
  const steps = Number($('step-steps').value);
  const sign  = $('step-dir').value === 'ccw' ? -1 : 1;
  sendWs('esp4', { type: 'stepper_move',
    steps: steps * sign, speed: Number($('step-speed').value),
    microstep: Number($('step-ms').value) });
});
$('step-stop').addEventListener('click',   () => sendWs('esp4', { type: 'stepper_stop' }));
$('step-cal').addEventListener('click',    () => sendWs('esp4', { type: 'stepper_cal' }));
$('step-reset').addEventListener('click',  () => sendWs('esp4', { type: 'stepper_reset' }));
$('extras-emergency').addEventListener('click', () => {
  if (confirm('Emergency stop ESP32 #4 and disable all motion and outputs?')) {
    sendWs('esp4', { type: 'emergency_stop' });
  }
});

/* ----------------------- Logs panel ----------------------- */
$('log-clear').addEventListener('click', () => {
  logEntries.length = 0;
  renderLogView();
  document.dispatchEvent(new CustomEvent('aquagrow:logs-cleared'));
});
$('log-download').addEventListener('click',() => {
  const blob = new Blob([logEntries.map((entry) => `[${entry.timestamp}] ${entry.message}`).join('\n')], { type: 'text/plain' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'calibrator.log';
  a.click();
});

/* ----------------------- Tab switching ----------------------- */
function openPanel(name, activeButton = null) {
  const panel = $('tab-' + name);
  if (!panel) return;
  document.querySelectorAll('.tab').forEach(b => b.classList.remove('active'));
  document.querySelectorAll('.panel').forEach(p => p.classList.remove('active'));
  panel.classList.add('active');
  const btn = activeButton || document.querySelector(`.tab[data-tab="${name}"]`);
  if (btn) btn.classList.add('active');
  window.scrollTo({ top: 0, behavior: matchMedia('(prefers-reduced-motion: reduce)').matches ? 'auto' : 'smooth' });
  document.dispatchEvent(new CustomEvent('aquagrow:panel', { detail: name }));
}
document.querySelectorAll('.tab').forEach(btn => btn.addEventListener('click', () => openPanel(btn.dataset.tab, btn)));
document.addEventListener('aquagrow:panel', (event) => {
  if (event.detail === 'sensors' && sensorHistory.length) requestAnimationFrame(drawChart);
});
window.addEventListener('resize', () => {
  if ($('tab-sensors')?.classList.contains('active') && sensorHistory.length) requestAnimationFrame(drawChart);
}, { passive: true });

/* ----------------------- Periodic refresh ----------------------- */
setInterval(() => {
  if (boards.esp1.ws && boards.esp1.ws.readyState === 1) sendWs('esp1', { type: 'get_sensors' });
  if (boards.esp2.ws && boards.esp2.ws.readyState === 1) sendWs('esp2', { type: 'get_state' });
  if (boards.esp3.ws && boards.esp3.ws.readyState === 1) sendWs('esp3', { type: 'get_state' });
  if (boards.esp4.ws && boards.esp4.ws.readyState === 1) sendWs('esp4', { type: 'get_state' });
}, 2000);

log('AquaGrow 365 Calibrator ready - connecting...');

  /* Expose the most commonly used hooks so other scripts (e.g. the home
   * dashboard glue layer) can react to state changes without re-implementing
   * the WebSocket / REST plumbing. */
  window.AG365 = {
    boards, sendWs, log, badge,
    connectBoard,    connectBoard,
    disconnectBoard, disconnectBoard,
    handleMessage,   handleMessage,
    requestState,    requestState,
    openPanel, renderLogView, logEntries,
  };
  window.boards       = boards;
  window.connectBoard = connectBoard;
