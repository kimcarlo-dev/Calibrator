/* Calibrator auto-connect
 *  - Loads the saved IPs from localStorage and connects as soon as the
 *    page is ready.
 *  - Uses each board's stable mDNS hostname by default while DHCP assigns
 *    a unique IP address to every ESP32.
 *  - Can discover updated firmware through its HTTP /info identity route.
 *  - Reconnects with exponential backoff if a board drops out.
 *
 *  All of this is opt-out via the "Auto-connect" checkbox in the
 *  Connection tab; if you clear it, the original manual behaviour is
 *  restored.
 */
(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);
  const LS_IPS  = 'calibrator.ips';
  const LS_AUTO = 'calibrator.autoconnect';
  const boardIds = ['esp1','esp2','esp3','esp4'];

  /* ------------- Auto-connect preference ----------------------------- */
  const autoBox = document.getElementById('auto-connect');
  if (autoBox) {
    autoBox.checked = localStorage.getItem(LS_AUTO) !== '0';
    autoBox.addEventListener('change', () => {
      localStorage.setItem(LS_AUTO, autoBox.checked ? '1' : '0');
      if (autoBox.checked) {
        kickOffAutoConnect();
      } else {
        // user just turned it off -> disconnect everything
        for (const b of boardIds) {
          if (typeof disconnectBoard === 'function') disconnectBoard(b);
        }
      }
    });
  }
  const autoEnabled = () => !autoBox || autoBox.checked;

  /* ------------- helper: extract the last octet the user can edit ----- */
  function getBaseSubnet() {
    const any = Object.values(configuredAddresses()).find((value) => /^\d+\.\d+\.\d+\.\d+$/.test(value));
    if (!any) return null;
    const m = String(any).match(/^(\d+\.\d+\.\d+)\.\d+$/);
    return m ? m[1] : null;
  }

  function savedIps() {
    try { return JSON.parse(localStorage.getItem(LS_IPS) || '{}'); }
    catch (_) { return {}; }
  }
  function configuredAddresses() {
    const addresses = savedIps();
    for (const boardId of boardIds) {
      const value = document.getElementById('ip-' + boardId)?.value.trim();
      if (value) addresses[boardId] = value;
    }
    return addresses;
  }
  function writeIps(map) {
    const cur = savedIps();
    const next = { ...cur, ...map };
    localStorage.setItem(LS_IPS, JSON.stringify(next));
    for (const [k, v] of Object.entries(map)) {
      const el = document.getElementById('ip-' + k);
      if (el && v) el.value = v;
    }
  }

  /* ------------- 1. Try mDNS hostnames first -------------------------- */
  async function tryMdns(boardId, attempts = 3) {
    const names = {
      esp1: 'calibrator-esp32-1.local',
      esp2: 'calibrator-esp32-2.local',
      esp3: 'calibrator-esp32-3.local',
      esp4: 'calibrator-esp32-4.local',
    };
    const host = names[boardId];
    for (let i = 0; i < attempts; ++i) {
      try {
        const ctrl = new AbortController();
        const t = setTimeout(() => ctrl.abort(), 800);
        const r = await fetch(`http://${host}/info`, { signal: ctrl.signal });
        clearTimeout(t);
        if (r.ok) {
          const j = await r.json();
          if (j && j.type === 'device_info' && j.device_id === boardId) return host;
        }
      } catch (_) { /* not resolvable, try again */ }
    }
    return null;
  }

  /* ------------- 2. Scan the last /24 subnet for HTTP /info ----------- */
  async function scanSubnet() {
    const base = getBaseSubnet();
    if (!base) return;
    if (location.protocol === 'https:') {
      // mixed-content would block the fetch, skip silently
      return;
    }
    const found = {};
    // Probe in small batches to avoid flooding the browser and local router.
    for (let start = 1; start <= 254 && Object.keys(found).length < 4; start += 24) {
      const tasks = [];
      for (let last = start; last < Math.min(start + 24, 255); ++last) {
        const host = `${base}.${last}`;
        const ctl  = new AbortController();
        const t    = setTimeout(() => ctl.abort(), 600);
        tasks.push(
          fetch(`http://${host}/info`, { signal: ctl.signal })
            .then(r => r.ok ? r.json() : null)
            .then(j => {
              if (j && boardIds.includes(j.device_id)) found[j.device_id] = host;
            })
            .catch(() => {})
            .finally(() => clearTimeout(t))
        );
      }
      await Promise.all(tasks);
    }
    if (Object.keys(found).length) {
      writeIps(found);
      log('Discovered boards on ' + base + '.x: ' + JSON.stringify(found));
    }
  }

  /* ------------- 3. Connect everything we know about ----------------- */
  async function autoConnectAll() {
    if (!autoEnabled()) return;
    const cur = configuredAddresses();
    let any = false;

    // First try mDNS so the user never has to type static IPs.
    const resolved = { ...cur };
    await Promise.all(boardIds.map(async (b) => {
      if (resolved[b]) return;
      const mdns = await tryMdns(b);
      if (mdns) { resolved[b] = mdns; any = true; }
    }));
    if (any || Object.keys(resolved).length) writeIps(resolved);

    // Then probe the local subnet if we still have gaps.
    if (Object.values(resolved).filter(Boolean).length < 4) {
      await scanSubnet();
    }

    // Finally, kick off the connect routine for every board with an IP.
    for (const b of boardIds) {
      const ip = (savedIps()[b] || '').trim();
      if (!ip) continue;
      const el = document.getElementById('ip-' + b);
      if (el) el.value = ip;
      if (typeof connectBoard === 'function') connectBoard(b);
    }
  }

  /* ------------- 4. Auto-reconnect with back-off --------------------- */
  function patchReconnect() {
    // We hook each board's WebSocket by listening on the global mutation
    // event the page already exposes. A simple approach: re-attach by
    // intercepting disconnectBoard/connectBoard and scheduling a retry.
    if (window.__calReconnectPatched) return;
    window.__calReconnectPatched = true;

    const retryState = { esp1:0, esp2:0, esp3:0, esp4:0 };

    document.addEventListener('calibrator:closed', (ev) => {
      const b = ev.detail;
      if (!b || !autoEnabled()) return;
      retryState[b] = Math.min(retryState[b] + 1, 6);
      const delay = Math.min(30000, 500 * (2 ** retryState[b]));
      log(`${b} dropped, retrying in ${delay} ms`);
      setTimeout(() => {
        if (autoEnabled()) connectBoard(b);
      }, delay);
    });

    document.addEventListener('calibrator:open', (ev) => {
      const b = ev.detail;
      if (b) retryState[b] = 0;
    });
  }

  /* ------------- Entry point ---------------------------------------- */
  async function kickOffAutoConnect() {
    if (!autoEnabled()) return;
    log('Auto-connect: looking for ESP32 boards...');
    try { await autoConnectAll(); } catch (e) { log('auto-connect error: ' + e); }
  }

  // Dispatch the helper events from the existing WebSocket onclose/onopen.
  function installBridge() {
    // app.js emits calibrator:open / calibrator:closed directly. Keeping
    // this hook as an explicit no-op preserves older load orders without
    // registering duplicate reconnect events.
    window.__calBridgeInstalled = true;
  }

  document.addEventListener('DOMContentLoaded', () => {
    installBridge();
    patchReconnect();
    // give the rest of the script a tick to wire its handlers
    setTimeout(kickOffAutoConnect, 50);
  });
  // expose entry points for the dashboard glue layer
  window.kickOffAutoConnect = kickOffAutoConnect;
  window.autoConnectAll = autoConnectAll;
  window.boards = boards;
})();

