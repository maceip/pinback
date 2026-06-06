'use strict';

/* Shared native webview bridge for every Pinback shell.
 * See platform/CONTRACT.md. Loaded by setup.html (file://) and the cockpit (ui/app). */

(function (global) {
  function normalizeUrl(u) {
    u = (u || '').trim();
    if (!u) return '';
    if (!/^https?:\/\//i.test(u)) u = 'http://' + u;
    return u.replace(/\/+$/, '');
  }

  /** Bridge for platform/common/setup.html (pre-connection). */
  function setupBridge() {
    if (global.PinbackSetup && typeof global.PinbackSetup.post === 'function') {
      return {
        getSavedUrl: () => (global.PinbackSetup.getSavedUrl && global.PinbackSetup.getSavedUrl()) || '',
        getDefaultUrl: () => (global.PinbackSetup.getDefaultUrl && global.PinbackSetup.getDefaultUrl()) || '',
        send: (msg) => global.PinbackSetup.post(typeof msg === 'string' ? msg : JSON.stringify(msg)),
      };
    }
    if (global.PinbackHost && typeof global.PinbackHost.getSavedUrl === 'function') {
      return {
        getSavedUrl: () => global.PinbackHost.getSavedUrl() || '',
        getDefaultUrl: () => (global.PinbackHost.getDefaultUrl && global.PinbackHost.getDefaultUrl()) || '',
        send: (msg) => global.PinbackHost.post(JSON.stringify(msg)),
      };
    }
    if (global.webkit && global.webkit.messageHandlers && global.webkit.messageHandlers.pinbackSetup) {
      const h = global.webkit.messageHandlers.pinbackSetup;
      return {
        getSavedUrl: () => '',
        getDefaultUrl: () => '',
        send: (msg) => h.postMessage(msg),
      };
    }
    if (global.chrome && global.chrome.webview) {
      return {
        getSavedUrl: () => '',
        getDefaultUrl: () => '',
        send: (msg) => global.chrome.webview.postMessage(msg),
      };
    }
    return null;
  }

  /** Bridge for ui/app cockpit (workspace list, open setup from offline overlay). */
  function cockpitSink() {
    const apple = global.webkit && global.webkit.messageHandlers && global.webkit.messageHandlers.pinback;
    if (apple) return (m) => apple.postMessage(m);
    if (global.PinbackHost && typeof global.PinbackHost.post === 'function')
      return (m) => global.PinbackHost.post(JSON.stringify(m));
    const wv2 = global.chrome && global.chrome.webview;
    if (wv2 && typeof wv2.postMessage === 'function') return (m) => wv2.postMessage(m);
    return null;
  }

  function canOpenServerSetup() {
    if (global.PinbackHost && typeof global.PinbackHost.openServerSettings === 'function')
      return true;
    return !!cockpitSink();
  }

  function openServerSetup() {
    if (global.PinbackHost && typeof global.PinbackHost.openServerSettings === 'function') {
      global.PinbackHost.openServerSettings();
      return;
    }
    const sink = cockpitSink();
    if (sink) sink({ type: 'pinback-host', action: 'openSetup' });
  }

  function initSetupPanel(opts) {
    const bridge = setupBridge();
    const urlEl = opts.urlEl;
    const statusEl = opts.statusEl;
    function setStatus(text, cls) {
      statusEl.textContent = text;
      statusEl.className = cls || 'warn';
    }
    function send(action) {
      const url = normalizeUrl(urlEl.value);
      if (!url) { setStatus('Enter a server URL.', 'err'); return; }
      if (!bridge) { setStatus('Native bridge unavailable.', 'err'); return; }
      setStatus(action === 'test' ? 'Testing…' : 'Connecting…', 'warn');
      bridge.send({ type: 'pinback-setup', action, url });
    }
    opts.testBtn.addEventListener('click', () => send('test'));
    opts.connectBtn.addEventListener('click', () => send('connect'));
    const saved = bridge && bridge.getSavedUrl ? bridge.getSavedUrl() : '';
    const fallback = (bridge && bridge.getDefaultUrl && bridge.getDefaultUrl()) ||
      'http://127.0.0.1:8088';
    urlEl.value = saved || fallback;
    setStatus('Enter the server address and tap Test or Connect.', 'warn');
  }

  global.pinbackHost = {
    normalizeUrl,
    setupBridge,
    cockpitSink,
    canOpenServerSetup,
    openServerSetup,
    initSetupPanel,
  };
})(typeof window !== 'undefined' ? window : global);
