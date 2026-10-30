// reader.h — the reading pipeline: one page-extraction script whose result feeds reader
// view, the AI's page context and Memory, plus the reader-view overlay itself. Pure JS
// strings (part of the "core" per PORTING.md) — every shell runs them in its webview.
#pragma once

// ---------------------------------------------------------------------------
// kExtractJs — a JS *expression* evaluating to the page's main content:
//   {title, byline, site, lang, url, words, readerable, hasPassword, blocks:[{k,t,l,s}], text}
// k is 'h' (heading, l = level), 'p', 'li', 'pre', 'q' (quote) or 'img' (s = src).
// Scoring is the classic text-density approach (Arc90, which Firefox's Readability grew
// from): paragraphs credit their ancestors, link-heavy containers are penalized, and the
// best container (plus strong siblings) is kept. Read-only: the page is never modified.
// ---------------------------------------------------------------------------
static const wchar_t* kExtractJs = LR"JS((() => {
  const doc = document, body = doc.body;
  const out = {title:'', byline:'', site:'', lang:'', url:location.href, words:0, readerable:false,
               hasPassword:false, blocks:[], text:''};
  if (!body) return out;
  const meta = (sel) => { const m = doc.querySelector(sel); return m && m.content ? m.content.trim() : ''; };
  const norm = (s) => (s || '').replace(/\s+/g, ' ').trim();
  out.lang = (doc.documentElement.lang || '').slice(0, 12);
  out.site = meta('meta[property="og:site_name"]') || location.hostname.replace(/^www\./, '');
  out.byline = meta('meta[name="author"]') || meta('meta[property="article:author"]');
  if (/^https?:/.test(out.byline)) out.byline = '';
  out.hasPassword = !!doc.querySelector('input[type=password]');

  const SKIP_TAGS = /^(SCRIPT|STYLE|NOSCRIPT|SVG|CANVAS|IFRAME|FORM|BUTTON|SELECT|TEXTAREA|NAV|ASIDE|FOOTER|HEADER|DIALOG|TEMPLATE|OBJECT|EMBED)$/;
  const SKIP_ROLES = /^(navigation|banner|contentinfo|complementary|dialog|menu|menubar|search|alert)$/;
  const BAD = /(^|[\s_-])(nav|navbar|menu|footer|masthead|sidebar|comment|comments|share|sharing|social|related|recommend|promo|advert|ads|ad-slot|sponsor|cookie|consent|newsletter|subscribe|signup|popup|modal|breadcrumbs?|widget|outbrain|taboola|byline-tools|toolbar|skip|references|reflist|footnotes|citations?|navbox|catlinks|mw-editsection)([\s_-]|$)/i;
  const GOOD = /(^|[\s_-])(article|content|entry|post|story|main|body|text|prose|blog)([\s_-]|$)/i;
  const cls = (el) => ((typeof el.className === 'string' ? el.className : '') + ' ' + (el.id || ''));
  const skip = (el) => SKIP_TAGS.test(el.tagName) || SKIP_ROLES.test(el.getAttribute('role') || '') ||
                       el.hidden || el.getAttribute('aria-hidden') === 'true' || BAD.test(cls(el));
  function excluded(el) {
    for (let e = el; e && e !== body; e = e.parentElement) if (skip(e)) return true;
    return false;
  }
  const linkDensity = (el) => {
    const total = norm(el.textContent).length || 1;
    let links = 0;
    for (const a of el.querySelectorAll('a')) links += norm(a.textContent).length;
    return links / total;
  };

  // 1. Score: every substantial paragraph-like element credits its ancestors.
  const scores = new Map();
  const credit = (el, v) => { if (el && el !== doc.documentElement) scores.set(el, (scores.get(el) || 0) + v); };
  let scanned = 0;
  for (const p of body.querySelectorAll('p, pre, blockquote, td, div')) { // not li: comma-rich citation lists would win
    if (++scanned > 8000) break; // bound the cost on enormous pages
    if (p.tagName === 'DIV' && p.firstElementChild && p.querySelector('p, div, pre, table, ul, ol, blockquote')) continue; // leaf divs only
    const t = norm(p.textContent);
    if (t.length < 25 || excluded(p)) continue;
    const v = 1 + (t.split(/[,،、，]/).length - 1) + Math.min(3, Math.floor(t.length / 100));
    credit(p.parentElement, v);
    credit(p.parentElement && p.parentElement.parentElement, v / 2);
    credit(p.parentElement && p.parentElement.parentElement && p.parentElement.parentElement.parentElement, v / 3);
  }
  // Link density is costly (a full text walk), so only rank the strongest raw candidates.
  const top = [...scores].sort((x, y) => y[1] - x[1]).slice(0, 15);
  let best = null, bestScore = 0;
  for (const [el, s0] of top) {
    let s = s0 * (1 - linkDensity(el));
    if (/^(ARTICLE|MAIN)$/.test(el.tagName) || el.getAttribute('role') === 'main') s *= 1.25;
    if (GOOD.test(cls(el))) s *= 1.15;
    if (s > bestScore) { best = el; bestScore = s; }
  }
  if (!best) best = doc.querySelector('article, main, [role=main]') || body;

  // 2. Keep the best container plus siblings that look like more of the same article.
  const parts = [best];
  if (best.parentElement && best !== body) {
    for (const sib of best.parentElement.children) {
      if (sib === best || skip(sib)) continue;
      const raw = scores.get(sib) || 0;
      if (raw > bestScore * 0.2) { if (raw * (1 - linkDensity(sib)) > bestScore * 0.2) parts.push(sib); continue; }
      if (sib.tagName === 'P') { const t = norm(sib.textContent); if (t.length > 80 && linkDensity(sib) < 0.25) parts.push(sib); }
    }
    const pos = (n) => Array.prototype.indexOf.call(best.parentElement.children, n);
    parts.sort((a, b) => pos(a) - pos(b));
  }

  // 3. Flatten to clean blocks, in document order.
  const blocks = [];
  const seen = new Set();
  const push = (b) => { const key = b.k + '|' + (b.t || b.s); if (!seen.has(key)) { seen.add(key); blocks.push(b); } };
  function walk(el) {
    if (el.nodeType !== 1 || skip(el)) return;
    const tag = el.tagName;
    if (/^H[1-6]$/.test(tag)) { const t = norm(el.textContent); if (t) push({k:'h', l:+tag[1], t}); return; }
    if (tag === 'PRE') { const t = el.textContent.replace(/\s+$/, ''); if (t.trim()) push({k:'pre', t: t.slice(0, 20000)}); return; }
    if (tag === 'BLOCKQUOTE') { const t = norm(el.textContent); if (t.length > 10) push({k:'q', t}); return; }
    if (tag === 'IMG') {
      const src = el.currentSrc || el.src;
      if (/^https?:/.test(src) && (el.naturalWidth || el.width || 0) >= 160) push({k:'img', s: src, t: norm(el.alt)});
      return;
    }
    if (tag === 'LI') {
      const t = norm(el.textContent);
      if (t.length > 2 && linkDensity(el) < 0.6) push({k:'li', t});
      return;
    }
    if (tag === 'P' || (tag === 'DIV' && !el.querySelector('p, div, pre, ul, ol, table, blockquote, h1, h2, h3, h4, img, figure'))) {
      const t = norm(el.textContent);
      if (t.length >= 20 && (t.length > 140 || linkDensity(el) < 0.5)) push({k:'p', t});
      return;
    }
    if (tag === 'TABLE') {
      const rows = [...el.rows].slice(0, 60).map(r => [...r.cells].map(c => norm(c.textContent)).join(' | ')).filter(Boolean);
      if (rows.length) push({k:'pre', t: rows.join('\n')});
      return;
    }
    for (const c of el.children) walk(c);
  }
  for (const p of parts) walk(p);

  const h1 = doc.querySelector('h1');
  out.title = meta('meta[property="og:title"]') || (h1 && norm(h1.textContent)) || norm(doc.title);
  if (blocks.length && blocks[0].k === 'h' && blocks[0].t === out.title) blocks.shift(); // shown as the title
  const text = blocks.filter(b => b.k !== 'img').map(b => b.t).join('\n');
  out.blocks = blocks.slice(0, 1500);
  out.text = text.slice(0, 100000);
  out.words = (text.match(/\S+/g) || []).length;
  out.readerable = out.words >= 200 && blocks.filter(b => b.k === 'p').length >= 3;
  return out;
})())JS";

