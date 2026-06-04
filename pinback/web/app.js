'use strict';

let shikiHl = null;
let shikiReady = false;
let shikiInitPromise = null;

function ensureShiki() {
  if (shikiInitPromise) return shikiInitPromise;
  shikiInitPromise = (async () => {
    try {
      const mod = await import('/vendor/shiki/shiki.mjs');
      shikiHl = await mod.getPinbackHighlighter('/vendor/shiki/onig.wasm');
      shikiReady = true;
      reflowAllCodeBlocks();
    } catch (e) {
      console.warn('shiki failed to load; code blocks will stay unhighlighted', e);
    }
  })();
  return shikiInitPromise;
}

const PREFERS_DARK = window.matchMedia &&
  window.matchMedia('(prefers-color-scheme: dark)').matches;
const SHIKI_THEME = PREFERS_DARK ? 'vitesse-dark' : 'vitesse-light';

function shikiHighlight(code, lang) {
  if (!shikiReady) return null;
  try {
    return shikiHl.codeToHtml(code, { lang: lang || 'txt', theme: SHIKI_THEME });
  } catch (_) {
    try {
      return shikiHl.codeToHtml(code, { lang: 'txt', theme: SHIKI_THEME });
    } catch (_) { return null; }
  }
}

const chat       = document.getElementById('chat');
const empty      = document.getElementById('empty');
const promptEl   = document.getElementById('prompt');
const sendBtn    = document.getElementById('send');
const stopBtn    = document.getElementById('stop');
const upDot      = document.getElementById('up-dot');
const upState    = document.getElementById('up-state');
const wsButton   = document.getElementById('ws-button');
const wsLabel    = document.getElementById('ws-label');
const wsPath     = document.getElementById('ws-path');
const wsTray     = document.getElementById('ws-tray');
const wsList     = document.getElementById('ws-list');
const wsNewPath  = document.getElementById('ws-new-path');
const wsNewBtn   = document.getElementById('ws-new-btn');
const resetBtn   = document.getElementById('reset-btn');

let activeWorkspaceId = null;
let activeWorkspaceMeta = null;
let workspaces = [];

let activeAssistant = null;
let activeAssistantText = '';
let lastSeq = -1;
let knownGeneration = -1;
let es = null;
let busy = false;

// DSML tokens (U+FF5C FULLWIDTH VERTICAL LINE).
const DSML_OPEN  = '<\uFF5CDSML\uFF5Ctool_calls>';
const DSML_CLOSE = '</\uFF5CDSML\uFF5Ctool_calls>';
const DSML_INVOKE_OPEN_RE  = /<\uFF5CDSML\uFF5Cinvoke\s+name="([^"]*)"\s*>/g;
const DSML_INVOKE_CLOSE    = '</\uFF5CDSML\uFF5Cinvoke>';
const DSML_PARAM_OPEN_RE   = /<\uFF5CDSML\uFF5Cparameter\s+name="([^"]*)"(?:\s+string="(true|false)")?\s*>/g;
const DSML_PARAM_CLOSE     = '</\uFF5CDSML\uFF5Cparameter>';

function el(role, text) {
  empty.style.display = 'none';
  const d = document.createElement('div');
  d.className = 'msg ' + role;
  if (text) d.textContent = text;
  chat.appendChild(d);
  chat.scrollTop = chat.scrollHeight;
  return d;
}

function escHtml(s) {
  const d = document.createElement('div');
  d.textContent = s == null ? '' : String(s);
  return d.innerHTML;
}

/* ---- #3 tool-call cards ---- */
function renderToolCard(raw) {
  const det = document.createElement('details');
  det.className = 'toolcard';
  const sum = document.createElement('summary');
  let name, arg;
  if (/^\$\s?/.test(raw)) { name = 'bash'; arg = raw.replace(/^\$\s*/, ''); }
  else { const m = raw.match(/^(\S+)\s*([\s\S]*)$/); name = m ? m[1] : raw; arg = m ? m[2].trim() : ''; }
  const ns = document.createElement('span'); ns.className = 'tc-name';
  ns.textContent = '🛠️ ' + name;
  const ar = document.createElement('span'); ar.className = 'tc-arg';
  ar.textContent = arg;
  sum.append(ns, ar);
  det.appendChild(sum);
  const body = document.createElement('div'); body.className = 'tc-body';
  const pre = document.createElement('pre'); pre.textContent = raw;
  body.appendChild(pre); det.appendChild(body);
  return det;
}

