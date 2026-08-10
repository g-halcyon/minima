// ui_assets.h — portable UI: every screen Minima renders is authored here as a
// self-contained HTML/CSS/JS string. This is the largest part of the "core" from
// PORTING.md and is 100% platform-independent — a webview on any OS renders it as-is.
// Extracted verbatim from the Windows shell (src/main.cpp) as the first step of the
// core/shell split; no behavior change.
#pragma once

// ---------------------------------------------------------------------------
// Embedded chrome UI (tabs + address bar). Talks to C++ via
// window.chrome.webview.postMessage("cmd\x1Farg") and receives state as JSON.
// ---------------------------------------------------------------------------
static const wchar_t* kChromeHtml = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><style>
  :root { color-scheme: light dark;
    --bg:#eceef2; --fg:#1c1c21; --muted:#5f6368; --tab-in:rgba(0,0,0,.045); --tab-fg:#555;
    --tab-active:#fff; --hover:rgba(0,0,0,.07); --field:rgba(0,0,0,.055); --field-focus:#fff;
    --accent:#3b82f6; --sel:rgba(59,130,246,.12); --ok:#188038;
  }
  @media (prefers-color-scheme: dark) { :root {
    --bg:#1f2023; --fg:#e8eaed; --muted:#9aa0a6; --tab-in:rgba(255,255,255,.06); --tab-fg:#bdc1c6;
    --tab-active:#37393e; --hover:rgba(255,255,255,.09); --field:rgba(255,255,255,.08); --field-focus:#2a2b2f;
    --sel:rgba(59,130,246,.25); --ok:#6dd58c;
  } }
  * { margin:0; padding:0; box-sizing:border-box; user-select:none;
      font-family:"Segoe UI Variable Text","Segoe UI",system-ui; }
  html, body { overflow:hidden; }
  ::-webkit-scrollbar { display:none; }
  body { font-size:13px; background:var(--bg); color:var(--fg); }
  svg { width:16px; height:16px; stroke:currentColor; stroke-width:1.8; fill:none;
        stroke-linecap:round; stroke-linejoin:round; display:block; }

  #tabs { display:flex; align-items:flex-end; height:36px; padding:5px 8px 0; }
  .tab { display:flex; align-items:center; gap:7px; min-width:0; max-width:220px; flex:1 1 0;
         height:31px; padding:0 5px 0 10px; margin-right:3px; border-radius:9px 9px 0 0; cursor:default;
         background:var(--tab-in); color:var(--tab-fg); font-size:12px; white-space:nowrap;
         transition:background .12s; }
  .tab:hover:not(.active) { background:var(--hover); }
  .tab.active { background:var(--tab-active); color:var(--fg); box-shadow:0 1px 3px rgba(0,0,0,.15); }
  .tab img.fav { width:15px; height:15px; border-radius:3px; flex:none; }
  .tab .favdot { flex:none; display:flex; opacity:.55; }
  .tab .favdot svg { width:14px; height:14px; }
  .tab span.t { overflow:hidden; text-overflow:ellipsis; min-width:0; flex:1; }
  .tab button { border:none; background:none; color:inherit; cursor:pointer; flex:none;
                width:20px; height:20px; border-radius:5px; display:flex; align-items:center;
                justify-content:center; padding:0; }
  .tab button svg { width:11px; height:11px; stroke-width:2.4; }
  .tab .x { opacity:.5; }
  .tab .x:hover { opacity:1; background:var(--hover); }
  .tab .snd { opacity:.7; }
  .tab .snd:hover { opacity:1; background:var(--hover); }
  .tab .snd svg { width:13px; height:13px; stroke-width:1.8; }
  .spin { width:13px; height:13px; flex:none; border:2px solid var(--accent); border-top-color:transparent;
          border-radius:50%; animation:r .8s linear infinite; }
  @keyframes r { to { transform:rotate(360deg); } }
  #newtab { border:none; background:none; color:var(--muted); width:27px; height:27px;
            border-radius:7px; cursor:pointer; flex:none; margin:0 0 2px 1px; display:flex;
            align-items:center; justify-content:center; }
  #newtab:hover { background:var(--hover); }
  #newtab svg { width:14px; height:14px; }

  #bar { display:flex; align-items:center; height:52px; padding:0 10px 8px; gap:3px; }
  #bar > button { border:none; background:none; width:33px; height:33px; border-radius:8px;
                  color:var(--muted); cursor:pointer; flex:none; display:flex; align-items:center;
                  justify-content:center; }
  #bar > button:hover:not(:disabled) { background:var(--hover); color:var(--fg); }
  #bar > button:disabled { opacity:.35; cursor:default; }
  #ai { font-size:16px; color:var(--accent); }
  #ai:hover { background:var(--hover); }

  #addrwrap { flex:1; display:flex; align-items:center; height:34px; margin:0 6px; padding:0 4px 0 11px;
              border-radius:10px; background:var(--field); transition:background .12s, box-shadow .12s;
              min-width:0; }
  #addrwrap.focus { background:var(--field-focus); box-shadow:0 0 0 2px var(--accent); }
  #lock { display:flex; color:var(--muted); margin-right:7px; flex:none; }
  #lock svg { width:13px; height:13px; }
  #lock.secure { color:var(--ok); }
  #addr { flex:1; min-width:0; height:100%; border:none; background:none; font-size:13px; outline:none;
          color:inherit; user-select:text; }
  #addrwrap button { border:none; background:none; width:28px; height:28px; border-radius:7px;
                     color:var(--muted); cursor:pointer; flex:none; display:flex; align-items:center;
                     justify-content:center; position:relative; }
  #addrwrap button:hover { background:var(--hover); color:var(--fg); }
  #addrwrap button svg { width:15px; height:15px; }
  #shield.on { color:var(--accent); }
  #shield.off { opacity:.4; }
  #shieldCount { position:absolute; top:-3px; right:-3px; background:var(--accent); color:#fff;
                 font-size:8.5px; font-weight:600; border-radius:8px; padding:1px 4px; line-height:1.2; }
  #star.active { color:#f2a71b; }
  #star.active svg { fill:#f2a71b; stroke:#f2a71b; }

  #suggest { display:none; padding:2px 50px 8px; }
  .sugrow { display:flex; align-items:center; gap:10px; height:36px; padding:0 12px; border-radius:8px;
            cursor:default; font-size:13px; min-width:0; }
  .sugrow.sel, .sugrow:hover { background:var(--sel); }
  .sugrow .ic { color:var(--muted); flex:none; display:flex; }
  .sugrow .ic svg { width:14px; height:14px; }
  .sugrow .ic.ai { color:var(--accent); font-size:14px; }
  .sugrow .txt { overflow:hidden; text-overflow:ellipsis; white-space:nowrap; flex:none; max-width:55%; }
  .sugrow .txt b { font-weight:600; }
  .sugrow .url { color:var(--muted); font-size:12px; overflow:hidden; text-overflow:ellipsis;
                 white-space:nowrap; min-width:0; }
  .aibox { padding:2px 12px 10px; }
  .aihead { display:flex; align-items:center; gap:8px; font-size:12px; color:var(--muted); margin-bottom:8px; }
  .aihead .ic { color:var(--accent); font-size:13px; }
  .aihead b { color:inherit; font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .aibody { font-size:13px; line-height:1.5; max-height:118px; overflow-y:auto; white-space:pre-wrap;
            margin-bottom:8px; user-select:text; }
  .aimore { border:none; background:none; color:var(--accent); font-size:12px; font-weight:600;
            cursor:pointer; padding:4px 0; }
  .aimore:hover { text-decoration:underline; }
  .dots { display:inline-flex; gap:5px; padding:4px 0; }
  .dots span { width:5px; height:5px; border-radius:50%; background:#999; animation:bounce 1.2s infinite ease-in-out; }
  .dots span:nth-child(2) { animation-delay:.15s; }
  .dots span:nth-child(3) { animation-delay:.3s; }
  @keyframes bounce { 0%,80%,100% { transform:scale(.6); opacity:.4; } 40% { transform:scale(1); opacity:1; } }
)HTML" LR"HTML(
  #palette { display:none; padding:0 10px 10px; }
  #palette .pwrap { background:var(--field-focus); border:1px solid var(--hover); border-radius:12px;
                    box-shadow:0 10px 34px rgba(0,0,0,.22); overflow:hidden; }
  #pcmd { width:100%; border:none; background:none; outline:none; padding:14px 16px; font-size:15px;
          color:var(--fg); border-bottom:1px solid var(--hover); user-select:text; }
  #plist { max-height:270px; overflow-y:auto; padding:6px; }
  .prow { display:flex; align-items:center; gap:11px; padding:9px 12px; border-radius:8px; cursor:default;
          font-size:13px; }
  .prow.sel { background:var(--sel); }
  .prow .pi { color:var(--muted); display:flex; width:16px; justify-content:center; flex:none; }
  .prow .pi.ai { color:var(--accent); font-size:14px; }
  .prow .pl { flex:1; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .prow .ph { color:var(--muted); font-size:11.5px; flex:none; }

  #findbar { display:none; padding:0 10px 8px; }
  #findbar .fwrap { display:flex; align-items:center; gap:4px; background:var(--field-focus);
                    border:1px solid var(--hover); border-radius:10px; padding:4px 6px 4px 12px;
                    box-shadow:0 6px 22px rgba(0,0,0,.16); max-width:420px; }
  #fq { flex:1; min-width:0; border:none; background:none; outline:none; font-size:13px;
        color:var(--fg); height:26px; user-select:text; }
  #fcount { color:var(--muted); font-size:11.5px; flex:none; padding:0 6px; white-space:nowrap; }
  #findbar button { border:none; background:none; color:var(--muted); width:26px; height:26px;
                    border-radius:6px; cursor:pointer; flex:none; display:flex; align-items:center;
                    justify-content:center; }
  #findbar button:hover { background:var(--hover); color:var(--fg); }
  #findbar button svg { width:13px; height:13px; }
