// Shared client: SSE connection, formatting, header status chips.
const App = { cal: {}, local: false, base: null, link: [], cfg: null, race: null, devices: {}, gate: {}, status: {}, sim: false, handlers: [], offset: 0 };
const PCOL = n => `var(--p${n})`;
const RACEBAND = [5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917];
const $ = (s, el = document) => el.querySelector(s);
const fmt = ms => {
  if (ms == null) return '–';
  const s = ms / 1000, m = Math.floor(s / 60);
  return (m ? m + ':' + (s % 60).toFixed(2).padStart(5, '0') : s.toFixed(2));
};
// Level codes are 0.25 dB steps. SNR mode (the gate's default): dB above the channel's own noise floor;
// otherwise absolute dBFS. The gate says which in its GATE reply / status packets.
function isSnr() {
  const g = App.gate && App.gate.gate; if (g && g.metric) return g.metric === 'snr';
  const s = Object.values(App.status || {})[0]; return !!(s && s.metric === 'snr');
}
const dbfs = code => isSnr() ? code / 4 : code / 4 - 63.75;
// Thresholds are shown and typed in the graph's unit and stored as codes.
const fromDb = v => Math.round(isSnr() ? v * 4 : (v + 63.75) * 4);
const dbShow = code => +dbfs(code).toFixed(2);
const levelShort = () => isSnr() ? 'dB' : 'dBFS';
const levelUnit = () => isSnr() ? 'dB over noise' : 'dBFS';
const esc = s => String(s).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v === null ? d : JSON.parse(v); } catch { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch {} },
};
async function post(path, body = {}) {
  const r = await fetch(path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
  return r.json().catch(() => ({}));
}
function onMsg(fn) { App.handlers.push(fn); }

function connect() {
  const es = new EventSource('/events');
  es.onopen = () => setConn(true);
  es.onerror = () => setConn(false);
  es.onmessage = e => {
    const m = JSON.parse(e.data);
    switch (m.type) {
      case 'snapshot':
        App.cfg = m.cfg; App.race = m.race; App.devices = m.devices; App.gate = m.gate || {};
        App.status = m.status || {}; App.base = m.base; App.link = m.link || []; App.sim = m.sim; App.local = !!m.local; App.cal = m.cal || {}; App.offset = m.now - Date.now() / 1000; break;
      case 'cfg': App.cfg = m.cfg; break;
      case 'race': App.race = m.race; App.offset = m.race.now - Date.now() / 1000; break;
      case 'devices': App.devices = m.d; break;
      case 'gate': App.gate.gate = m.d; break;
      case 'lora': App.gate.lora = m.d; break;
      case 'status': App.status[m.d.gate] = m.d; break;
      case 'base': App.base = m.d; break;
      case 'cal': App.cal[m.d.kind] = m.d; break;
      case 'link': App.link.push(m.d); if (App.link.length > 300) App.link.shift(); break;
    }
    renderChips();
    App.handlers.forEach(h => h(m));
  };
}
let connected = false;
function setConn(v) { connected = v; renderChips(); }
function serverNow() { return Date.now() / 1000 + App.offset; }
// Server timestamps are epoch seconds on the Mac server and seconds since boot on the base; this maps either to a browser time.
const wall = t => Date.now() - (serverNow() - t) * 1000;

function renderChips() {
  const el = $('#chips'); if (!el) return;
  const d = App.devices || {};
  const chip = (label, state, title) => `<span class="chip" title="${esc(title || '')}"><span class="dot ${state}"></span>${label}</span>`;
  const b = App.base || {}, heard = b.last_rx && serverNow() - b.last_rx < 40;
  el.innerHTML =
    chip(App.local ? 'Base' : 'Server', connected ? 'ok' : 'bad', App.local ? 'this device (WiFi hotspot)' : '') +
    (App.sim ? chip('Simulator', 'warn', 'No hardware: synthetic gate') : '') +
    (App.local ? chip('Gate (LoRa)', heard ? 'ok' : 'warn', heard ? 'heard a packet in the last 40 s' : 'no packet from the gate lately')
      : chip('Gate USB', d.gate ? 'ok' : 'warn', d.gate || 'not connected') + chip('Base', d.base ? 'ok' : 'warn', d.base || 'not connected')) +
    (d.unknown && d.unknown.length ? chip('Unrecognised port', 'warn', d.unknown.join(', ') + ' (not running our firmware)') : '');
}

// Spoken call-outs (browser speech synthesis). Needs one click to unlock.
let voiceOn = store.get('voice', false);
function setVoice(v) { voiceOn = v; store.set('voice', v); }
onMsg(m => {
  if (m.type !== 'callout' || !voiceOn || !('speechSynthesis' in window)) return;
  const u = new SpeechSynthesisUtterance(m.text); u.rate = 1.1; speechSynthesis.speak(u);
});

function header(active) {
  document.body.insertAdjacentHTML('afterbegin', `<div class="wrap" style="padding-bottom:0"><header>
    <img class="logo" src="/breaking_carbon.jpg" alt="Breaking Carbon"><h1>tinytimer</h1>
    <nav><a href="/" class="${active === 'race' ? 'on' : ''}">Race</a><a href="/gate" class="${active === 'gate' ? 'on' : ''}">Gate debug</a><a href="/base" class="${active === 'base' ? 'on' : ''}">Base station</a></nav>
    <div class="chips" id="chips"></div></header></div>`);
}