/* ---- #4 per-turn change panel + per-hunk revert ---- */
function parseDiff(text) {
  const lines = text.split('\n');
  const files = [];
  let cur = null, hunk = null;
  for (const ln of lines) {
    if (ln.startsWith('diff --git ')) {
      const m = ln.match(/^diff --git a\/(.*) b\/(.*)$/);
      cur = { file: m ? m[2] : ln, header: [ln], hunks: [] };
      files.push(cur); hunk = null;
    } else if (!cur) {
      continue;
    } else if (ln.startsWith('@@')) {
      hunk = { lines: [ln] }; cur.hunks.push(hunk);
    } else if (hunk) {
      hunk.lines.push(ln);
    } else {
      cur.header.push(ln);
    }
  }
  return files;
}

function renderTurnDiff(diffText) {
  const wrap = document.createElement('div');
  wrap.className = 'turndiff';
  const files = parseDiff(diffText);
  const head = document.createElement('div');
  head.className = 'td-head';
  head.textContent = 'Changes this turn — ' + files.length +
    ' file' + (files.length === 1 ? '' : 's');
  wrap.appendChild(head);
  for (const f of files) {
    const det = document.createElement('details');
    det.className = 'td-file';
    det.open = files.length <= 3;
    const sum = document.createElement('summary');
    sum.textContent = f.file;
    det.appendChild(sum);
    for (const h of f.hunks) {
      const hd = document.createElement('div');
      hd.className = 'hunk';
      const rev = document.createElement('button');
      rev.className = 'revert';
      rev.textContent = 'Revert hunk';
      const patch = f.header.join('\n') + '\n' + h.lines.join('\n') + '\n';
      rev.addEventListener('click', () => revertHunk(patch, hd, rev));
      hd.appendChild(rev);
      const pre = document.createElement('pre');
      for (const ln of h.lines) {
        const span = document.createElement('span');
        span.className = 'dl ' + (ln[0] === '+' ? 'add' : ln[0] === '-' ? 'del' :
          ln.startsWith('@@') ? 'hdr' : '');
        span.textContent = ln || ' ';
        pre.appendChild(span);
      }
      hd.appendChild(pre);
      det.appendChild(hd);
    }
    wrap.appendChild(det);
  }
  return wrap;
}

async function revertHunk(patch, hunkEl, btn) {
  if (!activeWorkspaceId) return;
  btn.disabled = true;
  try {
    const r = await fetch('/api/w/' + activeWorkspaceId + '/revert', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ patch }),
    });
    if (r.ok) {
      hunkEl.style.opacity = '.45';
      btn.textContent = 'reverted';
    } else {
      const e = await r.json().catch(() => ({}));
      btn.disabled = false;
      btn.textContent = 'retry revert';
      console.warn('revert failed', e);
    }
  } catch (e) {
    btn.disabled = false; btn.textContent = 'retry revert';
    console.warn('revert error', e);
  }
}

function parseAssistantStream(text) {
  const out = [];
  let i = 0;
  while (i < text.length) {
    const dsml = text.indexOf(DSML_OPEN, i);
    const fence = text.indexOf('```', i);
    let next = -1, kind = null;
    if (dsml !== -1 && (fence === -1 || dsml < fence)) { next = dsml; kind = 'dsml'; }
    else if (fence !== -1) { next = fence; kind = 'code'; }
    if (next === -1) {
      if (i < text.length) out.push({ kind: 'text', text: text.slice(i) });
      break;
    }
    if (next > i) out.push({ kind: 'text', text: text.slice(i, next) });
    if (kind === 'code') {
      const langStart = next + 3;
      const nl = text.indexOf('\n', langStart);
      const langEnd = nl === -1 ? text.length : nl;
      const lang = text.slice(langStart, langEnd).trim();
      const codeStart = nl === -1 ? text.length : nl + 1;
      const close = text.indexOf('```', codeStart);
      if (close === -1) {
        out.push({ kind: 'code', lang: lang, text: text.slice(codeStart), open: true });
        break;
      }
      out.push({ kind: 'code', lang: lang, text: text.slice(codeStart, close) });
      i = close + 3;
      if (text[i] === '\n') i++;
    } else {
      const blockStart = next + DSML_OPEN.length;
      const blockClose = text.indexOf(DSML_CLOSE, blockStart);
      const blockEnd = blockClose === -1 ? text.length : blockClose;
      const dsmlBody = text.slice(blockStart, blockEnd);
      out.push({ kind: 'dsml', open: blockClose === -1, invokes: parseDsmlInvokes(dsmlBody) });
      if (blockClose === -1) break;
      i = blockClose + DSML_CLOSE.length;
    }
  }
  return out;
}