</style></head><body>
<div id="tabs"></div>
<div id="bar">
  <button id="back" title="Back (Alt+Left)"></button>
  <button id="fwd" title="Forward (Alt+Right)"></button>
  <button id="reload" title="Reload (Ctrl+R)"></button>
  <div id="addrwrap">
    <span id="lock"></span>
    <input id="addr" placeholder="Search or enter address" spellcheck="false" autocomplete="off">
    <button id="shield" title="Ad &amp; tracker blocking"><span class="icw"></span></button>
    <button id="star" title="Bookmark (Ctrl+D)"></button>
  </div>
  <button id="ai" title="Ask this page with AI (Ctrl+K)">&#10022;</button>
  <button id="dl" title="Downloads (Ctrl+J)"></button>
  <button id="hist" title="History (Ctrl+H)"></button>
  <button id="settings" title="Settings (Ctrl+,)"></button>
</div>
<div id="suggest"></div>
<div id="findbar"><div class="fwrap">
  <input id="fq" placeholder="Find in page" spellcheck="false" autocomplete="off">
  <span id="fcount"></span>
  <button id="fprev" title="Previous (Shift+Enter)"><svg viewBox="0 0 24 24"><path d="M18 15l-6-6-6 6"/></svg></button>
  <button id="fnext" title="Next (Enter)"><svg viewBox="0 0 24 24"><path d="M6 9l6 6 6-6"/></svg></button>
  <button id="fclose" title="Close (Esc)"><svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg></button>
</div></div>
<div id="palette"><div class="pwrap">
  <input id="pcmd" placeholder="Type a command or ask AI…" spellcheck="false" autocomplete="off">
  <div id="plist"></div>
