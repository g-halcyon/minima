// pages.h — the History & Memory page and the Bookmarks page. Portable UI (part of the
// "core" per PORTING.md): each is head + an injected `const INIT = {...};` + tail.
#pragma once

// ---------------------------------------------------------------------------
// History & Memory (Ctrl+H). INIT = {recent:[{url,title,time}], memory:bool, pages:int, q:str}
// Search goes to the shell ("hist-search\x1Fq"), which answers window.__results({q, mem, hist}).
// ---------------------------------------------------------------------------
static const wchar_t* kHistoryPageHead = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>History</title><style>
  :root { color-scheme: light dark; --mut:#80838a; --hover:rgba(0,0,0,.05); --acc:#3b82f6; --line:rgba(0,0,0,.07); }
  @media (prefers-color-scheme: dark) { :root { --hover:rgba(255,255,255,.07); --line:rgba(255,255,255,.08); } }
  * { box-sizing:border-box; }
  body { margin:0; font:14px/1.45 "Segoe UI",system-ui; background:#fafafa; color:#1a1a1a; }
  @media (prefers-color-scheme: dark) { body { background:#16161a; color:#eee; } }
  .wrap { max-width:860px; margin:0 auto; padding:28px 28px 80px; }
  h1 { font-size:21px; margin:0 0 14px; }
  #q { width:100%; padding:12px 16px; font:15px "Segoe UI",system-ui; border-radius:12px; border:1px solid var(--line);
       background:#fff; color:inherit; outline:none; }
  @media (prefers-color-scheme: dark) { #q { background:#222327; } }
  #q:focus { border-color:var(--acc); box-shadow:0 0 0 3px rgba(59,130,246,.18); }
  .status { color:var(--mut); font-size:12.5px; margin:9px 2px 22px; display:flex; gap:10px; align-items:center; flex-wrap:wrap; }
  .status button { border:none; background:var(--acc); color:#fff; border-radius:7px; padding:5px 11px; font:12.5px "Segoe UI",system-ui;
                   font-weight:600; cursor:pointer; }
  h2 { font-size:12px; text-transform:uppercase; letter-spacing:.04em; color:var(--mut); margin:22px 4px 6px; font-weight:600; }
  .row { display:flex; align-items:flex-start; gap:10px; padding:9px 10px; border-radius:9px; }
  .row:hover { background:var(--hover); }
  .row a { flex:1; min-width:0; text-decoration:none; color:inherit; }
  .t { font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .u { font-size:12px; color:var(--mut); overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .s { font-size:13px; margin-top:2px; color:inherit; opacity:.86; display:-webkit-box; -webkit-line-clamp:2;
       -webkit-box-orient:vertical; overflow:hidden; }
  .s b { background:rgba(59,130,246,.18); color:inherit; border-radius:3px; padding:0 2px; font-weight:600; }
  .acts { display:none; gap:2px; flex:none; }
  .row:hover .acts { display:flex; }
  .acts button { border:none; background:none; color:var(--mut); font:12px "Segoe UI",system-ui; padding:4px 7px; border-radius:6px; cursor:pointer; }
  .acts button:hover { background:var(--hover); color:#e0555f; }
  .empty { color:var(--mut); padding:10px 4px; }
  .tm { font-size:11.5px; color:var(--mut); flex:none; padding-top:2px; }
</style></head><body><div class="wrap">
  <h1>History</h1>
  <input id="q" placeholder="Search history and the pages you've read" spellcheck="false" autocomplete="off" autofocus>
  <div class="status" id="status"></div>
  <div id="out"></div>
</div>
<script>)HTML";

static const wchar_t* kHistoryPageTail = LR"HTML(
  const send = (c, a='') => window.chrome.webview.postMessage(c + '\x1F' + a);
  const out = document.getElementById('out'), qEl = document.getElementById('q'), statusEl = document.getElementById('status');
  const esc = (s) => String(s || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  const snip = (s) => esc(s).replace(/\u0002/g, '<b>').replace(/\u0003/g, '</b>');
  const host = (u) => { try { return new URL(u).hostname.replace(/^www\./, ''); } catch (e) { return ''; } };
  const day = (t) => new Date(t * 1000).toLocaleDateString(undefined, {weekday:'long', month:'long', day:'numeric', year:'numeric'});
  const clock = (t) => new Date(t * 1000).toLocaleTimeString(undefined, {hour:'2-digit', minute:'2-digit'});
  const short = (t) => new Date(t * 1000).toLocaleDateString(undefined, {month:'short', day:'numeric'});

  function renderStatus() {
    if (INIT.memory) {
      statusEl.innerHTML = '<span>&#10022; Memory is on &middot; ' + INIT.pages.toLocaleString() +
        (INIT.pages === 1 ? ' page' : ' pages') + ' remembered &middot; stored only on this device</span>';
    } else {
      statusEl.innerHTML = '<span>Turn on Memory to also search the <i>text</i> of pages you read &mdash; kept only on this device.</span>' +
        '<button id="memOn">Turn on Memory</button>';
      document.getElementById('memOn').onclick = () => { send('mem-on'); INIT.memory = true; renderStatus(); };
    }
  }
  function histRow(h, showTime) {
    return '<div class="row" data-url="' + esc(h.url) + '"><a href="' + esc(h.url) + '"><div class="t">' + esc(h.title || h.url) +
      '</div><div class="u">' + esc(h.url) + '</div></a>' + (showTime ? '<span class="tm">' + clock(h.time) + '</span>' : '') +
      '<span class="acts"><button data-act="hist-remove" title="Remove from history">Remove</button></span></div>';
  }
  function memRow(m) {
    return '<div class="row" data-url="' + esc(m.url) + '" data-host="' + esc(host(m.url)) + '"><a href="' + esc(m.url) + '">' +
      '<div class="t">' + esc(m.title || m.url) + '</div><div class="u">' + esc(m.site || host(m.url)) + ' &middot; ' + short(m.visited) +
      '</div><div class="s">' + snip(m.snippet) + '</div></a><span class="acts">' +
      '<button data-act="mem-forget" title="Forget this page">Forget</button>' +
      '<button data-act="mem-exclude" title="Never remember this site">Never remember site</button></span></div>';
  }
  function renderRecent() {
    if (!INIT.recent.length) { out.innerHTML = '<p class="empty">No history yet.</p>'; return; }
    let html = '', last = '';
    for (const h of INIT.recent) {
      const d = day(h.time);
      if (d !== last) { html += '<h2>' + esc(d) + '</h2>'; last = d; }
      html += histRow(h, true);
    }
    out.innerHTML = html;
  }
  window.__results = (r) => {
    if (r.q !== qEl.value.trim()) return; // a newer query is on its way
    let html = '';
    if (r.mem.length) html += '<h2>In pages you\'ve read</h2>' + r.mem.map(memRow).join('');
    if (r.hist.length) html += '<h2>Titles &amp; addresses</h2>' + r.hist.map(h => histRow(h, false)).join('');
    out.innerHTML = html || '<p class="empty">Nothing found for &ldquo;' + esc(r.q) + '&rdquo;.</p>';
  };
  let timer = null;
  qEl.addEventListener('input', () => {
    clearTimeout(timer);
    const q = qEl.value.trim();
    if (!q) { renderRecent(); return; }
    timer = setTimeout(() => send('hist-search', q), 120);
  });
  out.addEventListener('click', (e) => {
    const b = e.target.closest('button[data-act]'); if (!b) return;
    e.preventDefault();
    const row = b.closest('.row'), act = b.dataset.act;
    if (act === 'mem-exclude') {
      if (!confirm('Never remember pages from ' + row.dataset.host + '? Pages already remembered from it are forgotten too.')) return;
      send(act, row.dataset.host);
      out.querySelectorAll('.row[data-host="' + row.dataset.host + '"]').forEach(r => r.querySelector('.s') && r.remove());
      return;
    }
    send(act, row.dataset.url);
    row.remove();
  });
  renderStatus();
  if (INIT.q) { qEl.value = INIT.q; send('hist-search', INIT.q); } else renderRecent();
</script></body></html>)HTML";

// ---------------------------------------------------------------------------
// Bookmarks (all of them, by folder, filterable). INIT = {items:[{url,title,folder}]}
// ---------------------------------------------------------------------------
static const wchar_t* kBookmarksPageHead = LR"HTML(<!doctype html>
<html><head><meta charset="utf-8"><title>Bookmarks</title><style>
  :root { color-scheme: light dark; --mut:#80838a; --hover:rgba(0,0,0,.05); --line:rgba(0,0,0,.07); }
  @media (prefers-color-scheme: dark) { :root { --hover:rgba(255,255,255,.07); --line:rgba(255,255,255,.08); } }
  * { box-sizing:border-box; }
  body { margin:0; font:14px/1.45 "Segoe UI",system-ui; background:#fafafa; color:#1a1a1a; }
  @media (prefers-color-scheme: dark) { body { background:#16161a; color:#eee; } }
  .wrap { max-width:860px; margin:0 auto; padding:28px 28px 80px; }
  h1 { font-size:21px; margin:0 0 14px; }
  #q { width:100%; padding:11px 15px; font:14.5px "Segoe UI",system-ui; border-radius:12px; border:1px solid var(--line);
       background:#fff; color:inherit; outline:none; margin-bottom:10px; }
  @media (prefers-color-scheme: dark) { #q { background:#222327; } }
  h2 { font-size:12px; text-transform:uppercase; letter-spacing:.04em; color:var(--mut); margin:22px 4px 6px; font-weight:600; }
  a.row { display:block; padding:8px 10px; border-radius:9px; text-decoration:none; color:inherit; }
  a.row:hover { background:var(--hover); }
  .t { font-weight:600; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .u { font-size:12px; color:var(--mut); overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
  .empty { color:var(--mut); padding:10px 4px; }
</style></head><body><div class="wrap">
  <h1>Bookmarks</h1>
  <input id="q" placeholder="Filter bookmarks" spellcheck="false" autocomplete="off" autofocus>
  <div id="out"></div>
</div>
<script>)HTML";

static const wchar_t* kBookmarksPageTail = LR"HTML(
  const out = document.getElementById('out'), qEl = document.getElementById('q');
  const esc = (s) => String(s || '').replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  function render() {
    const q = qEl.value.trim().toLowerCase();
    const items = INIT.items.filter(b => !q || (b.title + ' ' + b.url + ' ' + b.folder).toLowerCase().includes(q));
    if (!items.length) { out.innerHTML = '<p class="empty">' + (INIT.items.length ? 'No matches.' : 'No bookmarks yet &mdash; press Ctrl+D on any page.') + '</p>'; return; }
    const groups = new Map();
    for (const b of items) { const f = b.folder || 'Minima'; if (!groups.has(f)) groups.set(f, []); groups.get(f).push(b); }
    let html = '';
    for (const [f, list] of groups) {
      html += '<h2>' + esc(f) + ' &middot; ' + list.length + '</h2>';
      for (const b of list) html += '<a class="row" href="' + esc(b.url) + '"><div class="t">' + esc(b.title || b.url) +
                                    '</div><div class="u">' + esc(b.url) + '</div></a>';
    }
    out.innerHTML = html;
  }
  qEl.addEventListener('input', render);
  render();
</script></body></html>)HTML";