function parseDsmlInvokes(body) {
  const invokes = [];
  DSML_INVOKE_OPEN_RE.lastIndex = 0;
  let m;
  while ((m = DSML_INVOKE_OPEN_RE.exec(body)) !== null) {
    const name = m[1];
    const after = m.index + m[0].length;
    const closeIdx = body.indexOf(DSML_INVOKE_CLOSE, after);
    const innerEnd = closeIdx === -1 ? body.length : closeIdx;
    invokes.push({
      name: name,
      open: closeIdx === -1,
      params: parseDsmlParams(body.slice(after, innerEnd)),
    });
    if (closeIdx === -1) break;
    DSML_INVOKE_OPEN_RE.lastIndex = closeIdx + DSML_INVOKE_CLOSE.length;
  }
  return invokes;
}

function parseDsmlParams(body) {
  const params = [];
  DSML_PARAM_OPEN_RE.lastIndex = 0;
  let m;
  while ((m = DSML_PARAM_OPEN_RE.exec(body)) !== null) {
    const name = m[1];
    const isString = m[2];
    const after = m.index + m[0].length;
    const closeIdx = body.indexOf(DSML_PARAM_CLOSE, after);
    const valueEnd = closeIdx === -1 ? body.length : closeIdx;
    params.push({
      name: name,
      string: isString,
      value: body.slice(after, valueEnd),
      open: closeIdx === -1,
    });
    if (closeIdx === -1) break;
    DSML_PARAM_OPEN_RE.lastIndex = closeIdx + DSML_PARAM_CLOSE.length;
  }
  return params;
}

function appendDsmlBlock(bubble, dsml) {
  const wrap = document.createElement('div');
  wrap.className = 'dsml' + (dsml.open ? ' dsml-open' : '');
  for (const inv of dsml.invokes) {
    const det = document.createElement('details');
    det.className = 'dsml-invoke' + (inv.open ? ' dsml-streaming' : '');
    det.open = false;
    const sum = document.createElement('summary');
    sum.className = 'dsml-summary';
    const tag = document.createElement('span');
    tag.className = 'dsml-tag';
    tag.textContent = 'tool';
    const nameEl = document.createElement('span');
    nameEl.className = 'dsml-name';
    nameEl.textContent = inv.name || '(unnamed)';
    const status = document.createElement('span');
    status.className = 'dsml-status';
    status.textContent = inv.open ? '\u2026' : '';
    sum.appendChild(tag);
    sum.appendChild(nameEl);
    sum.appendChild(status);
    det.appendChild(sum);

    const body = document.createElement('div');
    body.className = 'dsml-body';
    if (inv.params.length === 0) {
      const ph = document.createElement('div');
      ph.className = 'dsml-empty';
      ph.textContent = inv.open ? 'streaming\u2026' : '(no parameters)';
      body.appendChild(ph);
    } else {
      const tbl = document.createElement('table');
      tbl.className = 'dsml-params';
      for (const p of inv.params) {
        const tr = document.createElement('tr');
        const k = document.createElement('td');
        k.className = 'dsml-pname';
        k.textContent = p.name;
        const v = document.createElement('td');
        v.className = 'dsml-pvalue';
        const multiline = p.value.indexOf('\n') !== -1 || p.value.length > 80;
        if (multiline) {
          const pre = document.createElement('pre');
          pre.className = 'dsml-pre';
          const codeEl = document.createElement('code');
          codeEl.textContent = p.value;
          pre.appendChild(codeEl);
          v.appendChild(pre);
        } else {
          const codeEl = document.createElement('code');
          codeEl.className = 'dsml-inline';
          codeEl.textContent = p.value;
          v.appendChild(codeEl);
        }
        if (p.open) {
          const dot = document.createElement('span');
          dot.className = 'dsml-streaming-dot';
          dot.textContent = '\u2026';
          v.appendChild(dot);
        }
        tr.appendChild(k);
        tr.appendChild(v);
        tbl.appendChild(tr);
      }
      body.appendChild(tbl);
    }
    det.appendChild(body);
    wrap.appendChild(det);
  }
  if (dsml.invokes.length === 0) {
    const placeholder = document.createElement('div');
    placeholder.className = 'dsml-empty';
    placeholder.textContent = dsml.open ? 'tool call streaming\u2026' : '(empty tool_calls block)';
    wrap.appendChild(placeholder);
  }
  bubble.appendChild(wrap);
}