</div></div>
<script>)HTML" LR"HTML(
  const send = (cmd, arg='') => window.chrome.webview.postMessage(cmd + '\x1F' + arg);
  const I = {
    back:'<svg viewBox="0 0 24 24"><path d="M19 12H5"/><path d="M12 19l-7-7 7-7"/></svg>',
    fwd:'<svg viewBox="0 0 24 24"><path d="M5 12h14"/><path d="M12 5l7 7-7 7"/></svg>',
    reload:'<svg viewBox="0 0 24 24"><path d="M21 12a9 9 0 1 1-2.6-6.4"/><path d="M21 3v6h-6"/></svg>',
    star:'<svg viewBox="0 0 24 24"><path d="M12 3l2.7 5.8 6.3.8-4.6 4.4 1.2 6.2-5.6-3.1-5.6 3.1 1.2-6.2L3 9.6l6.3-.8z"/></svg>',
    shield:'<svg viewBox="0 0 24 24"><path d="M12 3l7 3v5.5c0 4.5-3 8-7 9.5-4-1.5-7-5-7-9.5V6z"/></svg>',
    hist:'<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 3"/></svg>',
    settings:'<svg viewBox="0 0 24 24"><path d="M4 7h16"/><circle cx="9" cy="7" r="2.6"/><path d="M4 17h16"/><circle cx="15" cy="17" r="2.6"/></svg>',
    lock:'<svg viewBox="0 0 24 24"><rect x="5" y="11" width="14" height="9" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg>',
    globe:'<svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M3 12h18M12 3a14 14 0 0 1 0 18M12 3a14 14 0 0 0 0 18"/></svg>',
    search:'<svg viewBox="0 0 24 24"><circle cx="11" cy="11" r="7"/><path d="M21 21l-5-5"/></svg>',
    snd:'<svg viewBox="0 0 24 24"><path d="M11 5L6 9H3v6h3l5 4z"/><path d="M15.5 8.5a5 5 0 0 1 0 7"/></svg>',
    mute:'<svg viewBox="0 0 24 24"><path d="M11 5L6 9H3v6h3l5 4z"/><path d="M16 9l5 6M21 9l-5 6"/></svg>',
    close:'<svg viewBox="0 0 24 24"><path d="M6 6l12 12M18 6L6 18"/></svg>',
    plus:'<svg viewBox="0 0 24 24"><path d="M12 5v14M5 12h14"/></svg>',
    dl:'<svg viewBox="0 0 24 24"><path d="M12 4v11"/><path d="M6 11l6 6 6-6"/><path d="M5 20h14"/></svg>',
  };
  for (const id of ['back','fwd','reload','star','dl','hist','settings'])
    document.getElementById(id).innerHTML = I[id];
  document.querySelector('#shield .icw').innerHTML = I.shield;

  let state = { tabs: [], active: '', adblock: true };
  let addrFocused = false;
  const addr = document.getElementById('addr');
  const addrwrap = document.getElementById('addrwrap');
  const suggestEl = document.getElementById('suggest');

  function render() {
    const tabs = document.getElementById('tabs');
    tabs.innerHTML = '';
    for (const t of state.tabs) {
      const el = document.createElement('div');
      el.className = 'tab' + (t.id === state.active ? ' active' : '');
      el.title = t.url;
      if (t.loading) {
        const s = document.createElement('div'); s.className = 'spin'; el.appendChild(s);
      } else if (t.fav) {
        const img = document.createElement('img'); img.className = 'fav'; img.src = t.fav;
        img.onerror = () => { const d = document.createElement('span'); d.className = 'favdot';
                              d.innerHTML = I.globe; img.replaceWith(d); };
        el.appendChild(img);
      } else {
        const d = document.createElement('span'); d.className = 'favdot'; d.innerHTML = I.globe;
        el.appendChild(d);
      }
      const label = document.createElement('span'); label.className = 't';
      label.textContent = t.title || 'New Tab'; el.appendChild(label);
      if (t.audio || t.muted) {
        const snd = document.createElement('button'); snd.className = 'snd';
        snd.innerHTML = t.muted ? I.mute : I.snd;
        snd.title = t.muted ? 'Unmute tab' : 'Mute tab';
        snd.onclick = (e) => { e.stopPropagation(); send('mute', t.id); };
        el.appendChild(snd);
      }
      const x = document.createElement('button'); x.className = 'x'; x.innerHTML = I.close;
      x.title = 'Close tab (Ctrl+W)';
      x.onclick = (e) => { e.stopPropagation(); send('close', t.id); };
      el.appendChild(x);
      el.onclick = () => send('select', t.id);
      el.onauxclick = (e) => { if (e.button === 1) send('close', t.id); };
      el.draggable = true;
      el.ondragstart = (e) => { e.dataTransfer.setData('text/plain', t.id); e.dataTransfer.effectAllowed = 'move'; };
      el.ondragover = (e) => { e.preventDefault(); e.dataTransfer.dropEffect = 'move'; };
      el.ondrop = (e) => {
        e.preventDefault();
        const from = e.dataTransfer.getData('text/plain');
        if (from && from !== t.id) send('reorder', from + '|' + t.id);
      };
      tabs.appendChild(el);
    }
    const nt = document.createElement('button'); nt.id = 'newtab'; nt.innerHTML = I.plus;
    nt.title = 'New tab (Ctrl+T)'; nt.onclick = () => send('newtab');
    tabs.appendChild(nt);

    const active = state.tabs.find(t => t.id === state.active);
    document.getElementById('back').disabled = !active || !active.canBack;
    document.getElementById('fwd').disabled = !active || !active.canFwd;
    if (!addrFocused) addr.value = active ? active.url.replace(/^about:blank$/, '') : '';
    const lock = document.getElementById('lock');
    const u = active ? active.url : '';
    if (/^https:/.test(u)) { lock.innerHTML = I.lock; lock.className = 'secure'; }
    else if (/^http:/.test(u)) { lock.innerHTML = I.globe; lock.className = ''; }
    else { lock.innerHTML = I.search; lock.className = ''; }
    document.getElementById('star').classList.toggle('active', !!(active && active.bookmarked));
    const shield = document.getElementById('shield');
    shield.className = state.adblock ? 'on' : 'off';
    shield.title = state.adblock
      ? 'Ad & tracker blocking is on' + (active && active.blocked ? ' — ' + active.blocked + ' blocked on this page' : '') + '. Click to turn off.'
      : 'Ad & tracker blocking is off. Click to turn on.';
    let badge = document.getElementById('shieldCount');
    if (badge) badge.remove();
    if (state.adblock && active && active.blocked > 0) {
      badge = document.createElement('span'); badge.id = 'shieldCount';
      badge.textContent = active.blocked > 99 ? '99+' : active.blocked;
      shield.appendChild(badge);
    }
  }
)HTML" LR"HTML(
  // ---- suggestions dropdown (history/bookmarks/search/AI) ----
  let sugg = [], selIdx = -1, curDip = 0, inlineMode = false;
  let suggestTimer = null;

  function looksLikeQuestion(v) {
    v = v.trim();
    if (!v || /^(https?:\/\/|www\.)/i.test(v)) return false;
    if (v.includes('.') && !v.includes(' ')) return false;
    if (/\?$/.test(v)) return true;
    return /^(who|what|when|where|why|how|is|are|can|does|do|should|which|will)\b/i.test(v);
  }

  function setSuggestDip(d) {
    if (d !== curDip) { curDip = d; send('suggest', String(d)); }
  }

  function hideSuggest() {
    if (suggestTimer) { clearTimeout(suggestTimer); suggestTimer = null; }
    selIdx = -1; sugg = []; inlineMode = false;
    suggestEl.style.display = 'none';
    setSuggestDip(0);
  }

  function suggRows() {
    const q = addr.value.trim();
    const rows = [];
    if (!q) return rows;
    if (looksLikeQuestion(q)) rows.push({kind:'ai', title:q});
    rows.push({kind:'go', title:q});
    for (const s of sugg) rows.push(s);
    return rows.slice(0, 8);
  }

  function activateRow(r) {
    if (suggestTimer) { clearTimeout(suggestTimer); suggestTimer = null; }
    if (r.kind === 'ai') { askInline(r.title); return; }
    send('navigate', r.kind === 'go' ? r.title : r.url);
    hideSuggest();
    addr.blur();
  }

  function renderSuggest() {
    if (inlineMode) return;
    const rows = suggRows();
    if (!addrFocused || rows.length === 0) { hideSuggest(); return; }
    suggestEl.innerHTML = '';
    rows.forEach((r, i) => {
      const el = document.createElement('div');
      el.className = 'sugrow' + (i === selIdx ? ' sel' : '');
      const ic = document.createElement('span');
      ic.className = 'ic' + (r.kind === 'ai' ? ' ai' : '');
      ic.innerHTML = r.kind === 'ai' ? '✦' : r.kind === 'go' ? I.search : r.kind === 'b' ? I.star : I.hist;
      const txt = document.createElement('span'); txt.className = 'txt';
      if (r.kind === 'ai') {
        txt.append('Ask AI: ');
        const b = document.createElement('b'); b.textContent = r.title; txt.appendChild(b);
      } else if (r.kind === 'go') {
        const b = document.createElement('b'); b.textContent = r.title; txt.appendChild(b);
        txt.append(' — search');
      } else {
        txt.textContent = r.title || r.url;
      }
      el.appendChild(ic); el.appendChild(txt);
      if (r.url) {
        const u = document.createElement('span'); u.className = 'url'; u.textContent = r.url;
        el.appendChild(u);
      }
      el.addEventListener('mousedown', (e) => { e.preventDefault(); activateRow(r); });
      suggestEl.appendChild(el);
    });
    suggestEl.style.display = 'block';
    setSuggestDip(rows.length * 36 + 12);
  }

  function updateSuggest() {
    const q = addr.value.trim();
    selIdx = -1; inlineMode = false;
    if (suggestTimer) { clearTimeout(suggestTimer); suggestTimer = null; }
    if (!q) { hideSuggest(); return; }
    send('query', q);
    if (looksLikeQuestion(q))
      suggestTimer = setTimeout(() => { suggestTimer = null; askInline(addr.value.trim()); }, 650);
    renderSuggest();
  }

  window.chrome.webview.addEventListener('message', (e) => {
    if (e.data && e.data.sugg) {
      if (e.data.q === addr.value.trim()) { sugg = e.data.sugg; renderSuggest(); }
      return;
    }
    if (e.data && e.data.find) { updateFindCount(e.data.find); return; }
    state = e.data;
    render();
    if (pendingInlineQuery) {
      if (state.aiPort) {
        const q = pendingInlineQuery, body = pendingInlineBody;
        pendingInlineQuery = null; pendingInlineBody = null;
        body.innerHTML = dotsHtml;
        runInlineFetch(q, body);
      } else if (state.aiPhase === 6) {
        pendingInlineBody.textContent = 'Could not start the AI engine.';
        pendingInlineQuery = null; pendingInlineBody = null;
      }
    }
  });
)HTML" LR"HTML(
  // ---- inline AI answer box ----
  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  /// Minimal inline-only markdown (bold/italic/code/links) — good enough for short answers.
  function renderMarkdownLite(md) {
    let s = escapeHtml(md);
    s = s.replace(/```([\s\S]*?)```/g, (m, code) => '<pre><code>' + code.trim() + '</code></pre>');
    s = s.replace(/^[ \t]*[-*][ \t]+/gm, '• ');
    s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
    s = s.replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>');
    s = s.replace(/(^|[^*])\*([^*]+)\*/g, '$1<i>$2</i>');
    s = s.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
    return s.replace(/\n/g, '<br>');
  }

  function openFullChat(query) {
    send('askai', query);
    hideSuggest();
    addr.blur();
  }

  let pendingInlineQuery = null, pendingInlineBody = null;

  function buildInlineBox(query, initialBodyHtml) {
    inlineMode = true;
    setSuggestDip(230);
    suggestEl.innerHTML = '';
    const box = document.createElement('div');
    box.className = 'aibox';
    const head = document.createElement('div'); head.className = 'aihead';
    const ic = document.createElement('span'); ic.className = 'ic'; ic.textContent = '✦';
    const b = document.createElement('b'); b.textContent = query;
    head.appendChild(ic); head.appendChild(b);
    const body = document.createElement('div'); body.className = 'aibody';
    body.innerHTML = initialBodyHtml;
    const more = document.createElement('button'); more.className = 'aimore';
    more.textContent = 'Continue in AI chat →';
    more.addEventListener('mousedown', (e) => { e.preventDefault(); openFullChat(query); });
    box.appendChild(head); box.appendChild(body); box.appendChild(more);
    suggestEl.appendChild(box);
    suggestEl.style.display = 'block';
    return body;
  }

  const dotsHtml = '<div class="dots"><span></span><span></span><span></span></div>';

  function runInlineFetch(query, body) {
    fetch('http://127.0.0.1:' + state.aiPort + '/v1/chat/completions', {
      method: 'POST',
      headers: {'Content-Type':'application/json'},
      body: JSON.stringify({messages:[{role:'user', content:query}], stream:true})
    }).then(async (res) => {
      const reader = res.body.getReader();
      const decoder = new TextDecoder();
      let buf = '', full = '';
      while (true) {
        const {done, value} = await reader.read();
        if (done) break;
        buf += decoder.decode(value, {stream:true});
        const lines = buf.split('\n');
        buf = lines.pop();
        for (const line of lines) {
          const t = line.trim();
          if (!t.startsWith('data:')) continue;
          const data = t.slice(5).trim();
          if (data === '[DONE]') continue;
          try {
            const json = JSON.parse(data);
            const delta = json.choices && json.choices[0] && json.choices[0].delta && json.choices[0].delta.content;
            if (delta) { full += delta; body.innerHTML = renderMarkdownLite(full); }
          } catch (e) {}
        }
      }
    }).catch(() => { body.textContent = 'Could not reach the AI engine.'; });
  }

  function askInline(query) {
    if (state.aiPort) {
      const body = buildInlineBox(query, dotsHtml);
      runInlineFetch(query, body);
      return;
    }
    if (state.aiPhase >= 1 && state.aiPhase <= 4) {
      pendingInlineQuery = query;
      pendingInlineBody = buildInlineBox(query, '<span style="color:#999">Starting the on-device AI…</span>');
      return;
    }
    openFullChat(query);
  }
)HTML" LR"HTML(
  // ---- command palette (Ctrl+K): natural-language commands + AI ----
  const paletteEl = document.getElementById('palette');
  const pcmd = document.getElementById('pcmd');
  const plist = document.getElementById('plist');
  let pOpen = false, pSel = 0, pRows = [];
  const COMMANDS = [
    {label:'Ask AI about this page', hint:'Summarize & chat', icon:'✦', ai:true, run:() => send('sidebar')},
    {label:'New tab', hint:'Ctrl+T', icon:I.plus, run:() => send('newtab')},
    {label:'Reopen closed tab', hint:'Ctrl+Shift+T', icon:I.hist, run:() => send('reopen')},
    {label:'Bookmark this page', hint:'Ctrl+D', icon:I.star, run:() => send('bookmark')},
    {label:'Find in page', hint:'Ctrl+F', icon:I.search, run:() => window.__openFind()},
    {label:'Downloads', hint:'Ctrl+J', icon:I.dl, run:() => send('downloads')},
    {label:'History', hint:'Ctrl+H', icon:I.hist, run:() => send('history')},
    {label:'Settings', hint:'Ctrl+,', icon:I.settings, run:() => send('settings')},
    {label:'Toggle ad & tracker blocking', hint:'', icon:I.shield, run:() => send('shield')},
    {label:'Reload page', hint:'Ctrl+R', icon:I.reload, run:() => send('reload')},
  ];
  function pcompute() {
    const q = pcmd.value.trim(), ql = q.toLowerCase();
    let rows = COMMANDS.filter(c => !ql || c.label.toLowerCase().includes(ql));
    if (q) {
      rows = rows.slice(0, 5);
      rows.unshift({label:'Ask AI: ' + q, hint:'Enter', icon:'✦', ai:true, run:() => send('askai', q)});
      if (/\./.test(q) && !/\s/.test(q))
        rows.push({label:'Go to ' + q, hint:'', icon:I.globe, run:() => send('navigate', q)});
    }
    return rows.slice(0, 8);
  }
  function prender() {
    pRows = pcompute();
    if (pSel >= pRows.length) pSel = Math.max(0, pRows.length - 1);
    plist.innerHTML = '';
    pRows.forEach((r, i) => {
      const el = document.createElement('div');
      el.className = 'prow' + (i === pSel ? ' sel' : '');
      el.innerHTML = '<span class="pi' + (r.ai ? ' ai' : '') + '">' + r.icon + '</span>' +
                     '<span class="pl"></span><span class="ph">' + (r.hint || '') + '</span>';
      el.querySelector('.pl').textContent = r.label;
      el.addEventListener('mousedown', (e) => { e.preventDefault(); prun(r); });
      plist.appendChild(el);
    });
    setSuggestDip(64 + Math.min(pRows.length, 8) * 40 + 12);
  }
  function openPalette() {
    hideSuggest(); addr.blur();
    if (window.__closeFind) window.__closeFind();
    pOpen = true; pSel = 0; pcmd.value = '';
    paletteEl.style.display = 'block';
    prender();
    setTimeout(() => pcmd.focus(), 0);
  }
  function closePalette() {
    if (!pOpen) return;
    pOpen = false; paletteEl.style.display = 'none';
    setSuggestDip(0);
  }
  function prun(r) { closePalette(); if (r && r.run) r.run(); }
  window.__openPalette = openPalette;
  pcmd.addEventListener('input', () => { pSel = 0; prender(); });
  pcmd.addEventListener('blur', () => { setTimeout(closePalette, 120); });
  pcmd.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowDown' && pRows.length) { e.preventDefault(); pSel = (pSel + 1) % pRows.length; prender(); }
    else if (e.key === 'ArrowUp' && pRows.length) { e.preventDefault(); pSel = (pSel - 1 + pRows.length) % pRows.length; prender(); }
    else if (e.key === 'Enter') { e.preventDefault(); if (pRows[pSel]) prun(pRows[pSel]); }
    else if (e.key === 'Escape') { e.preventDefault(); closePalette(); }
  });

  // ---- find-in-page bar (Ctrl+F) ----
  const findbarEl = document.getElementById('findbar');
  const fq = document.getElementById('fq');
  const fcount = document.getElementById('fcount');
  let findOpen = false, findTimer = null;
  window.__openFind = () => {
    closePalette(); hideSuggest(); addr.blur();
    findOpen = true; findbarEl.style.display = 'block';
    setSuggestDip(52);
    setTimeout(() => { fq.focus(); fq.select(); }, 0);
  };
  window.__closeFind = () => {
    if (!findOpen) return;
    findOpen = false; findbarEl.style.display = 'none'; fcount.textContent = '';
    setSuggestDip(0);
  };
  function closeFindUser() { send('findstop'); window.__closeFind(); }
  function updateFindCount(f) {
    fcount.textContent = f.n < 0 ? '' : (f.n === 0 ? '0/0' : Math.max(1, f.i) + '/' + f.n);
  }
  fq.addEventListener('input', () => {
    if (findTimer) clearTimeout(findTimer);
    findTimer = setTimeout(() => { findTimer = null; send('find', fq.value.trim()); }, 200);
  });
  fq.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { e.preventDefault(); send(e.shiftKey ? 'findprev' : 'findnext'); }
    else if (e.key === 'Escape') { e.preventDefault(); closeFindUser(); }
  });
  document.getElementById('fnext').onclick = () => send('findnext');
  document.getElementById('fprev').onclick = () => send('findprev');
  document.getElementById('fclose').onclick = closeFindUser;

  // ---- wiring ----
  addr.addEventListener('focus', () => { addrFocused = true; addrwrap.classList.add('focus'); addr.select(); });
  addr.addEventListener('blur', () => { addrFocused = false; addrwrap.classList.remove('focus'); render(); hideSuggest(); });
  addr.addEventListener('input', updateSuggest);
  addr.addEventListener('keydown', (e) => {
    const rows = suggRows();
    if (e.key === 'ArrowDown' && rows.length && !inlineMode) {
      e.preventDefault(); selIdx = (selIdx + 1) % rows.length; renderSuggest();
    } else if (e.key === 'ArrowUp' && rows.length && !inlineMode) {
      e.preventDefault(); selIdx = (selIdx - 1 + rows.length) % rows.length; renderSuggest();
    } else if (e.key === 'Enter' && addr.value.trim()) {
      if (selIdx >= 0 && rows[selIdx] && !inlineMode) { activateRow(rows[selIdx]); }
      else { send('navigate', addr.value.trim()); hideSuggest(); addr.blur(); }
    } else if (e.key === 'Escape') {
      hideSuggest(); addr.blur();
    }
  });
  document.getElementById('back').onclick = () => send('back');
  document.getElementById('fwd').onclick = () => send('forward');
  document.getElementById('reload').onclick = () => send('reload');
  document.getElementById('star').onclick = () => send('bookmark');
  document.getElementById('shield').onclick = () => send('shield');
  document.getElementById('ai').onclick = () => send('sidebar');
  document.getElementById('dl').onclick = () => send('downloads');
  document.getElementById('hist').onclick = () => send('history');
  document.getElementById('settings').onclick = () => send('settings');

  document.addEventListener('keydown', (e) => {
    if (e.ctrlKey && e.shiftKey && e.key.toLowerCase() === 't') { e.preventDefault(); send('reopen'); }
    else if (e.ctrlKey && e.key.toLowerCase() === 't') { e.preventDefault(); send('newtab'); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'w') { e.preventDefault(); send('close', state.active); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'l') { e.preventDefault(); addr.focus(); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'k') { e.preventDefault(); openPalette(); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'f') { e.preventDefault(); window.__openFind(); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'j') { e.preventDefault(); send('downloads'); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'r') { e.preventDefault(); send('reload'); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'd') { e.preventDefault(); send('bookmark'); }
    else if (e.ctrlKey && e.key.toLowerCase() === 'h') { e.preventDefault(); send('history'); }
    else if (e.ctrlKey && e.key === ',') { e.preventDefault(); send('settings'); }
  });
  send('ready');
