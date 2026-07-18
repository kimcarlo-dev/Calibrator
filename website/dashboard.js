/* AquaGrow 365 home dashboard
 * Mirrors live device state from app.js into the visual overview without
 * duplicating protocol or firmware commands.
 */
'use strict';

(function () {
  const $ = (id) => document.getElementById(id);
  const boardIds = ['esp1', 'esp2', 'esp3', 'esp4'];
  const previewHistory = { ph: [], tds: [], temp: [], do: [], level: [], ammonia: [] };
  const startedAt = Date.now();

  function ready(fn) {
    if (document.readyState !== 'loading') fn();
    else document.addEventListener('DOMContentLoaded', fn, { once: true });
  }

  ready(() => {
    buildRelayPreview($('dash-relay16-mini'), 16, 'esp2', 0);
    buildRelayPreview($('dash-relay84-8'), 8, 'esp3', 0);
    buildRelayPreview($('dash-relay84-4'), 4, 'esp3', 8);
    wireNavigation();
    wireDashboardActions();
    wireServoPreview('pan');
    wireServoPreview('tilt');
    syncAutoConnect();
    observeConnectionCards();
    enableHeroTilt();

    document.addEventListener('aquagrow:sensors', (event) => renderSensorPreview(event.detail));
    document.addEventListener('aquagrow:relay16', (event) => renderRelayPreview('esp2', event.detail));
    document.addEventListener('aquagrow:relay84', (event) => renderRelayPreview('esp3', event.detail));
    document.addEventListener('aquagrow:extras', (event) => renderExtrasPreview(event.detail));
    document.addEventListener('aquagrow:log', (event) => addFeedEntry(event.detail));
    document.addEventListener('aquagrow:logs-cleared', clearFeed);

    paintConnections();
    paintKpis();
    setInterval(() => {
      paintConnections();
      paintKpis();
      updateUptime();
    }, 1000);
  });

  function openPanel(name) {
    if (window.AG365?.openPanel) window.AG365.openPanel(name);
  }

  function wireNavigation() {
    $('brand-home')?.addEventListener('click', () => openPanel('home'));
    $('dash-manage-connection')?.addEventListener('click', () => openPanel('connect'));
    $('connection-back')?.addEventListener('click', () => openPanel('home'));
    $('dash-sensors-open')?.addEventListener('click', () => openPanel('sensors'));
    $('dash-r16-open')?.addEventListener('click', () => openPanel('relay16'));
    $('dash-r84-open')?.addEventListener('click', () => openPanel('relay84'));
    $('dash-extras-open')?.addEventListener('click', () => openPanel('extras'));
    $('dash-feed-open')?.addEventListener('click', () => openPanel('logs'));
  }

  function clickProxy(sourceId, targetId) {
    $(sourceId)?.addEventListener('click', () => $(targetId)?.click());
  }

  function wireDashboardActions() {
    clickProxy('dash-connect-all', 'btn-connect-all');
    clickProxy('dash-disconnect-all', 'btn-disconnect-all');
    clickProxy('dash-r16-all-off', 'r16-all-off');
    clickProxy('dash-r16-test-seq', 'r16-test-seq');
    clickProxy('dash-r16-all-on', 'r16-all-on');
    clickProxy('dash-r16-emergency', 'r16-emergency');
    clickProxy('dash-r84-board8-off', 'r84-board8-off');
    clickProxy('dash-r84-board4-off', 'r84-board4-off');
    clickProxy('dash-r84-all-off', 'r84-all-off');
    clickProxy('dash-r84-emergency', 'r84-emergency');
    clickProxy('dash-buzzer-on', 'buzzer-on');
    clickProxy('dash-buzzer-off', 'buzzer-off');
    clickProxy('dash-buzzer-pattern', 'buzzer-pattern');
    clickProxy('dash-red-blink', 'red-blink');
    clickProxy('dash-green-blink', 'green-blink');

    $('dash-scan')?.addEventListener('click', () => {
      if (typeof window.kickOffAutoConnect === 'function') window.kickOffAutoConnect();
      else boardIds.forEach((board) => window.AG365?.connectBoard(board));
    });
    $('dash-feed-clear')?.addEventListener('click', () => $('log-clear')?.click());
  }

  function syncAutoConnect() {
    const dashboard = $('dash-auto-connect');
    const settings = $('auto-connect');
    const label = $('dash-auto-label');
    if (!dashboard || !settings) return;
    const repaint = () => {
      dashboard.checked = settings.checked;
      if (label) label.textContent = settings.checked ? 'ON' : 'OFF';
    };
    repaint();
    dashboard.addEventListener('change', () => {
      settings.checked = dashboard.checked;
      settings.dispatchEvent(new Event('change'));
      repaint();
    });
    settings.addEventListener('change', repaint);
  }

  function buildRelayPreview(container, count, board, offset) {
    if (!container || container.dataset.built) return;
    for (let index = 1; index <= count; index += 1) {
      const channel = index + offset;
      const tile = document.createElement('div');
      tile.className = 'r';
      tile.dataset.board = board;
      tile.dataset.ch = String(channel);
      tile.innerHTML = `
        <div class="mini-relay-head"><span class="ch">CH ${channel}</span><span class="led"></span></div>
        <span class="gpio">GPIO --</span>
        <div class="mini-relay-actions">
          <button class="mini-toggle" type="button" aria-label="Toggle relay channel ${channel}"><i></i><span>OFF</span></button>
          <button class="mini-test" type="button" aria-label="Pulse-test relay channel ${channel}" title="Turns ON briefly, then automatically turns OFF">Pulse</button>
        </div>`;
      tile.querySelector('.mini-toggle').addEventListener('click', () => {
        const on = !tile.classList.contains('on');
        window.AG365?.sendWs(board, { type: 'set', channel, on });
      });
      tile.querySelector('.mini-test').addEventListener('click', () => {
        const duration = Number($('r16-test-dur')?.value || 800);
        window.AG365?.sendWs(board, { type: 'test_channel', channel, duration_ms: duration });
        tile.classList.add('testing');
        setTimeout(() => tile.classList.remove('testing'), duration);
      });
      container.appendChild(tile);
    }
    container.dataset.built = '1';
  }

  function renderRelayPreview(board, relays) {
    if (!Array.isArray(relays)) return;
    relays.forEach((relay) => {
      const tile = document.querySelector(`.r[data-board="${board}"][data-ch="${relay.ch}"]`);
      if (!tile) return;
      tile.classList.toggle('on', Boolean(relay.on));
      tile.querySelector('.gpio').textContent = `GPIO ${relay.gpio ?? '--'}`;
      const state = tile.querySelector('.mini-toggle span');
      if (state) state.textContent = relay.on ? 'ON' : 'OFF';
      tile.querySelector('.mini-toggle')?.setAttribute('aria-pressed', String(Boolean(relay.on)));
    });
    paintKpis();
  }

  function sensorKey(sensor) {
    const name = String(sensor.name || '').toLowerCase();
    if (name === 'ph' || name.includes('ph ')) return 'ph';
    if (name.includes('tds')) return 'tds';
    if (name.includes('ds18') || name.includes('temp')) return 'temp';
    if (name === 'do' || name.includes('dissolved')) return 'do';
    if (name.includes('ultrasonic') || name.includes('level')) return 'level';
    if (name.includes('mq137') || name.includes('ammonia') || name.includes('nh3')) return 'ammonia';
    return '';
  }

  function renderSensorPreview(detail) {
    const picked = {};
    (detail?.data || []).forEach((sensor) => {
      const key = sensorKey(sensor);
      if (key && !picked[key]) picked[key] = sensor;
    });

    Object.entries(picked).forEach(([key, sensor]) => {
      const card = document.querySelector(`.sensor-card[data-sensor="${key}"]`);
      if (!card) return;
      const finite = Number.isFinite(Number(sensor.value));
      card.querySelector('.value').textContent = finite ? Number(sensor.value).toFixed(key === 'tds' ? 0 : 2) : '--';
      const status = card.querySelector('.status');
      const dot = card.querySelector('.row2 .dot');
      const healthy = Boolean(sensor.present) && !sensor.error;
      if (status) status.textContent = sensor.present ? (sensor.error ? 'Needs calibration' : 'Calibrated') : 'Not detected';
      dot?.classList.toggle('live', healthy);
      card.classList.toggle('has-data', finite);
      if (finite) {
        previewHistory[key].push(Number(sensor.value));
        while (previewHistory[key].length > 22) previewHistory[key].shift();
        drawSparkline(card, previewHistory[key]);
      }
    });
    const updated = $('dash-sensors-updated');
    if (updated) updated.textContent = `Updated ${new Date().toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })}`;
  }

  function drawSparkline(card, values) {
    if (values.length < 2) return;
    const svg = card.querySelector('.sparkline');
    if (!svg) return;
    const lo = Math.min(...values), hi = Math.max(...values), span = hi - lo || 1;
    const points = values.map((value, index) => `${(index / (values.length - 1)) * 100},${23 - ((value - lo) / span) * 18}`).join(' ');
    svg.innerHTML = `<polyline points="${points}"></polyline>`;
  }

  function wireServoPreview(servo) {
    const slider = $(`dash-${servo}-angle`);
    const value = $(`dash-${servo}-value`);
    const gauge = document.querySelector(`.mini-gauge[data-servo="${servo}"]`);
    if (!slider) return;
    const update = () => {
      if (value) value.textContent = `${slider.value}\u00B0`;
      const visualAngle = (Number(slider.value) / Number(slider.max || 270)) * 180;
      gauge?.style.setProperty('--servo-angle', `${visualAngle}deg`);
    };
    update();
    slider.addEventListener('input', update);
    slider.addEventListener('change', () => {
      const full = $(`${servo}-angle`);
      if (full) {
        full.value = slider.value;
        full.dispatchEvent(new Event('input'));
        full.dispatchEvent(new Event('change'));
      }
    });
    document.querySelector(`[data-servo-action="${servo}-center"]`)?.addEventListener('click', () => $(`${servo}-center-btn`)?.click());
    document.querySelector(`[data-servo-action="${servo}-sweep"]`)?.addEventListener('click', () => $(`${servo}-sweep`)?.click());
  }

  function renderExtrasPreview(state) {
    if (state?.buzzer) {
      const chip = $('dash-buzzer-state');
      if (chip) {
        chip.textContent = state.buzzer.on ? 'ON' : 'OFF';
        chip.classList.toggle('on', Boolean(state.buzzer.on));
      }
    }
  }

  function observeConnectionCards() {
    const grid = $('conn-status');
    if (!grid) return;
    new MutationObserver(paintConnections).observe(grid, { childList: true, characterData: true, attributes: true, subtree: true });
  }

  function paintConnections() {
    boardIds.forEach((board) => {
      const source = document.querySelector(`.conn-card[data-board="${board}"]`);
      const card = document.querySelector(`.device-card[data-board="${board}"]`);
      if (!source || !card) return;
      const status = (source.querySelector('.badge')?.textContent || 'unknown').trim().toLowerCase();
      card.classList.remove('is-online', 'is-connecting', 'is-error');
      if (status === 'connected') card.classList.add('is-online');
      else if (status === 'connecting') card.classList.add('is-connecting');
      else if (status === 'error' || status === 'failed') card.classList.add('is-error');
      const label = card.querySelector('.lbl');
      if (label) label.textContent = status;
      const ip = $(`dash-ip-${board}`);
      if (ip) ip.textContent = $(`ip-${board}`)?.value.trim() || '--';
    });

    const online = document.querySelectorAll('.device-card.is-online').length;
    const status = $('system-status');
    if (status) {
      status.classList.toggle('is-online', online === 4);
      status.classList.toggle('is-partial', online > 0 && online < 4);
      status.classList.toggle('is-offline', online === 0);
      const strong = status.querySelector('strong');
      const small = status.querySelector('small');
      if (online === 4) {
        strong.textContent = 'System Online'; small.textContent = 'All systems operational';
      } else if (online > 0) {
        strong.textContent = 'Partially Online'; small.textContent = `${online} of 4 boards connected`;
      } else {
        strong.textContent = 'System Offline'; small.textContent = 'Waiting for ESP32 boards';
      }
    }
    if ($('stat-boards')) $('stat-boards').textContent = String(online);
    if ($('stat-boards-note')) {
      $('stat-boards-note').textContent = online === 4
        ? 'All connected'
        : online > 0
          ? `${online} of 4 connected`
          : 'Awaiting boards';
    }
    if ($('kpi-online')) $('kpi-online').textContent = `${online}/4`;
  }

  function paintKpis() {
    const relaysOn = document.querySelectorAll('.relay.on').length;
    const sensorsActive = document.querySelectorAll('.sensor-cal-card .sensor-state.present').length;
    if ($('kpi-relays-on')) $('kpi-relays-on').textContent = String(relaysOn);
    if ($('kpi-sensor-count')) $('kpi-sensor-count').textContent = String(sensorsActive);
    if ($('stat-sensors')) $('stat-sensors').textContent = String(sensorsActive);
    if ($('stat-sensors-note')) {
      $('stat-sensors-note').textContent = sensorsActive > 0 ? 'Monitoring' : 'Awaiting data';
    }
  }

  function updateUptime() {
    const seconds = Math.floor((Date.now() - startedAt) / 1000);
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const rest = seconds % 60;
    if ($('kpi-uptime')) $('kpi-uptime').textContent = `${hours ? `${hours}h ` : ''}${minutes}m ${rest}s`;
  }

  function addFeedEntry(entry) {
    const feed = $('dash-feed');
    if (!feed || !entry) return;
    feed.querySelector('.feed-empty')?.remove();
    const item = document.createElement('li');
    const cssType = entry.type === 'warning' ? 'warn' : entry.type === 'connection' ? 'connect' : entry.type;
    item.className = `feed-item ${cssType}`;
    item.innerHTML = `<span class="t">${escapeHtml(entry.timestamp.slice(11))}</span><span class="m">${escapeHtml(entry.message)}</span><span class="src">${escapeHtml(entry.board === 'system' ? 'SYS' : entry.board.toUpperCase())}</span>`;
    feed.appendChild(item);
    while (feed.querySelectorAll('.feed-item').length > 8) feed.querySelector('.feed-item')?.remove();
  }

  function clearFeed() {
    const feed = $('dash-feed');
    if (feed) feed.innerHTML = '<li class="feed-empty">Waiting for events...</li>';
  }

  function escapeHtml(value) {
    return String(value).replace(/[&<>"']/g, (character) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[character]));
  }

  function enableHeroTilt() {
    const hero = $('system-overview');
    const illustration = $('hero-illustration');
    if (!hero || !illustration || matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    hero.addEventListener('pointermove', (event) => {
      const rect = hero.getBoundingClientRect();
      const x = (event.clientX - rect.left) / rect.width - 0.5;
      const y = (event.clientY - rect.top) / rect.height - 0.5;
      illustration.style.transform = `perspective(900px) rotateX(${-y * 2.4}deg) rotateY(${x * 3.4}deg)`;
    });
    hero.addEventListener('pointerleave', () => { illustration.style.transform = ''; });
  }
})();