function appendCodeBlock(bubble, code, lang) {
  const html = shikiHighlight(code, lang);
  if (html) {
    const tmp = document.createElement('div');
    tmp.innerHTML = html;
    while (tmp.firstChild) bubble.appendChild(tmp.firstChild);
  } else {
    const pre = document.createElement('pre');
    pre.className = 'code';
    if (lang) pre.dataset.lang = lang;
    const codeEl = document.createElement('code');
    codeEl.textContent = code;
    pre.appendChild(codeEl);
    bubble.appendChild(pre);
  }
}

function renderAssistantBubble(bubble, fullText, withCaret) {
  while (bubble.firstChild) bubble.removeChild(bubble.firstChild);
  for (const seg of parseAssistantStream(fullText)) {
    if (seg.kind === 'text') {
      bubble.appendChild(document.createTextNode(seg.text));
    } else if (seg.kind === 'code') {
      appendCodeBlock(bubble, seg.text, seg.lang);
    } else if (seg.kind === 'dsml') {
      appendDsmlBlock(bubble, seg);
    }
  }
  if (withCaret) {
    const caret = document.createElement('span');
    caret.className = 'caret';
    bubble.appendChild(caret);
  }
}

function reflowAllCodeBlocks() {
  const bubbles = chat.querySelectorAll('.msg.assistant');
  for (const b of bubbles) {
    const txt = b.dataset.text || '';
    if (!txt) continue;
    renderAssistantBubble(b, txt, b === activeAssistant);
  }
}

function setRuntimeBanner(text, level) {
  upState.textContent = text;
  upDot.className = 'dot' + (level ? ' ' + level : '');
}

function showEmptyState() {
  chat.innerHTML = '';
  const e = document.createElement('div');
  e.className = 'empty';
  e.id = 'empty';
  if (activeWorkspaceMeta) {
    const t = document.createElement('div');
    t.className = 'title';
    t.textContent = activeWorkspaceMeta.label || 'workspace';
    e.appendChild(t);
    const cwd = document.createElement('div');
    cwd.className = 'cwd';
    cwd.textContent = activeWorkspaceMeta.path || '';
    e.appendChild(cwd);
    const h = document.createElement('div');
    h.className = 'hint';
    h.textContent = 'Type a message to start.';
    e.appendChild(h);
  } else {
    const t = document.createElement('div');
    t.className = 'title';
    t.textContent = 'pinback';
    e.appendChild(t);
    e.appendChild(document.createTextNode('Pick or add a workspace to begin.'));
    const h = document.createElement('div');
    h.className = 'hint';
    h.textContent = 'A workspace is just a directory the agent runs in.';
    e.appendChild(h);
  }
  chat.appendChild(e);
}