</script></body></html>)HTML";

// Start page shown in fresh tabs; head/tail are static, tiles are injected per-build.
static const wchar_t* kStartHtmlHead = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>New Tab</title><style>
  :root { color-scheme: light dark; }
  body { margin:0; min-height:100vh; display:flex; flex-direction:column; align-items:center;
         padding-top:14vh; font:16px "Segoe UI",system-ui;
         background:linear-gradient(135deg,#fafafa,#e8e8ee); }
  @media (prefers-color-scheme: dark) { body { background:linear-gradient(135deg,#101014,#1a1a22); color:#eee; } }
  h1 { font-size:56px; background:linear-gradient(90deg,#64748b,#2563eb);
       -webkit-background-clip:text; color:transparent; margin:0 0 6px; }
  p.hint { color:#888; margin:0 0 32px; }
  .tiles { display:flex; flex-wrap:wrap; gap:16px; max-width:640px; justify-content:center; }
  .tile { display:flex; flex-direction:column; align-items:center; gap:6px; width:84px;
          text-decoration:none; color:inherit; }
  .tile .fav { width:44px; height:44px; border-radius:12px; background:rgba(0,0,0,.06); display:flex;
               align-items:center; justify-content:center; font-size:18px; font-weight:600; color:#666; }
  @media (prefers-color-scheme: dark) { .tile .fav { background:rgba(255,255,255,.09); color:#ccc; } }
  .tile span { font-size:12px; text-align:center; overflow:hidden; text-overflow:ellipsis; white-space:nowrap;
               max-width:100%; color:#777; }
  .tile:hover .fav { background:rgba(74,128,245,.18); }
</style></head><body>
  <h1>minima</h1>
  <p class="hint">Press <b>Ctrl+L</b> and type to search or enter an address.</p>
  <div class="tiles">)HTML";
static const wchar_t* kStartHtmlTail = L"</div></body></html>";

// History page shown in a tab via Ctrl+H.
static const wchar_t* kHistoryHtmlHead = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>History</title><style>
  :root { color-scheme: light dark; }
  body { margin:0; font:14px "Segoe UI",system-ui; padding:24px 32px; background:#fafafa; }
  @media (prefers-color-scheme: dark) { body { background:#16161a; color:#eee; } }
  h1 { font-size:20px; margin:0 0 16px; }
  .row { display:flex; flex-direction:column; gap:2px; padding:8px 10px; border-radius:8px;
         text-decoration:none; color:inherit; }
  .row:hover { background:rgba(0,0,0,.06); }
  @media (prefers-color-scheme: dark) { .row:hover { background:rgba(255,255,255,.08); } }
  .row .t { font-weight:600; }
  .row .u { font-size:12px; color:#888; }
  .empty { color:#888; }
</style></head><body><h1>History</h1>)HTML";
static const wchar_t* kHistoryHtmlTail = L"</body></html>";

// AI chat page shown via Ctrl+H-style internal navigation ("minima:" scheme actions handled in C++).
static const wchar_t* kAiHtmlHead = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Minima AI</title><style>
  :root { color-scheme: light dark; }
  * { box-sizing:border-box; }
  html, body { overflow:hidden; }
  ::-webkit-scrollbar { width:8px; height:8px; }
  ::-webkit-scrollbar-thumb { background:rgba(0,0,0,.18); border-radius:4px; }
  ::-webkit-scrollbar-track { background:transparent; }
  @media (prefers-color-scheme: dark) { ::-webkit-scrollbar-thumb { background:rgba(255,255,255,.18); } }
  body { margin:0; font:14px/1.5 "Segoe UI",system-ui; height:100vh; display:flex; flex-direction:column;
         background:#fafafa; color:#1a1a1a; }
  @media (prefers-color-scheme: dark) { body { background:#16161a; color:#eee; } }

  #setup { margin:auto; text-align:center; width:340px; padding:24px; }
  #setup .icon { width:52px; height:52px; margin:0 auto 18px; border-radius:16px;
                 background:linear-gradient(135deg,#4a80f5,#7c5cff); display:flex; align-items:center;
                 justify-content:center; font-size:24px; color:#fff; box-shadow:0 6px 20px rgba(74,128,245,.35); }
  #setup h1 { font-size:18px; margin:0 0 8px; font-weight:600; }
  #setup p { color:#888; font-size:13px; margin:0 0 26px; }
  #setup p.err { color:#e0555f; }
  #setup .progress { display:none; }
  #setup .bar { height:6px; border-radius:3px; background:rgba(0,0,0,.08); overflow:hidden; margin-bottom:10px; }
  @media (prefers-color-scheme: dark) { #setup .bar { background:rgba(255,255,255,.1); } }
  #setup .fill { height:100%; width:0%; border-radius:3px; background:linear-gradient(90deg,#4a80f5,#7c5cff);
                 transition:width .25s ease; }
  #setup .status { display:flex; justify-content:space-between; font-size:12px; color:#999; margin-bottom:22px; }
  #setup button { background:#4a80f5; color:#fff; border:none; border-radius:10px; padding:11px 24px;
                  font:13px/1 "Segoe UI",system-ui; font-weight:600; cursor:pointer; }
  #setup button:hover { background:#3a6fe0; }
  #setup button:disabled { opacity:.45; cursor:default; }

  #chat { display:none; flex-direction:column; height:100%; min-height:0; }
  #msgs { flex:1; min-height:0; overflow-y:auto; padding:22px 15%; display:flex; flex-direction:column; gap:18px; }
  .msg { max-width:100%; }
  .msg.user { align-self:flex-end; background:#4a80f5; color:#fff; padding:9px 14px;
              border-radius:16px 16px 3px 16px; font-size:13.5px; }
  .msg.ai { align-self:flex-start; padding:1px 0; width:100%; }
  .msg.ai p { margin:0 0 10px; }
  .msg.ai p:last-child { margin-bottom:0; }
  .msg.ai h1, .msg.ai h2, .msg.ai h3 { margin:16px 0 8px; font-weight:600; line-height:1.3; }
  .msg.ai h1:first-child, .msg.ai h2:first-child, .msg.ai h3:first-child { margin-top:0; }
  .msg.ai h1 { font-size:19px; } .msg.ai h2 { font-size:16.5px; } .msg.ai h3 { font-size:14.5px; }
  .msg.ai ul, .msg.ai ol { margin:0 0 10px 20px; padding:0; }
  .msg.ai li { margin:3px 0; }
  .msg.ai code { background:rgba(0,0,0,.07); padding:2px 5px; border-radius:4px;
                 font:12.5px/1.4 "Cascadia Code","Consolas",monospace; }
  .msg.ai pre { background:rgba(0,0,0,.055); padding:12px 14px; border-radius:10px; overflow-x:auto;
                margin:4px 0 12px; }
  .msg.ai pre code { background:none; padding:0; }
  @media (prefers-color-scheme: dark) {
    .msg.ai code { background:rgba(255,255,255,.1); }
    .msg.ai pre { background:rgba(255,255,255,.07); }
  }
  .msg.ai a { color:#4a80f5; }
  .dots { display:inline-flex; gap:5px; padding:6px 0; }
  .dots span { width:6px; height:6px; border-radius:50%; background:#aaa; animation:bounce 1.2s infinite ease-in-out; }
  .dots span:nth-child(2) { animation-delay:.15s; }
  .dots span:nth-child(3) { animation-delay:.3s; }
  @keyframes bounce { 0%,80%,100% { transform:scale(.6); opacity:.4; } 40% { transform:scale(1); opacity:1; } }

  #input { display:flex; align-items:flex-end; gap:8px; padding:14px 15%; border-top:1px solid rgba(0,0,0,.08); }
  @media (prefers-color-scheme: dark) { #input { border-color:rgba(255,255,255,.1); } }
  #q { flex:1; padding:11px 16px; border-radius:20px; border:none; background:rgba(0,0,0,.06);
       font:13.5px "Segoe UI",system-ui; outline:none; color:inherit; resize:none; max-height:120px; }
  @media (prefers-color-scheme: dark) { #q { background:rgba(255,255,255,.08); } }
  #send { border:none; background:rgba(0,0,0,.08); color:#999; width:36px; height:36px; flex:none;
          border-radius:50%; cursor:pointer; display:flex; align-items:center; justify-content:center;
          transition:background .15s, color .15s, transform .1s; }
  @media (prefers-color-scheme: dark) { #send { background:rgba(255,255,255,.1); color:#aaa; } }
  #send.ready { background:#4a80f5; color:#fff; }
  #send.ready:hover { background:#3a6fe0; }
  #send.ready:active { transform:scale(.92); }
  #send:disabled { cursor:default; opacity:.5; }
  #send svg { width:16px; height:16px; }
</style></head><body>
  <div id="setup">
    <div class="icon">&#10022;</div>
    <h1>Minima AI</h1>
    <p id="setupMsg">A small Gemma model that runs fully on this device &mdash; nothing leaves your
      computer. First run downloads the engine (~60&nbsp;MB) and model (~800&nbsp;MB).</p>
    <div class="progress" id="progress">
      <div class="bar"><div class="fill" id="fill"></div></div>
      <div class="status"><span id="statusLabel">Starting&hellip;</span><span id="statusPct"></span></div>
    </div>
    <button id="go">Download &amp; start</button>
  </div>
  <div id="chat">
    <div id="msgs"></div>
    <div id="input">
      <textarea id="q" rows="1" placeholder="Ask anything..."></textarea>
      <button id="send" title="Send" disabled>
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round">
          <line x1="12" y1="19" x2="12" y2="5"></line>
          <polyline points="6 11 12 5 18 11"></polyline>
        </svg>
      </button>
    </div>
  </div>
<script>)HTML";
static const wchar_t* kAiHtmlTail = LR"HTML(
  const setupMsg = document.getElementById('setupMsg');
  const fillEl = document.getElementById('fill');
  const progressEl = document.getElementById('progress');
  const statusLabel = document.getElementById('statusLabel');
  const statusPct = document.getElementById('statusPct');
  const goBtn = document.getElementById('go');
  const chatEl = document.getElementById('chat');
  const setupEl = document.getElementById('setup');
  const msgsEl = document.getElementById('msgs');
  const qEl = document.getElementById('q');
  const sendBtn = document.getElementById('send');
  let history = [];

  function showChat() {
    setupEl.style.display = 'none';
    chatEl.style.display = 'flex';
    qEl.focus();
    if (initialQuery) { qEl.value = initialQuery; initialQuery = ''; sendMsg(); }
  }

  goBtn.onclick = () => {
    goBtn.disabled = true;
    goBtn.textContent = 'Starting…';
    setupMsg.className = '';
    progressEl.style.display = 'block';
    window.chrome.webview.postMessage('ai-start');
  };

  window.__minimaAiStatus = (phase, downloaded, total, p, err) => {
    if (phase === 5) { port = p; showChat(); return; }
    if (phase === 6) {
      setupMsg.textContent = err || 'Setup failed.';
      setupMsg.className = 'err';
      progressEl.style.display = 'none';
      goBtn.disabled = false;
      goBtn.textContent = 'Try again';
      return;
    }
    const labels = {1:'Downloading engine', 2:'Extracting engine', 3:'Downloading model', 4:'Starting engine'};
    setupMsg.textContent = 'Setting up Minima AI…';
    setupMsg.className = '';
    progressEl.style.display = 'block';
    goBtn.disabled = true;
    goBtn.textContent = 'Working…';
    statusLabel.textContent = labels[phase] || 'Working…';
    if (total > 0) {
      const pct = Math.min(100, downloaded / total * 100);
      fillEl.style.width = pct + '%';
      statusPct.textContent = Math.round(pct) + '%';
    } else {
      fillEl.style.width = '100%';
      statusPct.textContent = '';
    }
  };

  if (port > 0) {
    showChat();
  } else if (initPhase > 0) {
    window.__minimaAiStatus(initPhase, initDl, initTotal, 0, '');
  } else {
    goBtn.click();
  }

  function escapeHtml(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  function inlineMd(s) {
    s = escapeHtml(s);
    s = s.replace(/`([^`]+)`/g, '<code>$1</code>');
    s = s.replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>');
    s = s.replace(/(^|[^*])\*([^*]+)\*/g, '$1<i>$2</i>');
    s = s.replace(/\[([^\]]+)\]\(([^)\s]+)\)/g, '<a href="$2" target="_blank" rel="noopener">$1</a>');
    return s;
  }

  function renderMarkdown(md) {
    const blocks = [];
    md = md.replace(/```(?:[a-zA-Z0-9]*\n)?([\s\S]*?)```/g, (m, code) => {
      blocks.push('<pre><code>' + escapeHtml(code.replace(/\n$/, '')) + '</code></pre>');
      return '\x00' + (blocks.length - 1) + '\x00';
    });
    const lines = md.split('\n');
    let html = '', inUl = false, inOl = false;
    const closeLists = () => {
      if (inUl) { html += '</ul>'; inUl = false; }
      if (inOl) { html += '</ol>'; inOl = false; }
    };
    for (const raw of lines) {
      const line = raw;
      const blockRef = line.trim().match(/^\x00(\d+)\x00$/);
      if (blockRef) { closeLists(); html += blocks[parseInt(blockRef[1])]; continue; }
      const h = line.match(/^(#{1,3})\s+(.*)/);
      if (h) { closeLists(); html += `<h${h[1].length}>${inlineMd(h[2])}</h${h[1].length}>`; continue; }
      const ul = line.match(/^\s*[-*]\s+(.*)/);
      if (ul) {
        if (inOl) { html += '</ol>'; inOl = false; }
        if (!inUl) { html += '<ul>'; inUl = true; }
        html += `<li>${inlineMd(ul[1])}</li>`;
        continue;
      }
      const ol = line.match(/^\s*\d+\.\s+(.*)/);
      if (ol) {
        if (inUl) { html += '</ul>'; inUl = false; }
        if (!inOl) { html += '<ol>'; inOl = true; }
        html += `<li>${inlineMd(ol[1])}</li>`;
        continue;
      }
      closeLists();
      if (line.trim() !== '') html += `<p>${inlineMd(line)}</p>`;
    }
    closeLists();
    return html;
  }

  function addMsg(role, text) {
    const el = document.createElement('div');
    el.className = 'msg ' + (role === 'user' ? 'user' : 'ai');
    if (role === 'user') el.textContent = text;
    msgsEl.appendChild(el);
    msgsEl.scrollTop = msgsEl.scrollHeight;
    return el;
  }

  function autoGrow() {
    qEl.style.height = 'auto';
    qEl.style.height = Math.min(120, qEl.scrollHeight) + 'px';
    const has = qEl.value.trim().length > 0;
    sendBtn.classList.toggle('ready', has);
    sendBtn.disabled = !has;
  }
  qEl.addEventListener('input', autoGrow);

  async function sendMsg() {
    const text = qEl.value.trim();
    if (!text || !port) return;
    qEl.value = '';
    autoGrow();
    addMsg('user', text);
    history.push({role:'user', content:text});
    const aiEl = addMsg('ai', '');
    aiEl.innerHTML = '<div class="dots"><span></span><span></span><span></span></div>';
    sendBtn.disabled = true;
    try {
      const res = await fetch('http://127.0.0.1:' + port + '/v1/chat/completions', {
        method: 'POST',
        headers: {'Content-Type':'application/json'},
        body: JSON.stringify({messages: history, stream: true})
      });
      const reader = res.body.getReader();
      const decoder = new TextDecoder();
      let buf = '', full = '';
      while (true) {
        const {done, value} = await reader.read();
        if (done) break;
        buf += decoder.decode(value, {stream:true});
        const lines = buf.split('\n');
        buf = lines.pop();
        for (const line of lines) {
          const t = line.trim();
          if (!t.startsWith('data:')) continue;
          const data = t.slice(5).trim();
          if (data === '[DONE]') continue;
          try {
            const json = JSON.parse(data);
            const delta = json.choices && json.choices[0] && json.choices[0].delta && json.choices[0].delta.content;
            if (delta) {
              full += delta;
              aiEl.innerHTML = renderMarkdown(full);
              msgsEl.scrollTop = msgsEl.scrollHeight;
            }
          } catch (e) {}
        }
      }
      history.push({role:'assistant', content:full});
    } catch (e) {
      aiEl.innerHTML = '<p style="color:#e0555f">Could not reach the AI engine.</p>';
    }
    autoGrow();
  }

  sendBtn.onclick = sendMsg;
  qEl.addEventListener('keydown', (e) => {
    if (e.key === 'Enter' && !e.shiftKey) { e.preventDefault(); sendMsg(); }
  });
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// "Ask this page" sidebar — a docked, fully on-device AI panel that reads the
// current page's text and answers questions / summarizes it. Signature feature:
// unlike Edge/Chrome's cloud copilots, the page content never leaves the device.
// ---------------------------------------------------------------------------
static const wchar_t* kSidebarHtml = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Ask this page</title><style>
  :root { color-scheme: light dark; --acc:#7c5cff; --acc2:#4a80f5; }
  * { box-sizing:border-box; margin:0; padding:0; }
  html, body { height:100%; }
  ::-webkit-scrollbar { width:7px; }
  ::-webkit-scrollbar-thumb { background:rgba(128,128,128,.35); border-radius:4px; }
  body { font:13.5px/1.5 "Segoe UI",system-ui; display:flex; flex-direction:column; height:100vh;
         background:#f7f7fa; color:#1a1a1a; border-left:1px solid rgba(0,0,0,.08); }
  @media (prefers-color-scheme: dark) { body { background:#17171b; color:#eee; border-color:rgba(255,255,255,.08); } }
  header { display:flex; align-items:center; gap:9px; padding:12px 14px; border-bottom:1px solid rgba(0,0,0,.07); }
  @media (prefers-color-scheme: dark) { header { border-color:rgba(255,255,255,.08); } }
  header .logo { width:26px; height:26px; border-radius:8px; flex:none; display:flex; align-items:center;
                 justify-content:center; color:#fff; font-size:14px;
                 background:linear-gradient(135deg,var(--acc2),var(--acc)); }
  header .ht { flex:1; min-width:0; }
  header .h1 { font-weight:700; font-size:13.5px; }
  header .h2 { color:#888; font-size:11.5px; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  header button { border:none; background:none; color:#999; font-size:18px; cursor:pointer; padding:2px 6px;
                  border-radius:6px; }
  header button:hover { background:rgba(0,0,0,.08); color:inherit; }

  #setup { margin:auto; text-align:center; padding:30px 22px; }
  #setup .big { font-size:30px; margin-bottom:12px; }
  #setup p { color:#888; font-size:12.5px; margin-bottom:18px; }
  #setup button, .chip { border:none; border-radius:9px; padding:9px 16px; font:12.5px "Segoe UI",system-ui;
                         font-weight:600; cursor:pointer; background:var(--acc2); color:#fff; }
  #setup button:hover { background:#3a6fe0; }
  #setup .status { color:var(--acc2); font-size:12px; margin-top:14px; min-height:15px; }

  #main { display:none; flex-direction:column; flex:1; min-height:0; }
  .quick { display:flex; flex-wrap:wrap; gap:7px; padding:12px 14px; }
  .chip { background:rgba(0,0,0,.05); color:inherit; font-weight:500; }
  @media (prefers-color-scheme: dark) { .chip { background:rgba(255,255,255,.08); } }
  .chip:hover { background:rgba(124,92,255,.16); }
  #msgs { flex:1; min-height:0; overflow-y:auto; padding:6px 14px 14px; display:flex; flex-direction:column; gap:14px; }
  .msg.user { align-self:flex-end; max-width:88%; background:var(--acc2); color:#fff; padding:8px 12px;
              border-radius:14px 14px 3px 14px; font-size:13px; }
  .msg.ai { align-self:stretch; font-size:13px; }
  .msg.ai p { margin:0 0 8px; } .msg.ai p:last-child { margin:0; }
  .msg.ai ul, .msg.ai ol { margin:0 0 8px 18px; } .msg.ai li { margin:2px 0; }
  .msg.ai b { font-weight:700; }
  .msg.ai code { background:rgba(0,0,0,.07); padding:1px 4px; border-radius:4px; font-family:"Cascadia Code",monospace; }
  @media (prefers-color-scheme: dark) { .msg.ai code { background:rgba(255,255,255,.1); } }
  .dots span { display:inline-block; width:6px; height:6px; border-radius:50%; background:#aaa; margin-right:4px;
               animation:b 1.2s infinite; } .dots span:nth-child(2){animation-delay:.15s;} .dots span:nth-child(3){animation-delay:.3s;}
  @keyframes b { 0%,80%,100%{transform:scale(.6);opacity:.4;} 40%{transform:scale(1);opacity:1;} }
  #input { display:flex; gap:7px; padding:11px 14px; border-top:1px solid rgba(0,0,0,.07); }
  @media (prefers-color-scheme: dark) { #input { border-color:rgba(255,255,255,.08); } }
  #q { flex:1; border:none; border-radius:16px; padding:9px 13px; background:rgba(0,0,0,.06); outline:none;
       color:inherit; font:13px "Segoe UI",system-ui; resize:none; max-height:100px; }
  @media (prefers-color-scheme: dark) { #q { background:rgba(255,255,255,.08); } }
  #send { border:none; width:34px; height:34px; flex:none; border-radius:50%; cursor:pointer; color:#fff;
          background:var(--acc2); font-size:15px; }
  #send:disabled { opacity:.4; cursor:default; }
</style></head><body>)HTML" LR"HTML(
  <header>
    <div class="logo">&#10022;</div>
    <div class="ht"><div class="h1">Ask this page</div><div class="h2" id="ptitle">On-device AI</div></div>
    <button id="close" title="Close">&times;</button>
  </header>
  <div id="setup">
    <div class="big">&#10022;</div>
    <p>Ask questions about any page and get summaries &mdash; running fully on this device.
       Nothing you browse is sent to the cloud.</p>
    <button id="go">Set up on-device AI</button>
    <div class="status" id="sstatus"></div>
  </div>
  <div id="main">
    <div class="quick">
      <button class="chip" data-p="Summarize this page in 4-6 concise bullet points.">Summarize</button>
      <button class="chip" data-p="What are the key takeaways from this page? Be brief.">Key points</button>
      <button class="chip" data-p="Explain this page in simple terms.">Explain simply</button>
    </div>
    <div id="msgs"></div>
    <div id="input">
      <textarea id="q" rows="1" placeholder="Ask about this page…"></textarea>
      <button id="send" disabled>&#8593;</button>
    </div>
  </div>
<script>
  const send = (c, a='') => window.chrome.webview.postMessage(a === '' ? c : c + '\x1F' + a);
  let port = 0, phase = 0, pageText = '', pageTitle = '', pageUrl = '', history = [];
  const setupEl = document.getElementById('setup');
  const mainEl = document.getElementById('main');
  const msgsEl = document.getElementById('msgs');
  const sstatus = document.getElementById('sstatus');
  const qEl = document.getElementById('q');
  const sendBtn = document.getElementById('send');

  window.__sbState = (p, ph, title, url) => {
    port = p; phase = ph; pageTitle = title || ''; pageUrl = url || '';
    document.getElementById('ptitle').textContent = pageTitle || pageUrl || 'On-device AI';
    if (port > 0) { setupEl.style.display = 'none'; mainEl.style.display = 'flex'; }
    else {
      setupEl.style.display = 'block'; mainEl.style.display = 'none';
      const labels = {1:'Downloading engine…',2:'Extracting…',3:'Downloading model…',4:'Starting engine…'};
      sstatus.textContent = labels[ph] || '';
      document.getElementById('go').style.display = (ph >= 1 && ph <= 4) ? 'none' : 'inline-block';
    }
  };
  window.__sbContext = (text) => { pageText = text || ''; };

  document.getElementById('go').onclick = () => { sstatus.textContent = 'Starting…'; send('sb-setup'); };
  document.getElementById('close').onclick = () => send('sb-close');
  send('sb-ready');
)HTML" LR"HTML(
  function esc(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}
  function md(t){
    let s = esc(t);
    s = s.replace(/`([^`]+)`/g,'<code>$1</code>').replace(/\*\*([^*]+)\*\*/g,'<b>$1</b>');
    const lines = s.split('\n'); let out='', inUl=false;
    for (let ln of lines){
      const m = ln.match(/^\s*[-*]\s+(.*)/);
      if (m){ if(!inUl){out+='<ul>';inUl=true;} out+='<li>'+m[1]+'</li>'; continue; }
      if (inUl){out+='</ul>';inUl=false;}
      if (ln.trim()) out+='<p>'+ln+'</p>';
    }
    if (inUl) out+='</ul>';
    return out;
  }
  function addMsg(role, text){
    const el=document.createElement('div'); el.className='msg '+role;
    if (role==='user') el.textContent=text;
    msgsEl.appendChild(el); msgsEl.scrollTop=msgsEl.scrollHeight; return el;
  }
  function grow(){ qEl.style.height='auto'; qEl.style.height=Math.min(100,qEl.scrollHeight)+'px';
    const h=qEl.value.trim().length>0; sendBtn.disabled=!h; }
  qEl.addEventListener('input', grow);

  async function ask(prompt){
    if (!port) return;
    send('sb-context'); // refresh page text for the current page
    await new Promise(r => setTimeout(r, 60));
    addMsg('user', prompt);
    const aiEl = addMsg('ai',''); aiEl.innerHTML='<span class="dots"><span></span><span></span><span></span></span>';
    sendBtn.disabled = true;
    const sys = 'You are a helpful assistant answering questions about a web page the user is viewing. '
      + 'Use only the page content below. Be concise.\n\nPAGE TITLE: ' + pageTitle
      + '\nURL: ' + pageUrl + '\n\nPAGE CONTENT:\n' + pageText.slice(0, 7000);
    const msgs = [{role:'system', content:sys}].concat(history).concat([{role:'user', content:prompt}]);
    history.push({role:'user', content:prompt});
    try {
      const res = await fetch('http://127.0.0.1:'+port+'/v1/chat/completions',
        {method:'POST', headers:{'Content-Type':'application/json'}, body:JSON.stringify({messages:msgs, stream:true})});
      const rd = res.body.getReader(); const dec = new TextDecoder(); let buf='', full='';
      while(true){ const {done,value}=await rd.read(); if(done)break;
        buf+=dec.decode(value,{stream:true}); const ls=buf.split('\n'); buf=ls.pop();
        for(const line of ls){ const t=line.trim(); if(!t.startsWith('data:'))continue;
          const d=t.slice(5).trim(); if(d==='[DONE]')continue;
          try{ const j=JSON.parse(d); const dl=j.choices&&j.choices[0]&&j.choices[0].delta&&j.choices[0].delta.content;
            if(dl){ full+=dl; aiEl.innerHTML=md(full); msgsEl.scrollTop=msgsEl.scrollHeight; } }catch(e){}
        }
      }
      history.push({role:'assistant', content:full});
    } catch(e){ aiEl.innerHTML='<p style="color:#e0555f">Could not reach the on-device AI engine.</p>'; }
  }
  function sendMsg(){ const t=qEl.value.trim(); if(!t)return; qEl.value=''; grow(); ask(t); }
  sendBtn.onclick = sendMsg;
  qEl.addEventListener('keydown', (e)=>{ if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendMsg();} });
  document.querySelectorAll('.chip').forEach(c => c.onclick = () => ask(c.dataset.p));
</script></body></html>)HTML";