// ---------------------------------------------------------------------------
// kReaderJs — a JS *function expression* (article, prefs) => 'opened' | 'closed'.
// Renders the extracted article as a full-window overlay inside the page (closed shadow
// root, so page CSS can't leak in), keeping the real page underneath — the URL, history
// and session stay untouched. Calling it again closes it (toggle). Read aloud uses the
// OS's speech voices via speechSynthesis, so it stays on-device too.
// Preference changes are posted to the shell as "reader-prefs\x1F{json}".
// ---------------------------------------------------------------------------
static const wchar_t* kReaderJs = LR"JS((function (a, prefs) {
  if (window.__minimaReaderClose) { window.__minimaReaderClose(); return 'closed'; }
  prefs = Object.assign({size: 19, font: 'serif', theme: 'auto'}, prefs || {});
  const host = document.createElement('div');
  host.style.cssText = 'position:fixed;inset:0;z-index:2147483647;';
  const root = host.attachShadow({mode: 'closed'});
  const esc = (s) => String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;').replace(/"/g, '&quot;');
  const mins = Math.max(1, Math.round(a.words / 230));
  let html = '';
  a.blocks.forEach((b, i) => {
    const id = ' data-i="' + i + '"';
    if (b.k === 'h') { const l = Math.min(4, Math.max(2, b.l)); html += '<h' + l + id + '>' + esc(b.t) + '</h' + l + '>'; }
    else if (b.k === 'p') html += '<p' + id + '>' + esc(b.t) + '</p>';
    else if (b.k === 'li') html += '<p class="li"' + id + '>' + esc(b.t) + '</p>';
    else if (b.k === 'q') html += '<blockquote' + id + '>' + esc(b.t) + '</blockquote>';
    else if (b.k === 'pre') html += '<pre' + id + '>' + esc(b.t) + '</pre>';
    else if (b.k === 'img') html += '<figure><img loading="lazy" src="' + esc(b.s) + '" alt="' + esc(b.t || '') + '"></figure>';
  });
  root.innerHTML = `<style>
    :host { all: initial; }
    .wrap { position:fixed; inset:0; overflow-y:auto; background:var(--bg); color:var(--fg);
            font-family:var(--font); -webkit-font-smoothing:antialiased; }
    .light { --bg:#fbfbfa; --fg:#1d1d1f; --mut:#6e6e73; --bar:rgba(251,251,250,.92); --line:rgba(0,0,0,.08); --hl:rgba(59,130,246,.14); }
    .sepia { --bg:#f6efe2; --fg:#4b3b2a; --mut:#8a7660; --bar:rgba(246,239,226,.92); --line:rgba(75,59,42,.12); --hl:rgba(180,120,40,.18); }
    .dark  { --bg:#18181b; --fg:#e4e4e7; --mut:#9a9aa2; --bar:rgba(24,24,27,.92); --line:rgba(255,255,255,.09); --hl:rgba(96,165,250,.22); }
    .serif { --font:Charter,"Iowan Old Style",Georgia,Cambria,"Times New Roman",serif; }
    .sans  { --font:"Segoe UI Variable Text","Segoe UI",system-ui,-apple-system,sans-serif; }
    .bar { position:sticky; top:0; display:flex; align-items:center; gap:4px; padding:8px 14px;
           background:var(--bar); backdrop-filter:blur(8px); border-bottom:1px solid var(--line);
           font:13px "Segoe UI",system-ui,sans-serif; z-index:1; }
    .bar .sp { flex:1; }
    button { font:inherit; border:none; background:none; color:var(--mut); padding:6px 10px; border-radius:7px; cursor:pointer; }
    button:hover { background:var(--line); color:var(--fg); }
    button.on { color:var(--fg); background:var(--line); }
    article { max-width:var(--w,680px); margin:0 auto; padding:40px 24px 120px; font-size:var(--size); line-height:1.65; }
    .site { color:var(--mut); font:13px "Segoe UI",system-ui,sans-serif; margin-bottom:12px; }
    h1 { font-size:1.9em; line-height:1.2; margin:0 0 10px; font-weight:700; }
    .meta { color:var(--mut); font:13px "Segoe UI",system-ui,sans-serif; margin-bottom:34px; }
    h2 { font-size:1.35em; margin:1.6em 0 .5em; line-height:1.3; } h3, h4 { font-size:1.12em; margin:1.4em 0 .4em; }
    p { margin:0 0 1em; } p.li { padding-left:1.1em; position:relative; } p.li:before { content:'•'; position:absolute; left:0; color:var(--mut); }
    blockquote { margin:1.2em 0; padding:.2em 0 .2em 1em; border-left:3px solid var(--line); color:var(--mut); font-style:italic; }
    pre { font:13.5px/1.5 "Cascadia Code",Consolas,monospace; background:var(--line); padding:12px 14px; border-radius:8px; overflow-x:auto; white-space:pre-wrap; }
    figure { margin:1.4em 0; } img { max-width:100%; height:auto; border-radius:6px; display:block; margin:0 auto; }
    .reading { background:var(--hl); border-radius:4px; box-shadow:0 0 0 4px var(--hl); }
  </style>
  <div class="wrap">
    <div class="bar">
      <button data-a="close" title="Exit reader view (Esc)">&#x2190; Back to page</button>
      <span class="sp"></span>
      <button data-a="listen" title="Read aloud — uses this device's voices">&#9654; Listen</button>
      <button data-a="smaller" title="Smaller text">A&#8722;</button>
      <button data-a="bigger" title="Larger text">A+</button>
      <button data-a="font" title="Serif / sans-serif">Aa</button>
      <button data-a="theme" title="Theme">&#9680;</button>
    </div>
    <article>
      <div class="site">${esc(a.site || '')}</div>
      <h1>${esc(a.title || '')}</h1>
      <div class="meta">${a.byline ? esc(a.byline) + ' · ' : ''}${mins} min read</div>
      ${html}
    </article>
  </div>`;
  const wrap = root.querySelector('.wrap'), art = root.querySelector('article');
  const dark = matchMedia('(prefers-color-scheme: dark)').matches;
  function apply() {
    const theme = prefs.theme === 'auto' ? (dark ? 'dark' : 'light') : prefs.theme;
    wrap.className = 'wrap ' + theme + ' ' + (prefs.font === 'sans' ? 'sans' : 'serif');
    art.style.setProperty('--size', prefs.size + 'px');
  }
  function save() {
    try { window.chrome.webview.postMessage('reader-prefs\x1F' + JSON.stringify(prefs)); } catch (e) {}
  }
  apply();

  // ---- read aloud ----
  const speakable = [...art.querySelectorAll('[data-i]')];
  let speaking = false, idx = 0;
  const listenBtn = root.querySelector('[data-a=listen]');
  function mark(n) {
    speakable.forEach(e => e.classList.remove('reading'));
    if (n >= 0 && speakable[n]) { speakable[n].classList.add('reading'); speakable[n].scrollIntoView({block:'center', behavior:'smooth'}); }
  }
  function stopSpeaking() { speaking = false; speechSynthesis.cancel(); mark(-1); listenBtn.innerHTML = '&#9654; Listen'; listenBtn.classList.remove('on'); }
  function speakNext() {
    if (!speaking || idx >= speakable.length) { stopSpeaking(); return; }
    const el = speakable[idx];
    const u = new SpeechSynthesisUtterance(el.textContent);
    if (a.lang) u.lang = a.lang;
    u.onend = () => { if (speaking) { idx++; speakNext(); } };
    u.onerror = () => { if (speaking) { idx++; speakNext(); } };
    mark(idx);
    speechSynthesis.speak(u);
  }
  listenBtn.onclick = () => {
    if (speaking) { stopSpeaking(); return; }
    // Start from the first paragraph on screen.
    const top = wrap.scrollTop + 60;
    idx = Math.max(0, speakable.findIndex(e => e.offsetTop >= top));
    speaking = true; listenBtn.innerHTML = '&#9632; Stop'; listenBtn.classList.add('on');
    speakNext();
  };

  // ---- controls ----
  const prevOverflow = document.documentElement.style.overflow;
  function close() {
    stopSpeaking();
    host.remove();
    document.documentElement.style.overflow = prevOverflow;
    window.removeEventListener('keydown', onKey, true);
    delete window.__minimaReaderClose;
  }
  function onKey(e) { if (e.key === 'Escape') { e.stopPropagation(); e.preventDefault(); close(); } }
  root.querySelector('.bar').addEventListener('click', (e) => {
    const b = e.target.closest('button'); if (!b) return;
    const act = b.dataset.a;
    if (act === 'close') close();
    else if (act === 'smaller') { prefs.size = Math.max(14, prefs.size - 1); apply(); save(); }
    else if (act === 'bigger') { prefs.size = Math.min(30, prefs.size + 1); apply(); save(); }
    else if (act === 'font') { prefs.font = prefs.font === 'sans' ? 'serif' : 'sans'; apply(); save(); }
    else if (act === 'theme') {
      const order = ['auto', 'light', 'sepia', 'dark'];
      prefs.theme = order[(order.indexOf(prefs.theme) + 1) % order.length]; apply(); save();
    }
  });
  window.addEventListener('keydown', onKey, true);
  window.__minimaReaderClose = close;
  document.documentElement.style.overflow = 'hidden';
  (document.body || document.documentElement).appendChild(host);
  wrap.tabIndex = -1; wrap.focus();
  return 'opened';
}))JS";