function applyEvent(env) {
  if (!env || typeof env !== 'object') return;
  if (typeof env.generation === 'number') knownGeneration = env.generation;
  if (typeof env.seq === 'number') lastSeq = env.seq;

  switch (env.kind) {
    case 'cursor_reset':
      return;
    case 'user': {
      el('user', env.payload && env.payload.text || '');
      activeAssistant = null;
      activeAssistantText = '';
      busy = true;
      sendBtn.hidden = true;
      stopBtn.hidden = false;
      return;
    }
    case 'answer':
    case 'token': {
      if (!activeAssistant) {
        activeAssistant = el('assistant', '');
        activeAssistantText = '';
      }
      const t = env.payload && env.payload.text || '';
      activeAssistantText += t;
      activeAssistant.dataset.text = activeAssistantText;
      renderAssistantBubble(activeAssistant, activeAssistantText, true);
      chat.scrollTop = chat.scrollHeight;
      return;
    }
    case 'tool_call': {
      // The real ds4-agent renders tool actions as a wrench-prefixed line
      // (no raw DSML), which the server delivers as a tool_call event with
      // the rendered text in payload.raw. Show it as a tool-activity chip.
      const p = env.payload || {};
      const raw = (p.raw || '').trim();
      if (!raw) return;
      if (activeAssistant) {
        renderAssistantBubble(activeAssistant, activeAssistantText, false);
        activeAssistant = null;
        activeAssistantText = '';
      }
      empty.style.display = 'none';
      chat.appendChild(renderToolCard(raw));
      chat.scrollTop = chat.scrollHeight;
      return;
    }
    case 'turn_diff': {
      const p = env.payload || {};
      if (p.diff && p.diff.trim()) chat.appendChild(renderTurnDiff(p.diff));
      chat.scrollTop = chat.scrollHeight;
      return;
    }
    case 'reverted': {
      // A hunk was reverted (here or by another client); leave the
      // optimistic UI in place.
      return;
    }
    case 'tool_result': {
      const p = env.payload || {};
      const txt = p.text || p.summary || JSON.stringify(p);
      const m = el('tool', '');
      const head = document.createElement('div');
      head.style.fontWeight = '600';
      head.style.color = 'var(--fg-mute)';
      head.style.marginBottom = '4px';
      head.textContent = 'tool_result' + (p.tool ? ' \u00b7 ' + p.tool : '');
      m.appendChild(head);
      const pre = document.createElement('pre');
      pre.className = 'code';
      pre.style.margin = '0';
      const codeEl = document.createElement('code');
      codeEl.textContent = txt;
      pre.appendChild(codeEl);
      m.appendChild(pre);
      activeAssistant = null;
      activeAssistantText = '';
      return;
    }
    case 'aborted': {
      if (activeAssistant) {
        renderAssistantBubble(activeAssistant, activeAssistantText, false);
        const meta = document.createElement('span');
        meta.className = 'meta';
        meta.textContent = 'aborted';
        activeAssistant.appendChild(meta);
      }
      return;
    }
    case 'answer_end': {
      const finished = activeAssistant;
      const finishedText = activeAssistantText;
      const wasAborted = env.payload && env.payload.aborted;
      activeAssistant = null;
      activeAssistantText = '';
      busy = false;
      sendBtn.hidden = false;
      stopBtn.hidden = true;
      if (finished) {
        finished.dataset.text = finishedText;
        renderAssistantBubble(finished, finishedText, false);
        if (wasAborted) {
          const meta = document.createElement('span');
          meta.className = 'meta';
          meta.textContent = 'aborted';
          finished.appendChild(meta);
        }
      }
      promptEl.focus();
      return;
    }
    case 'agent.save_sha':
    case 'save_sha': {
      // Informational only; surfaced via /api/runtime banners.
      return;
    }
    case 'runtime': {
      const p = env.payload || {};
      const st = p.state || p.upstream || 'unknown';
      let level = '';
      if (st === 'ready' || st === 'busy') level = 'ok';
      else if (st === 'spawning' || st === 'draining') level = 'warn';
      else level = 'err';
      setRuntimeBanner(st, level);
      return;
    }
    case 'error':
    case 'agent.error': {
      const p = env.payload || {};
      el('error', p.message || 'error');
      busy = false;
      sendBtn.hidden = false;
      stopBtn.hidden = true;
      return;
    }
  }
}

function renderSnapshotEvents(events) {
  chat.innerHTML = '';
  showEmptyState();
  activeAssistant = null;
  activeAssistantText = '';
  if (!Array.isArray(events) || events.length === 0) return;
  for (const env of events) applyEvent(env);
}

function disconnect() {
  if (es) { try { es.close(); } catch (_) {} es = null; }
}

