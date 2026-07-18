/* Progressive interface enhancements for navigation, instructions and logs. */
'use strict';

(function () {
  const $ = (id) => document.getElementById(id);

  function ready(fn) {
    if (document.readyState !== 'loading') fn();
    else document.addEventListener('DOMContentLoaded', fn, { once: true });
  }

  ready(() => {
    setupMobileNavigation();
    setupIndividualConnections();
    setupInstructionChecklist();
    setupSensorNavigation();
    setupLogFilters();
    setupServoDials();
    setupPanelState();
  });

  function setupMobileNavigation() {
    const toggle = $('nav-toggle');
    const tabs = $('tabs');
    if (!toggle || !tabs) return;
    const close = () => {
      tabs.classList.remove('is-open');
      toggle.setAttribute('aria-expanded', 'false');
      toggle.setAttribute('aria-label', 'Open navigation');
    };
    toggle.addEventListener('click', () => {
      const open = tabs.classList.toggle('is-open');
      toggle.setAttribute('aria-expanded', String(open));
      toggle.setAttribute('aria-label', open ? 'Close navigation' : 'Open navigation');
    });
    tabs.addEventListener('click', (event) => { if (event.target.closest('.tab')) close(); });
    document.addEventListener('keydown', (event) => { if (event.key === 'Escape') close(); });
  }

  function setupIndividualConnections() {
    document.addEventListener('click', (event) => {
      const connect = event.target.closest('[data-connect-board]');
      const disconnect = event.target.closest('[data-disconnect-board]');
      if (connect) window.AG365?.connectBoard(connect.dataset.connectBoard);
      if (disconnect) window.AG365?.disconnectBoard(disconnect.dataset.disconnectBoard);
    });
  }

  function setupInstructionChecklist() {
    const steps = [...document.querySelectorAll('.instructions .step')];
    if (!steps.length) return;
    let saved = {};
    try { saved = JSON.parse(localStorage.getItem('aquagrow.setup-progress') || '{}'); } catch (_) { saved = {}; }

    steps.forEach((step, index) => {
      const content = step.children[1];
      if (!content) return;
      content.classList.add('step-content');
      const heading = content.querySelector('h3');
      const controls = document.createElement('div');
      controls.className = 'step-actions';
      controls.innerHTML = `
        <label class="completion-check"><input type="checkbox" ${saved[index] ? 'checked' : ''}><span>Complete</span></label>
        <button type="button" class="step-toggle" aria-expanded="true">Collapse</button>`;
      heading?.insertAdjacentElement('afterend', controls);
      step.classList.toggle('is-complete', Boolean(saved[index]));

      controls.querySelector('input').addEventListener('change', (event) => {
        saved[index] = event.target.checked;
        step.classList.toggle('is-complete', event.target.checked);
        try { localStorage.setItem('aquagrow.setup-progress', JSON.stringify(saved)); } catch (_) {}
        updateProgress(steps);
      });
      controls.querySelector('.step-toggle').addEventListener('click', (event) => {
        const collapsed = step.classList.toggle('collapsed');
        event.currentTarget.textContent = collapsed ? 'Expand' : 'Collapse';
        event.currentTarget.setAttribute('aria-expanded', String(!collapsed));
      });

      content.querySelectorAll('code').forEach((code) => {
        if (code.closest('a') || code.closest('.library-list') || code.textContent.trim().length < 8) return;
        const button = document.createElement('button');
        button.className = 'copy-button';
        button.type = 'button';
        button.textContent = 'Copy';
        button.setAttribute('aria-label', `Copy ${code.textContent.trim()}`);
        button.addEventListener('click', async () => {
          const text = code.textContent.trim();
          try {
            await navigator.clipboard.writeText(text);
          } catch (_) {
            const area = document.createElement('textarea');
            area.value = text;
            area.style.position = 'fixed';
            area.style.opacity = '0';
            document.body.appendChild(area);
            area.select();
            document.execCommand('copy');
            area.remove();
          }
          button.textContent = 'Copied';
          setTimeout(() => { button.textContent = 'Copy'; }, 1400);
        });
        code.insertAdjacentElement('afterend', button);
      });
    });
    setupInstructionMasonry(steps);
    updateProgress(steps);
  }

  function setupInstructionMasonry(steps) {
    const grid = document.querySelector('.instructions');
    if (!grid || !steps.length) return;
    const desktop = window.matchMedia('(min-width: 761px)');
    let frame = 0;

    const layout = () => {
      frame = 0;
      if (!desktop.matches) {
        grid.classList.remove('is-masonry');
        steps.forEach((step) => {
          step.style.removeProperty('grid-column');
          step.style.removeProperty('grid-row-start');
          step.style.removeProperty('grid-row-end');
        });
        return;
      }
      grid.classList.add('is-masonry');
      const styles = getComputedStyle(grid);
      const rowHeight = Number.parseFloat(styles.gridAutoRows) || 8;
      const rowGap = Number.parseFloat(styles.rowGap) || 12;
      const nextRow = [1, 1];
      steps.forEach((step) => {
        // Always place the next numbered step in the shorter column. Because
        // column heights only increase, the visual top positions stay in the
        // same order as the DOM: 1, 2, 3, 4, 5, 6.
        const column = nextRow[0] <= nextRow[1] ? 0 : 1;
        const height = step.getBoundingClientRect().height;
        const span = Math.max(1, Math.ceil((height + rowGap) / (rowHeight + rowGap)));
        step.style.gridColumn = String(column + 1);
        step.style.gridRowStart = String(nextRow[column]);
        step.style.gridRowEnd = `span ${span}`;
        nextRow[column] += span;
      });
    };
    const schedule = () => {
      if (frame) cancelAnimationFrame(frame);
      frame = requestAnimationFrame(layout);
    };

    if ('ResizeObserver' in window) {
      const observer = new ResizeObserver(schedule);
      steps.forEach((step) => observer.observe(step));
    }
    desktop.addEventListener?.('change', schedule);
    window.addEventListener('resize', schedule, { passive: true });
    schedule();
  }

  function updateProgress(steps) {
    const complete = steps.filter((step) => step.classList.contains('is-complete')).length;
    const label = $('setup-progress-label');
    const bar = $('setup-progress-bar');
    if (label) label.textContent = `${complete} of ${steps.length} complete`;
    if (bar) bar.style.width = `${(complete / steps.length) * 100}%`;
  }

  function setupSensorNavigation() {
    ['sensor-manage-connection', 'sensor-empty-connect'].forEach((id) => {
      $(id)?.addEventListener('click', () => window.AG365?.openPanel('connect'));
    });
  }

  function setupLogFilters() {
    ['log-search', 'log-board-filter', 'log-type-filter'].forEach((id) => {
      $(id)?.addEventListener(id === 'log-search' ? 'input' : 'change', () => window.AG365?.renderLogView());
    });
  }

  function setupServoDials() {
    ['pan', 'tilt'].forEach((servo) => {
      const slider = $(`${servo}-angle`);
      const dial = document.querySelector(`[data-servo-gauge="${servo}"]`);
      const value = $(`${servo}-dial-value`);
      if (!slider || !dial) return;
      const repaint = () => {
        const visualAngle = (Number(slider.value) / Number(slider.max || 270)) * 180;
        dial.style.setProperty('--servo-angle', `${visualAngle}deg`);
        if (value) value.textContent = `${slider.value}\u00B0`;
      };
      repaint();
      slider.addEventListener('input', repaint);
    });
  }

  function setupPanelState() {
    document.addEventListener('aquagrow:panel', (event) => {
      const name = event.detail;
      document.body.dataset.panel = name;
      if (name === 'connect') document.querySelector('.tab[data-tab="home"]')?.classList.add('active');
    });
  }
})();