function connectActive() {
  disconnect();
  if (!activeWorkspaceId) {
    setRuntimeBanner('no workspace', '');
    return;
  }
  const url = new URL('/api/w/' + activeWorkspaceId + '/events', location.href);
  url.searchParams.set('after', String(lastSeq >= 0 ? lastSeq : 0));
  if (knownGeneration >= 0) url.searchParams.set('generation', String(knownGeneration));
  es = new EventSource(url.toString());
  es.onopen = () => setRuntimeBanner('connected', 'ok');
  es.onerror = () => setRuntimeBanner('reconnecting\u2026', 'warn');

  es.addEventListener('event', (ev) => {
    try { applyEvent(JSON.parse(ev.data)); }
    catch (e) { console.warn('bad event frame', e, ev.data); }
  });

  es.addEventListener('snapshot', (ev) => {
    try {
      const snap = JSON.parse(ev.data);
      if (typeof snap.generation === 'number') knownGeneration = snap.generation;
      if (typeof snap.newest_seq === 'number') lastSeq = snap.newest_seq;
      renderSnapshotEvents(snap.events);
    } catch (e) { console.warn('bad snapshot', e, ev.data); }
  });

  es.onmessage = (ev) => {
    try { applyEvent(JSON.parse(ev.data)); }
    catch (e) { console.warn('bad default frame', e, ev.data); }
  };
}

async function refreshWorkspaces() {
  try {
    const r = await fetch('/api/w');
    if (!r.ok) return;
    const j = await r.json();
    workspaces = Array.isArray(j.workspaces) ? j.workspaces : [];
    const newActive = j.active_id || null;
    const changed = (newActive !== activeWorkspaceId);
    activeWorkspaceId = newActive;
    activeWorkspaceMeta = workspaces.find((w) => w.id === activeWorkspaceId) || null;
    renderHeader();
    renderWorkspaceList();
    if (changed) {
      lastSeq = -1;
      knownGeneration = -1;
      activeAssistant = null;
      activeAssistantText = '';
      busy = false;
      sendBtn.hidden = false;
      stopBtn.hidden = true;
      showEmptyState();
      connectActive();
    }
  } catch (e) {
    console.warn('refreshWorkspaces failed', e);
  }
}

function renderHeader() {
  if (activeWorkspaceMeta) {
    wsLabel.textContent = activeWorkspaceMeta.label || activeWorkspaceMeta.id;
    wsPath.textContent = activeWorkspaceMeta.path || '';
    resetBtn.disabled = false;
  } else {
    wsLabel.textContent = workspaces.length ? 'Pick a workspace' : 'No workspace';
    wsPath.textContent = '';
    resetBtn.disabled = true;
  }
}

function renderWorkspaceList() {
  wsList.innerHTML = '';
  if (workspaces.length === 0) {
    const e = document.createElement('div');
    e.className = 'empty';
    e.textContent = 'no workspaces yet';
    wsList.appendChild(e);
    return;
  }
  for (const w of workspaces) {
    const row = document.createElement('div');
    row.className = 'row' + (w.id === activeWorkspaceId ? ' active' : '');
    const m = document.createElement('span');
    m.className = 'marker';
    row.appendChild(m);
    const col = document.createElement('div');
    col.className = 'col';
    const name = document.createElement('div');
    name.className = 'name';
    name.textContent = w.label || w.id;
    const path = document.createElement('div');
    path.className = 'path';
    path.textContent = w.path || '';
    col.appendChild(name);
    col.appendChild(path);
    row.appendChild(col);

    const actions = document.createElement('div');
    actions.className = 'row-actions';
    if (w.id !== activeWorkspaceId) {
      const a = document.createElement('button');
      a.textContent = 'Activate';
      a.addEventListener('click', (ev) => {
        ev.stopPropagation();
        activateWorkspace(w.id);
      });
      actions.appendChild(a);
    }
    const d = document.createElement('button');
    d.className = 'danger';
    d.textContent = 'Delete';
    d.disabled = (w.id === activeWorkspaceId);
    if (d.disabled) d.title = 'deactivate first';
    d.addEventListener('click', (ev) => {
      ev.stopPropagation();
      if (!confirm('Delete workspace "' + (w.label || w.id) + '"? Events log will be archived.')) return;
      deleteWorkspace(w.id);
    });
    actions.appendChild(d);
    row.appendChild(actions);

    row.addEventListener('click', () => {
      if (w.id !== activeWorkspaceId) activateWorkspace(w.id);
    });
    wsList.appendChild(row);
  }
}

async function activateWorkspace(id) {
  setRuntimeBanner('switching\u2026', 'warn');
  try {
    const r = await fetch('/api/w/' + id + '/activate', { method: 'POST' });
    if (!r.ok) {
      const j = await r.json().catch(() => ({ message: r.statusText }));
      el('error', 'activate failed: ' + (j.message || r.statusText));
      return;
    }
    closeTray();
    await refreshWorkspaces();
  } catch (e) {
    el('error', 'activate failed: ' + e);
  }
}

async function createWorkspace(path) {
  if (!path || !path.trim()) return;
  try {
    const r = await fetch('/api/w', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ path: path.trim() }),
    });
    if (!r.ok) {
      const j = await r.json().catch(() => ({ message: r.statusText }));
      el('error', 'add workspace failed: ' + (j.message || r.statusText));
      return null;
    }
    const meta = await r.json();
    wsNewPath.value = '';
    await refreshWorkspaces();
    return meta;
  } catch (e) {
    el('error', 'add workspace failed: ' + e);
    return null;
  }
}

async function deleteWorkspace(id) {
  try {
    const r = await fetch('/api/w/' + id, { method: 'DELETE' });
    if (!r.ok) {
      const j = await r.json().catch(() => ({ message: r.statusText }));
      el('error', 'delete failed: ' + (j.message || r.statusText));
      return;
    }
    await refreshWorkspaces();
  } catch (e) {
    el('error', 'delete failed: ' + e);
  }
}

async function resetActive() {
  if (!activeWorkspaceId) return;
  if (!confirm('Reset this conversation? The agent will respawn fresh in the same directory.')) return;
  try {
    const r = await fetch('/api/w/' + activeWorkspaceId + '/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ op: 'reset' }),
    });
    if (!r.ok) {
      const j = await r.json().catch(() => ({ message: r.statusText }));
      el('error', 'reset failed: ' + (j.message || r.statusText));
      return;
    }
    lastSeq = -1;
    knownGeneration = -1;
    showEmptyState();
    connectActive();
  } catch (e) {
    el('error', 'reset failed: ' + e);
  }
}

async function abortTurn() {
  if (!activeWorkspaceId) return;
  try {
    await fetch('/api/w/' + activeWorkspaceId + '/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ op: 'abort' }),
    });
  } catch (e) {
    console.warn('abort failed', e);
  }
}

async function send() {
  const text = promptEl.value;
  const trimmed = text.trim();
  if (!trimmed || busy) return;
  if (!activeWorkspaceId) {
    el('error', 'no workspace active');
    return;
  }
  promptEl.value = '';
  promptEl.style.height = 'auto';
  busy = true;
  sendBtn.disabled = true;
  try {
    const r = await fetch('/api/w/' + activeWorkspaceId + '/input', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ text: trimmed }),
    });
    if (!r.ok) {
      const j = await r.json().catch(() => ({ message: r.statusText }));
      el('error', j.message || ('http ' + r.status));
      busy = false;
    }
  } catch (e) {
    el('error', String(e));
    busy = false;
  } finally {
    sendBtn.disabled = false;
  }
}

function openTray() { wsTray.classList.add('open'); wsNewPath.focus(); }
function closeTray() { wsTray.classList.remove('open'); }

promptEl.addEventListener('keydown', (e) => {
  if (e.key === 'Enter' && !e.shiftKey) {
    e.preventDefault();
    send();
  }
});
promptEl.addEventListener('input', () => {
  promptEl.style.height = 'auto';
  promptEl.style.height = Math.min(promptEl.scrollHeight, 240) + 'px';
});

sendBtn.addEventListener('click', send);
stopBtn.addEventListener('click', abortTurn);
resetBtn.addEventListener('click', resetActive);

wsButton.addEventListener('click', (e) => {
  e.stopPropagation();
  if (wsTray.classList.contains('open')) closeTray();
  else { refreshWorkspaces(); openTray(); }
});
document.addEventListener('click', (e) => {
  if (!wsTray.contains(e.target) && e.target !== wsButton) closeTray();
});
wsTray.addEventListener('click', (e) => e.stopPropagation());
wsNewBtn.addEventListener('click', () => createWorkspace(wsNewPath.value));
wsNewPath.addEventListener('keydown', (e) => {
  if (e.key === 'Enter') { e.preventDefault(); createWorkspace(wsNewPath.value); }
});

async function pollRuntime() {
  try {
    const r = await fetch('/api/runtime');
    if (!r.ok) return;
    const j = await r.json();
    if (j.agent && j.agent.state) {
      const st = j.agent.state;
      let level = '';
      if (st === 'ready' || st === 'busy') level = 'ok';
      else if (st === 'spawning' || st === 'draining') level = 'warn';
      else if (st === 'idle') level = '';
      else level = 'err';
      setRuntimeBanner(st, level);
    }
    // If active changed externally (CLI), reflect it.
    if (j.active_id !== undefined && j.active_id !== activeWorkspaceId) {
      refreshWorkspaces();
    }
  } catch (_) {}
}

/* ---- #5 cross-workspace dashboard ---- */
const dashBtn     = document.getElementById('dash-btn');
const dashOverlay = document.getElementById('dash-overlay');
const dashClose   = document.getElementById('dash-close');
const dashRows    = document.getElementById('dash-rows');
let dashData = [];
let dashSort = { key: 'last_active_ms', dir: -1 };

function fmtAge(ms) {
  if (!ms) return '—';
  const s = (Date.now() - ms) / 1000;
  if (s < 60) return Math.max(0, Math.round(s)) + 's ago';
  if (s < 3600) return Math.round(s / 60) + 'm ago';
  if (s < 86400) return Math.round(s / 3600) + 'h ago';
  return Math.round(s / 86400) + 'd ago';
}

function renderDash() {
  const k = dashSort.key, dir = dashSort.dir;
  const rows = [...dashData].sort((a, b) => {
    let av = k === 'preview' ? (a.last_user || '') : a[k];
    let bv = k === 'preview' ? (b.last_user || '') : b[k];
    if (typeof av === 'string') return dir * av.localeCompare(bv);
    return dir * ((av || 0) - (bv || 0));
  });
  dashRows.innerHTML = '';
  if (!rows.length) {
    dashRows.innerHTML = '<tr><td colspan="4" style="color:var(--fg-mute)">No workspaces yet.</td></tr>';
    return;
  }
  for (const w of rows) {
    const tr = document.createElement('tr');
    tr.className = 'row' + (w.active ? ' active' : '');
    const prev = (w.last_user ? 'You: ' + w.last_user : '') +
                 (w.last_answer ? '  ·  ' + w.last_answer : '');
    tr.innerHTML =
      '<td><div>' + escHtml(w.label || '(workspace)') + '</div>' +
        '<div class="dash-prev" style="font-size:11px">' + escHtml(w.path) + '</div></td>' +
      '<td><span class="dash-state ' + escHtml(w.state) + '">' + escHtml(w.state) + '</span></td>' +
      '<td>' + escHtml(fmtAge(w.last_active_ms)) + '</td>' +
      '<td><div class="dash-prev">' + escHtml(prev || '—') + '</div></td>';
    tr.addEventListener('click', () => { dashOverlay.hidden = true; activateWorkspace(w.id); });
    dashRows.appendChild(tr);
  }
}

async function openDashboard() {
  dashOverlay.hidden = false;
  dashRows.innerHTML = '<tr><td colspan="4" style="color:var(--fg-mute)">loading…</td></tr>';
  try {
    const r = await fetch('/api/dashboard');
    const j = await r.json();
    dashData = j.workspaces || [];
    renderDash();
  } catch (e) {
    dashRows.innerHTML = '<tr><td colspan="4">failed to load dashboard</td></tr>';
  }
}

if (dashBtn) {
  dashBtn.addEventListener('click', openDashboard);
  dashClose.addEventListener('click', () => { dashOverlay.hidden = true; });
  dashOverlay.addEventListener('click', (e) => {
    if (e.target === dashOverlay) dashOverlay.hidden = true;
  });
  document.querySelectorAll('#dash-table th[data-sort]').forEach((th) => {
    th.addEventListener('click', () => {
      const k = th.dataset.sort;
      if (dashSort.key === k) dashSort.dir *= -1;
      else { dashSort.key = k; dashSort.dir = (k === 'last_active_ms') ? -1 : 1; }
      renderDash();
    });
  });
}

ensureShiki();
showEmptyState();
refreshWorkspaces().then(() => connectActive());
pollRuntime();
setInterval(pollRuntime, 5000);
