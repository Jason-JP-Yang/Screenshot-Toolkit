// A frame-exact replica of Screenshot Toolkit's overlay, driven by the springs, sizes, and colors in
// include/sstk/config and the layout rules in src/overlay. Everything runs on a fixed simulated clock, so any frame
// can be rendered on demand and every run produces the same pictures.
const TAU = Math.PI * 2;
const W = 1280, H = 800, BOTTOM = 752;
const K = {
  flashFall: [0.62, 1], flight: [0.5, 0.86], reflow: [0.42, 0.86], morph: [0.36, 0.86], exit: [0.38, 1],
  fade: [0.16, 1], quick: [0.09, 1], capEnter: [0.42, 0.78], capLeave: [0.26, 1], pillLead: [0.24, 0.8], pillTrail: [0.34, 0.84],
  capResize: [0.42, 0.74], slide: [0.34, 0.82], pillMorph: [0.4, 0.8], resultPop: [0.4, 0.62], follow: [0.26, 1],
  dragLift: [0.36, 0.72], catchUp: [0.16, 1], tilt: [0.46, 0.5], dropVanish: [0.3, 1],
  badgeIn: [0.32, 0.54], badgeOut: [0.15, 1], binPull: [0.42, 0.88], binSwallow: [0.3, 1], binMorph: [0.34, 0.84],
  binLid: [0.26, 0.58], binLidAim: [0.3, 0.62], binLidSlam: [0.18, 0.62], binCan: [0.3, 0.52],
  hudIn: [0.34, 0.8], hudOut: [0.22, 1], keyDown: [0.07, 1], keyUp: [0.24, 0.62], chat: [0.42, 0.9],
};

// ---------------------------------------------------------------- clock and springs
const clock = { now: 0, events: [], springs: [] };

function springStep(x, v, target, spec, dt) {
  const w0 = TAU / spec[0];
  const z = spec[1];
  const a = x - target;
  if (z < 1) {
    const wd = w0 * Math.sqrt(1 - z * z);
    const e = Math.exp(-z * w0 * dt);
    const c = Math.cos(wd * dt), s = Math.sin(wd * dt);
    const B = (v + z * w0 * a) / wd;
    return [target + e * (a * c + B * s), e * (-z * w0 * (a * c + B * s) + (-a * wd * s + B * wd * c))];
  }
  const e = Math.exp(-w0 * dt);
  const B = v + w0 * a;
  return [target + (a + B * dt) * e, (B - w0 * (a + B * dt)) * e];
}

class Spring {
  constructor(value = 0) {
    this.x = value; this.v = 0; this.target = value; this.spec = K.fade; this.queue = [];
    clock.springs.push(this);
  }
  snap(value) { this.x = value; this.v = 0; this.target = value; this.queue = []; return this; }
  to(target, spec, delay = 0) {
    if (delay > 0) {
      this.queue.push({ at: clock.now + delay, target, spec });
      this.queue.sort((a, b) => a.at - b.at);
    } else { this.queue = []; this.target = target; this.spec = spec; }
    return this;
  }
  kick(velocity) { this.v += velocity; }
  step(dt) {
    while (this.queue.length && this.queue[0].at <= clock.now) {
      const q = this.queue.shift();
      this.target = q.target; this.spec = q.spec;
    }
    if (this.x === this.target && this.v === 0) return;
    [this.x, this.v] = springStep(this.x, this.v, this.target, this.spec, dt);
    if (Math.abs(this.x - this.target) < 1e-4 && Math.abs(this.v) < 1e-3) { this.x = this.target; this.v = 0; }
  }
  get value() { return this.x; }
}

function later(delay, fn) {
  clock.events.push({ t: clock.now + delay, fn });
  clock.events.sort((a, b) => a.t - b.t);
}

const clamp = (v, a, b) => Math.min(Math.max(v, a), b);
const ease = (u) => (u < 0.5 ? 4 * u * u * u : 1 - Math.pow(-2 * u + 2, 3) / 2);
const smooth = (u) => u * u * (3 - 2 * u);
const svgns = 'http://www.w3.org/2000/svg';
function el(tag, cls, parent) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (parent) parent.appendChild(e);
  return e;
}
function svgEl(tag, attributes, parent) {
  const e = document.createElementNS(svgns, tag);
  for (const [k, v] of Object.entries(attributes)) e.setAttribute(k, v);
  if (parent) parent.appendChild(e);
  return e;
}
const hex = (c, a = 1) => `rgba(${(c >> 16) & 255},${(c >> 8) & 255},${c & 255},${a})`;
const toward = (from, to, share) => {
  let out = 0;
  for (let s = 0; s < 24; s += 8) {
    const a = (from >> s) & 255, b = (to >> s) & 255;
    out |= Math.round(a + (b - a) * share) << s;
  }
  return out;
};

// ---------------------------------------------------------------- text and glyphs
const LABEL_FONT = '"Segoe UI Variable Display", "Segoe UI"';
const measureCtx = document.createElement('canvas').getContext('2d');
function measure(text, size = 13, weight = 600) {
  measureCtx.font = `${weight} ${size}px ${LABEL_FONT}`;
  const m = measureCtx.measureText(text);
  const cap = measureCtx.measureText('H').actualBoundingBoxAscent;
  return { left: -m.actualBoundingBoxLeft, width: m.actualBoundingBoxRight + m.actualBoundingBoxLeft, cap };
}

function textEl(parent, text, x, baseline, color, alpha = 1, size = 13) {
  const t = svgEl('text', { x, y: baseline, fill: hex(color, alpha), 'font-family': LABEL_FONT, 'font-weight': 600, 'font-size': size }, parent);
  t.textContent = text;
  return t;
}

// RenderGlyph: a mini screen with the region filled in, from the shot's real geometry.
function glyph(parent, spec, color, alpha) {
  const g = svgEl('g', {}, parent);
  let sw = 20, sh = sw / spec.aspect;
  if (sh > 15) { sh = 15; sw = sh * spec.aspect; }
  const ox = Math.round((38 - sw) / 2), oy = Math.round((28 - sh) / 2);
  sw = Math.round(sw); sh = Math.round(sh);
  const gutter = 2.1, ix = ox + gutter, iy = oy + gutter, iw = sw - 2 * gutter, ih = sh - 2 * gutter;
  let fl = Math.round(ix + spec.l * iw), fr = Math.round(ix + spec.r * iw), ft = Math.round(iy + spec.t * ih), fb = Math.round(iy + spec.b * ih);
  if (spec.gap === 'bottom') fb = Math.round(iy + ih) - 3;
  const widen = (lo, hi, least, floor, ceil) => {
    least = Math.min(least, ceil - floor);
    if (hi - lo >= least) return [lo, hi];
    lo = clamp(Math.round((lo + hi - least) / 2), floor, ceil - least);
    return [lo, lo + least];
  };
  if (spec.scroll) {
    [fl, fr] = widen(fl, fr, 10, Math.round(ix), Math.round(ix + iw));
    [ft, fb] = widen(ft, fb, 7, Math.round(iy), Math.round(iy + ih));
  }
  if (fr - fl < 2.2) fr = fl + 2.2;
  if (fb - ft < 2.2) fb = ft + 2.2;
  const fill = hex(color, alpha);
  svgEl('rect', { x: ox + 0.55, y: oy + 0.55, width: sw - 1.1, height: sh - 1.1, rx: 2.25, fill: 'none', stroke: fill, 'stroke-width': 1.1 }, g);
  if (spec.scroll) {
    const id = `m${Math.random().toString(36).slice(2)}`;
    const mask = svgEl('mask', { id }, g);
    svgEl('rect', { x: 0, y: 0, width: 38, height: 28, fill: '#fff' }, mask);
    const cx = (fl + fr) / 2, cy = (ft + fb) / 2, u = Math.min(1, (fr - fl) / 10, (fb - ft) / 6.6);
    svgEl('path', { d: `M${cx - 3 * u} ${cy - 1.6 * u} L${cx} ${cy + 1.6 * u} L${cx + 3 * u} ${cy - 1.6 * u}`, fill: 'none', stroke: '#000',
      'stroke-width': 1.7 * u, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' }, mask);
    svgEl('rect', { x: fl, y: ft, width: fr - fl, height: fb - ft, rx: 1, fill, mask: `url(#${id})` }, g);
  } else {
    svgEl('rect', { x: fl, y: ft, width: fr - fl, height: fb - ft, rx: 1, fill }, g);
  }
  return { g, inkLeft: ox, inkRight: ox + sw, inkTop: oy, inkBottom: oy + sh };
}

// RenderKeycap: 15 high and 17 wide, with the Windows panes or the Enter arrow.
function keycap(parent, key, x, y, color, alpha = 1) {
  const g = svgEl('g', { transform: `translate(${x} ${y})` }, parent);
  const c = hex(color, alpha);
  svgEl('rect', { x: 0.5, y: 0.5, width: 16, height: 14, rx: 2.9, fill: 'none', stroke: c, 'stroke-width': 1 }, g);
  const cx = 8.5, cy = 7.5;
  if (key === 'win') {
    for (const sx of [-1.88, 1.88]) for (const sy of [-1.88, 1.88]) {
      svgEl('rect', { x: cx + sx - 1.45, y: cy + sy - 1.45, width: 2.9, height: 2.9, rx: 0.35, fill: c }, g);
    }
  } else {
    svgEl('path', { d: `M${cx + 2.8} ${cy - 2.6}V${cy + 1.1}H${cx - 2.9}M${cx - 0.9} ${cy - 0.9}L${cx - 2.9} ${cy + 1.1}L${cx - 0.9} ${cy + 3.1}`,
      fill: 'none', stroke: c, 'stroke-width': 1.04, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' }, g);
  }
  return g;
}

function statusIcon(parent, kind, x, y, color, alpha) {
  const g = svgEl('g', {}, parent);
  const inner = svgEl('g', { transform: `translate(${x + 7} ${y + 7})` }, g);
  const c = hex(color, alpha);
  const line = (d, w) => svgEl('path', { d, fill: 'none', stroke: c, 'stroke-width': w, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' }, inner);
  if (kind === 'check') line('M-4.3 .2L-1.4 3.1L4.3 -3.2', 1.7);
  else if (kind === 'cross') line('M-3.5 -3.5L3.5 3.5M-3.5 3.5L3.5 -3.5', 1.7);
  else { line('M0 -4.6V1', 1.84); svgEl('circle', { cx: 0, cy: 4, r: 1.1, fill: c }, inner); }
  return g;
}

// ---------------------------------------------------------------- regions
const REGION_LABEL = { full: 'Fullscreen', notb: 'No Taskbar', app: 'Window', zone: 'Window', scroll: 'Scrolling Screenshot', long: 'Scrolling Screenshot' };
function glyphSpec(region) {
  const [l, t, r, b] = region.rect;
  const scroll = region.kind === 'scroll' || region.kind === 'long';
  const notb = l === 0 && t === 0 && r === W && b === BOTTOM;
  return { aspect: W / H, l: l / W, t: t / H, r: r / W, b: b / H, gap: notb && !scroll ? 'bottom' : null, scroll };
}
function thumbSize(rect) {
  const w = rect[2] - rect[0], h = rect[3] - rect[1];
  const fit = Math.min(1, 340 / w, 160 / h);
  return [Math.max(Math.round(w * fit), 1), Math.max(Math.round(h * fit), 1)];
}
const rectForSize = ([w, h]) => ({ l: 16, t: 736 - h, r: 16 + w, b: 736 });
const LONG_MAX = Math.max(Math.min(460, BOTTOM * 0.5), 160);

// ---------------------------------------------------------------- the stage
class Stage {
  constructor(root) {
    root.innerHTML = '';
    this.el = el('div', 'stage', root);
    this.desk = buildDesktop();
    this.el.appendChild(this.desk);
    this.fx = el('div', 'fx', this.el);
    this.shadowLayer = el('div', 'shadows', this.fx);
    this.cardLayer = el('div', '', this.fx);
    this.capsule = new Capsule(this);
    this.bin = new Bin(this);
    this.spriteLayer = el('div', '', this.fx);
    this.flashEl = el('div', 'flash', this.fx);
    this.cursorEl = el('div', 'cursor', this.fx);
    this.hud = new Hud(this);
    this.cards = [];
    this.exiting = [];
    this.flashAt = -10;
    this.scrollY = 0;
    this.chat = new Spring(0);
    this.incoming = new Spring(0);
    this.drop = new Spring(0);
    this.typing = null;
    this.sprite = null;
    this.cursorKind = 'arrow';
    // Held now, because the chat later shows a copy of this desktop with the same parts inside it.
    this.parts = {
      newwrap: this.desk.querySelector('.newwrap'), incoming: this.desk.querySelector('.incoming'),
      typing: this.desk.querySelector('.typing'), dropzone: this.desk.querySelector('.dropzone'), shot: this.desk.querySelector('.attach .shot'),
    };
    setScroll(this.desk, 0);
  }

  snapshot() {
    const copy = this.desk.cloneNode(true);
    copy.style.cssText = 'position:absolute;left:0;top:0;width:1280px;height:800px';
    return copy;
  }

  capture(regions, selected = 0) {
    this.flashAt = clock.now;
    this.render();
    const card = new Card(this, this.snapshot(), regions, selected);
    this.cards.push(card);
    this.layout(K.reflow, card);
    this.capsule.bind(regions, selected);
    return card;
  }

  front() { return this.cards[this.cards.length - 1]; }

  layout(spec = K.reflow, flying = null) {
    const n = this.cards.length;
    if (!n) return;
    const fr = rectForSize(this.front().displaySize());
    this.cards.forEach((card, i) => {
      const depth = n - 1 - i;
      let rect = fr;
      if (depth > 0) {
        const own = rectForSize(card.displaySize());
        const level = Math.min(depth, 3);
        const keep = 1 - 0.055 * level;
        const w = (own.r - own.l) * keep, h = (own.b - own.t) * keep;
        const l = own.l + ((own.r - own.l) - w) / 2;
        const t = Math.min(fr.t - 9 * level, fr.b - h - 9 * level);
        rect = { l, t, r: l + w, b: t + h };
      }
      if (card === flying) card.moveTo(rect, depth, K.flight, 0.02, true);
      else card.moveTo(rect, depth, spec, 0, false);
    });
    let tallest = 160;
    for (let i = Math.max(n - 3, 0); i < n; i++) tallest = Math.max(tallest, this.cards[i].longHeight());
    this.capsule.moveTo(fr.l, Math.round(736 - tallest - 9 * Math.min(n - 1, 2) - 10 - 34));
  }

  select(index) {
    this.front().sel = index;
    this.capsule.select(index);
    this.layout(K.morph);
  }

  removeCard(card) { this.cards = this.cards.filter((c) => c !== card); }

  render() {
    const t = clock.now - this.flashAt;
    let flash = 0;
    if (t >= 0 && t < 0.06) flash = 0.26 * smooth(t / 0.06);
    else if (t >= 0.06) flash = Math.max(springStep(0.26, 0, 0, K.flashFall, t - 0.06)[0], 0);
    this.flashEl.style.opacity = flash;
    for (const c of [...this.cards, ...this.exiting]) c.render();
    this.capsule.render();
    this.bin.render();
    if (this.sprite) this.sprite.render();
    this.hud.render();
    const p = this.pointer;
    if (this.cursorEl.dataset.kind !== this.cursorKind) {
      this.cursorEl.dataset.kind = this.cursorKind;
      this.cursorEl.innerHTML = this.cursorKind === 'text'
        ? '<svg width="10" height="20" viewBox="0 0 10 20"><path d="M2 1.5h2.2L5 2.3l.8-.8H8M5 2.3v15.4M2 18.5h2.2l.8-.8.8.8H8" fill="none" stroke="#fff" stroke-width="3" stroke-linejoin="round"/><path d="M2 1.5h2.2L5 2.3l.8-.8H8M5 2.3v15.4M2 18.5h2.2l.8-.8.8.8H8" fill="none" stroke="#000" stroke-width="1.1" stroke-linejoin="round"/></svg>'
        : '<svg width="20" height="24" viewBox="0 0 20 24"><path d="M1.5 1.5v17.2l4.1-3.9 2.8 6.4 2.9-1.2-2.7-6.3h5.7z" fill="#fff" stroke="#000" stroke-width="1.15" stroke-linejoin="round"/></svg>';
    }
    const [ox, oy] = this.cursorKind === 'text' ? [5, 10] : [1.5, 1.5];
    this.cursorEl.style.transform = `translate(${p.x - ox}px, ${p.y - oy}px)`;
    this.cursorEl.style.opacity = p.hidden ? 0 : 1;
    setScroll(this.desk, this.scrollY);
    const grow = (wrap, spring) => {
      wrap.style.height = `${Math.max(spring.value, 0) * wrap.firstElementChild.offsetHeight}px`;
      wrap.style.opacity = clamp(spring.value * 1.4 - 0.2, 0, 1);
    };
    grow(this.parts.newwrap, this.chat);
    grow(this.parts.incoming, this.incoming);
    const typing = this.parts.typing;
    const on = this.typing && clock.now >= this.typing[0] && clock.now < this.typing[1];
    typing.style.opacity = on ? 1 : 0;
    if (on) typing.querySelector('.dots').textContent = '.'.repeat(1 + (Math.floor((clock.now - this.typing[0]) * 3) % 3));
    this.parts.dropzone.style.opacity = clamp(this.drop.value, 0, 1);
  }
}

// ---------------------------------------------------------------- one screenshot
class Card {
  constructor(stage, snapshot, regions, selected) {
    this.stage = stage;
    this.regions = regions;
    this.sel = selected;
    this.long = null;
    this.l = new Spring(0); this.t = new Spring(0); this.r = new Spring(W); this.b = new Spring(H);
    this.radius = new Spring(0); this.px = new Spring(0); this.py = new Spring(0); this.sx = new Spring(1); this.sy = new Spring(1);
    this.ring = new Spring(0); this.shadow = new Spring(0); this.opacity = new Spring(1); this.slide = new Spring(0);
    this.shadowEl = el('div', 'cardshadow', stage.shadowLayer);
    this.root = el('div', 'thumb', stage.cardLayer);
    this.clip = el('div', 'clip', this.root);
    this.plane = el('div', 'plane', this.clip);
    this.plane.appendChild(snapshot);
    this.snapshot = snapshot;
    this.ringEl = el('div', 'ring', this.clip);
  }

  region() { return this.regions[this.sel]; }
  showsLong() { return this.long && (this.region().kind === 'scroll' || this.region().kind === 'long'); }

  source() {
    if (this.showsLong()) {
      const a = this.long.area;
      const visible = Math.min(this.long.rows, LONG_MAX / this.long.scale);
      return [a[0], a[1] + this.long.rows - visible, a[2], a[1] + this.long.rows];
    }
    return this.region().rect;
  }

  displaySize() {
    if (this.showsLong()) {
      const s = this.long.scale;
      return [Math.round((this.long.area[2] - this.long.area[0]) * s), Math.min(this.long.rows * s, LONG_MAX)];
    }
    return thumbSize(this.region().rect);
  }

  longHeight() { return this.long ? Math.min(this.long.rows * this.long.scale, LONG_MAX) : 0; }

  moveTo(rect, depth, spec, delay, inFlight) {
    const src = this.source();
    const w = rect.r - rect.l, h = rect.b - rect.t;
    const sx = w / (src[2] - src[0]), sy = h / (src[3] - src[1]);
    this.l.to(rect.l, spec, delay); this.t.to(rect.t, spec, delay); this.r.to(rect.r, spec, delay); this.b.to(rect.b, spec, delay);
    this.radius.to(12, spec, delay);
    this.px.to(rect.l - src[0] * sx, spec, delay); this.py.to(rect.t - src[1] * sy, spec, delay);
    this.sx.to(sx, spec, delay); this.sy.to(sy, spec, delay);
    const decoration = inFlight ? 0.22 : 0;
    const shown = depth <= 2;
    this.ring.to(1, K.fade, decoration);
    const slim = clamp(Math.min(w, h) / 40, 0.5, 1);
    this.shadow.to(!shown ? 0 : (depth === 0 ? 1 : 0.5) * slim, K.fade, decoration);
    this.opacity.to(shown ? 1 - depth * 0.14 : 0, K.fade);
  }

  // From here on the card shows the long image: the page from the top of the scrolled area down.
  beginLong(area) {
    const size = thumbSize(area);
    const page = buildLongPage();
    this.long = { area, rows: area[3] - area[1], scale: size[0] / (area[2] - area[0]), page };
    page.setRows(this.long.rows);
    const holder = el('div');
    holder.style.cssText = `position:absolute;left:0;top:${area[1]}px;width:640px;height:6000px;overflow:hidden`;
    holder.appendChild(page.el);
    this.plane.innerHTML = '';
    this.plane.appendChild(holder);
  }

  render() {
    const l = this.l.value, t = this.t.value, r = this.r.value, b = this.b.value;
    const w = Math.max(r - l, 1), h = Math.max(b - t, 1);
    const rad = this.radius.value;
    const op = clamp(this.opacity.value, 0, 1);
    const slide = this.slide.value;
    if (this.long) this.long.page.setRows(this.long.rows);
    this.root.style.transform = `translateX(${slide}px)`;
    this.root.style.opacity = op;
    Object.assign(this.clip.style, { left: `${l}px`, top: `${t}px`, width: `${w}px`, height: `${h}px`, borderRadius: `${rad}px` });
    this.plane.style.transform = `translate(${this.px.value - l}px, ${this.py.value - t}px) scale(${this.sx.value}, ${this.sy.value})`;
    this.ringEl.style.borderRadius = `${rad}px`;
    this.ringEl.style.boxShadow = `inset 0 0 0 2px rgba(255,255,255,${clamp(this.ring.value, 0, 1)})`;
    const sa = clamp(this.shadow.value, 0, 1) * op;
    Object.assign(this.shadowEl.style, {
      left: `${l + slide}px`, top: `${t}px`, width: `${w}px`, height: `${h}px`, borderRadius: `${rad}px`,
      boxShadow: `0 5px 18px rgba(0,0,0,${0.26 * sa}), 0 1px 3px rgba(0,0,0,${0.14 * sa})`,
    });
  }

  remove() { this.root.remove(); this.shadowEl.remove(); }
}

// ---------------------------------------------------------------- the region selector
class Capsule {
  constructor(stage) {
    this.x = new Spring(0); this.y = new Spring(0); this.opacity = new Spring(0); this.width = new Spring(34);
    this.pl = new Spring(3); this.pr = new Spring(41); this.pt = new Spring(3); this.pb = new Spring(31); this.prad = new Spring(14);
    this.palpha = new Spring(1); this.statusA = new Spring(0); this.pop = new Spring(1);
    this.targetX = 0; this.targetY = 0; this.placed = false;
    this.segments = []; this.retiring = []; this.sel = 0; this.visible = false; this.mode = 'selector';
    this.named = new Set();
    this.pieces = {};
    this.root = el('div', 'capsule', stage.fx);
    this.shadowEl = el('div', 'cshadow', this.root);
    this.frame = el('div', 'frame', this.root);
    this.inner = el('div', 'inner', this.frame);
    el('div', 'frost', this.inner);
    this.statusEl = el('div', 'status', this.inner);
    this.light = svgEl('svg', { class: 'layer', width: 700, height: 34 }, this.inner);
    this.pill = el('div', 'pill', this.inner);
    this.dark = svgEl('svg', { width: 700, height: 34, style: 'position:absolute;left:0;top:0' }, this.pill);
    this.statusLayer = svgEl('svg', { class: 'layer', width: 700, height: 34 }, this.inner);
  }

  // The name and, for the scrolling option, the keycaps that start it, laid out by what is visibly drawn.
  label(parent, kind, inkRight, center, color, keyColor) {
    const name = REGION_LABEL[kind];
    const m = measure(name);
    const baseline = center + m.cap / 2;
    const x = inkRight + 6;
    textEl(parent, name, x - m.left, baseline, color, color === 0xffffff ? 0.74 : 1);
    let right = x + m.width;
    if (kind === 'scroll') {
      const ky = Math.round(baseline - m.cap / 2 - 7.5);
      keycap(parent, 'win', right + 8, ky, keyColor[0], keyColor[1]);
      keycap(parent, 'enter', right + 8 + 17 + 2.5, ky, keyColor[0], keyColor[1]);
      right += 8 + 17 + 2.5 + 17;
    }
    return right;
  }

  makeSegment(region) {
    const s = { kind: region.kind, rect: region.rect, x: new Spring(0), iconA: new Spring(1), labelA: new Spring(0), placed: false };
    const spec = glyphSpec(region);
    s.lightG = svgEl('g', {}, this.light);
    s.darkG = svgEl('g', {}, this.dark);
    glyph(s.lightG, spec, 0xffffff, 0.74);
    const dg = glyph(s.darkG, spec, 0x16171a, 1);
    s.darkGlyph = dg.g;
    s.labelG = svgEl('g', {}, s.darkG);
    const center = (dg.inkTop + dg.inkBottom) / 2;
    const right = this.label(s.labelG, region.kind, dg.inkRight, center, 0x16171a, [toward(0x16171a, 0xf3f2ef, 1 - 0.46), 1]);
    if (this.named.has(region.kind)) this.label(svgEl('g', {}, s.lightG), region.kind, dg.inkRight, center, 0xffffff, [0xffffff, 0.34]);
    s.activeWidth = right + dg.inkLeft;
    return s;
  }

  widthOf(s, i) { return i === this.sel || this.named.has(s.kind) ? s.activeWidth : 38; }

  arrange(immediate) {
    let x = 3;
    this.segments.forEach((s, i) => {
      if (immediate || !s.placed) { s.x.snap(x); s.placed = true; } else s.x.to(x, K.slide);
      x += this.widthOf(s, i);
    });
    const chosen = this.segments[this.sel];
    const left = chosen.x.target, right = left + chosen.activeWidth;
    if (immediate) {
      this.width.snap(x + 3); this.pl.snap(left); this.pr.snap(right); this.pt.snap(3); this.pb.snap(31); this.prad.snap(14);
    } else {
      this.width.to(x + 3, K.capResize);
      this.shapePill(left, right, 3, 31, K.pillMorph);
    }
  }

  shapePill(left, right, top, bottom, spec) {
    const movingRight = right > this.pr.target;
    this.pl.to(left, movingRight ? K.pillTrail : K.pillLead);
    this.pr.to(right, movingRight ? K.pillLead : K.pillTrail);
    this.pt.to(top, spec); this.pb.to(bottom, spec); this.prad.to((bottom - top) / 2, spec);
  }

  bind(regions, selected) {
    const wasVisible = this.visible;
    if (this.mode !== 'selector') this.leaveStatus();
    if (regions.length < 2) { this.hide(); return; }
    const next = [];
    for (const region of regions) {
      const i = this.segments.findIndex((s) => s && s.kind === region.kind && String(s.rect) === String(region.rect));
      if (i >= 0) { next.push(this.segments[i]); this.segments[i] = null; }
      else { const s = this.makeSegment(region); s.iconA.snap(wasVisible ? 0 : 1); next.push(s); }
    }
    for (const s of this.segments) {
      if (!s) continue;
      s.iconA.to(0, K.quick); s.labelA.to(0, K.quick);
      this.retiring.push(s);
      later(0.3, () => { s.lightG.remove(); s.darkG.remove(); this.retiring = this.retiring.filter((r) => r !== s); });
    }
    this.segments = next;
    this.sel = clamp(selected, 0, next.length - 1);
    this.visible = true;
    next.forEach((s, i) => {
      const chosen = i === this.sel;
      if (wasVisible) { s.iconA.to(1, K.fade, 0.05); s.labelA.to(chosen ? 1 : 0, chosen ? K.fade : K.quick, chosen ? 0.05 : 0); }
      else { s.iconA.snap(1); s.labelA.snap(chosen ? 1 : 0); }
    });
    if (wasVisible) { this.arrange(false); this.opacity.to(1, K.fade); }
    else {
      this.arrange(true);
      this.x.snap(this.targetX); this.y.snap(this.targetY + 10); this.opacity.snap(0);
      this.opacity.to(1, K.fade, 0.14); this.y.to(this.targetY, K.capEnter, 0.14);
    }
  }

  moveTo(x, y) {
    const first = !this.placed;
    this.placed = true;
    this.targetX = x; this.targetY = y;
    if (first) { this.x.snap(x); this.y.snap(y); return; }
    if (!this.visible) return;
    this.x.to(x, K.reflow); this.y.to(y, K.reflow);
  }

  select(index) {
    if (this.mode !== 'selector' || index === this.sel) return;
    this.segments[this.sel].labelA.to(0, K.quick);
    this.sel = index;
    this.segments[index].labelA.to(1, K.fade, 0.05);
    this.arrange(false);
  }

  hide() {
    this.visible = false;
    this.opacity.to(0, K.capLeave);
    this.y.to(this.targetY + 10, K.capLeave);
  }

  piece(name, build) {
    if (!this.pieces[name]) {
      const g = svgEl('g', { opacity: 0 }, this.statusLayer);
      this.pieces[name] = { g, alpha: new Spring(0), ...build(g) };
    }
    return this.pieces[name];
  }

  buildRecording() {
    const center = 17, textX = Math.round(center + 4 + 8);
    const line = (name, text) => this.piece(name, (g) => {
      const m = measure(text);
      textEl(g, text, textX - m.left, center + m.cap / 2, 0xffffff);
      return { inkWidth: m.width };
    });
    line('waiting', 'Scroll to capture');
    line('capturing', 'Recording');
    this.piece('finish', (g) => {
      const m = measure('to finish');
      const baseline = center + m.cap / 2;
      const ky = Math.round(baseline - m.cap / 2 - 7.5);
      keycap(g, 'win', 0, ky, 0xffffff);
      keycap(g, 'enter', 19.5, ky, 0xffffff);
      textEl(g, 'to finish', 41.5 - m.left, baseline, 0xffffff);
      return { inkWidth: 41.5 + m.width };
    });
    this.finishX = this.finishX || new Spring(0);
    return textX;
  }

  showRecording(capturing) {
    const textX = this.buildRecording();
    const entering = this.mode !== 'recording';
    if (!entering && capturing === this.capturing) return;
    if (entering) {
      this.mode = 'recording';
      for (const s of this.segments) { s.iconA.to(0, K.quick); s.labelA.to(0, K.quick); }
      this.shapePill(13, 21, 13, 21, K.pillMorph);
      this.breathFrom = clock.now + 0.38;
      this.pieces.finish.alpha.to(0.58, K.fade, 0.1);
    }
    this.capturing = capturing;
    const shown = this.pieces[capturing ? 'capturing' : 'waiting'];
    this.pieces[capturing ? 'waiting' : 'capturing'].alpha.to(0, K.quick);
    shown.alpha.to(0.94, K.fade, 0.05);
    const finishLeft = Math.round(textX + shown.inkWidth + 14);
    if (entering) this.finishX.snap(finishLeft); else this.finishX.to(finishLeft, K.slide);
    this.width.to(finishLeft + this.pieces.finish.inkWidth + 13, K.capResize);
  }

  showOutcome(kind) {
    this.mode = 'outcome';
    this.palpha.snap(this.breathValue());
    this.breathFrom = null;
    for (const n of ['waiting', 'capturing', 'finish']) this.pieces[n]?.alpha.to(0, K.quick);
    this.shapePill(17, 17, 17, 17, K.fade);
    this.palpha.to(0, K.quick);
    const color = { ok: 0x1c7c43, partial: 0x0d4527, fail: 0x8e1519 }[kind];
    const mark = { ok: 'check', partial: 'exclamation', fail: 'cross' }[kind];
    const text = { ok: 'Scrolling Screenshot Saved', partial: 'Stopped Early, Saved', fail: 'Scrolling Screenshot Failed' }[kind];
    this.statusEl.style.background = hex(color);
    this.statusA.to(1, K.fade);
    for (const n of ['icon', 'label']) if (this.pieces[n]) { this.pieces[n].g.remove(); delete this.pieces[n]; }
    const icon = this.piece('icon', (g) => { statusIcon(g, mark, 10, 10, 0xffffff, 1); return {}; });
    const inkLeft = mark === 'exclamation' ? 5.9 : mark === 'check' ? 1.85 : 2.65;
    const inkW = mark === 'exclamation' ? 2.2 : mark === 'check' ? 10.3 : 9.7;
    const inset = 10 + inkLeft;
    const labelLeft = Math.round(inset + inkW + 8);
    const m = measure(text);
    this.piece('label', (g) => { textEl(g, text, labelLeft - m.left, 17 + m.cap / 2, 0xffffff); return {}; });
    this.pop.snap(0.45); this.pop.to(1, K.resultPop);
    icon.alpha.to(0.94, K.fade);
    this.pieces.label.alpha.to(0.94, K.fade, 0.05);
    this.width.to(labelLeft + m.width + inset, K.capResize);
  }

  leaveStatus() {
    this.mode = 'selector';
    this.breathFrom = null;
    this.statusA.to(0, K.fade);
    for (const p of Object.values(this.pieces)) p.alpha.to(0, K.quick);
    this.palpha.to(1, K.fade, 0.06);
  }

  breathValue() {
    if (this.breathFrom == null || clock.now < this.breathFrom) return 1;
    return 0.65 + 0.35 * Math.sin(TAU * 0.75 * (clock.now - this.breathFrom) + TAU * 0.25);
  }

  render() {
    const w = this.width.value;
    this.root.style.transform = `translate(${this.x.value}px, ${this.y.value}px)`;
    this.root.style.opacity = clamp(this.opacity.value, 0, 1);
    this.frame.style.width = `${w}px`;
    this.inner.style.width = `${Math.max(w - 2, 0)}px`;
    Object.assign(this.shadowEl.style, { width: `${w}px`, boxShadow: '0 4px 18px rgba(0,0,0,.34)' });
    for (const s of [...this.segments, ...this.retiring]) {
      const x = s.x.value, ia = clamp(s.iconA.value, 0, 1);
      s.lightG.setAttribute('transform', `translate(${x} 3)`);
      s.lightG.setAttribute('opacity', ia);
      s.darkG.setAttribute('transform', `translate(${x} 3)`);
      s.darkGlyph.setAttribute('opacity', ia);
      s.labelG.setAttribute('opacity', clamp(s.labelA.value, 0, 1));
    }
    const pl = this.pl.value, pr = Math.max(this.pr.value, pl), pt = this.pt.value, pb = Math.max(this.pb.value, pt);
    this.pill.style.clipPath = `inset(${pt}px ${700 - pr}px ${34 - pb}px ${pl}px round ${Math.max(this.prad.value, 0)}px)`;
    const breathing = this.mode === 'recording' ? this.breathValue() : 1;
    this.pill.style.opacity = clamp(this.palpha.value * breathing, 0, 1);
    this.statusEl.style.opacity = clamp(this.statusA.value, 0, 1);
    for (const [name, p] of Object.entries(this.pieces)) {
      p.g.setAttribute('opacity', clamp(p.alpha.value, 0, 1));
      if (name === 'finish') p.g.setAttribute('transform', `translate(${this.finishX.value} 0)`);
      if (name === 'icon') p.g.setAttribute('transform', `translate(17 17) scale(${this.pop.value}) translate(-17 -17)`);
    }
  }
}

// ---------------------------------------------------------------- the rubbish bin
class Bin {
  constructor(stage) {
    this.presence = new Spring(0); this.left = new Spring(-19); this.right = new Spring(19); this.labelA = new Spring(0);
    this.over = new Spring(0); this.lidOpen = new Spring(0); this.lidAim = new Spring(0); this.squash = new Spring(0);
    this.cx = 640; this.cy = Math.round(BOTTOM - 18 - 19);
    this.showing = false; this.near = false; this.engaged = false; this.closeness = 0; this.slamming = false; this.hideAt = null;
    const m = measure('Rubbish Bin', 14);
    const unit = 20 / 24;
    const iconInkW = 15.8 * unit, iconInkL = 4.1 * unit;
    this.iconX = Math.round((38 - iconInkW) / 2) - iconInkL;
    this.labelX = this.iconX + iconInkL + iconInkW + 7.5;
    this.full = Math.round(this.labelX + m.width + 13.5) & ~1;
    this.root = el('div', 'bin', stage.fx);
    this.shadowEl = el('div', 'bshadow', this.root);
    this.body = el('div', 'body', this.root);
    this.inner = el('div', 'binner', this.body);
    this.overEl = el('div', 'over', this.inner);
    const svgRoot = svgEl('svg', { width: 400, height: 38, style: 'position:absolute;left:-1px;top:-1px;overflow:visible' }, this.inner);
    this.icon = svgEl('g', {}, svgRoot);
    const mask = svgEl('mask', { id: 'binslots' }, this.icon);
    svgEl('rect', { x: 0, y: 0, width: 24, height: 24, fill: '#fff' }, mask);
    svgEl('path', { d: 'M9.6 12.9L9.9 18.2M14.4 12.9L14.1 18.2', stroke: '#000', 'stroke-width': 1.44, 'stroke-linecap': 'round' }, mask);
    this.can = svgEl('g', {}, this.icon);
    svgEl('path', { d: 'M7.3 9.6H16.7Q18.7 9.6 18.6 11.6L17.9 19.4Q17.8 21.4 15.8 21.4H8.2Q6.2 21.4 6.1 19.4L5.4 11.6Q5.3 9.6 7.3 9.6Z', fill: '#fff', mask: 'url(#binslots)' }, this.can);
    this.lid = svgEl('g', {}, this.icon);
    svgEl('rect', { x: 4.1, y: 6.05, width: 15.8, height: 2.5, rx: 1.25, fill: '#fff' }, this.lid);
    svgEl('rect', { x: 9.2, y: 3.3, width: 5.6, height: 3.4, rx: 1.1, fill: '#fff' }, this.lid);
    this.unit = unit;
    this.label = textEl(svgRoot, 'Rubbish Bin', this.labelX - m.left, 19 + m.cap / 2, 0xffffff, 1, 14);
  }

  show() { this.showing = true; this.slamming = false; this.hideAt = null; this.near = this.engaged = false; this.closeness = 0; }
  hide() { this.showing = false; this.near = this.engaged = false; this.closeness = 0; }

  track(x, y) {
    const d = Math.hypot(Math.max(Math.abs(x - this.cx) - this.full / 2, 0), Math.max(Math.abs(y - this.cy) - 19, 0));
    this.near = d < 170 + (this.near ? 30 : 0);
    this.closeness = clamp(1 - d / 170, 0, 1);
    const margin = this.engaged ? 38 : 16;
    this.engaged = Math.abs(x - this.cx) <= this.full / 2 + margin && Math.abs(y - this.cy) <= 19 + margin;
    return this.engaged;
  }

  aim(x, y) { this.lidAim.target = clamp(Math.atan2(x - this.cx, Math.max(this.cy - y, 1)) / (55 * Math.PI / 180), -1, 1); this.lidAim.spec = K.binLidAim; }

  gulp() { this.slamming = true; this.engaged = false; this.squash.kick(3.4); this.hideAt = clock.now + 0.3; }

  frame() {
    if (this.hideAt != null && clock.now >= this.hideAt) { this.hideAt = null; this.hide(); }
    const expanded = this.near || this.engaged || this.slamming;
    const half = (expanded ? this.full : 38) / 2;
    Object.assign(this.left, { target: -half, spec: K.binMorph });
    Object.assign(this.right, { target: half, spec: K.binMorph });
    Object.assign(this.labelA, { target: expanded ? 1 : 0, spec: K.fade });
    Object.assign(this.over, { target: this.engaged ? 1 : 0, spec: K.fade });
    this.lidOpen.target = this.slamming ? 0 : this.engaged ? 1 : this.near ? this.closeness * 0.34 : 0;
    this.lidOpen.spec = this.slamming ? K.binLidSlam : K.binLid;
    Object.assign(this.presence, { target: this.showing ? 1 : 0, spec: this.showing ? K.capEnter : K.capLeave });
    Object.assign(this.squash, { target: 0, spec: K.binCan });
    if (!this.showing && this.presence.value < 0.004 && this.hideAt == null) {
      this.left.snap(-19); this.right.snap(19); this.labelA.snap(0); this.lidOpen.snap(0); this.lidAim.snap(0); this.slamming = false;
    }
  }

  render() {
    const p = this.presence.value;
    this.root.style.transform = `translate(${this.cx}px, ${this.cy + (1 - p) * 10}px)`;
    this.root.style.opacity = clamp(p * 1.5, 0, 1);
    const left = this.left.value, right = Math.max(this.right.value, left + 38 * 0.6);
    Object.assign(this.body.style, { left: `${left}px`, width: `${right - left}px` });
    Object.assign(this.shadowEl.style, { left: `${left}px`, width: `${right - left}px` });
    Object.assign(this.inner.style, { left: '1px', width: `${right - left - 2}px` });
    this.overEl.style.opacity = clamp(this.over.value, 0, 1);
    const squash = clamp(this.squash.value, -0.16, 0.16);
    const canX = 1 + squash * 0.55, canY = 1 - squash;
    this.can.setAttribute('transform', `matrix(${canX} 0 0 ${canY} ${12 * (1 - canX)} ${21.4 * (1 - canY)})`);
    const canDrop = (21.4 - 9.6) * squash;
    const open = Math.max(this.lidOpen.value, -0.12);
    const aim = clamp(this.lidAim.value, -1, 1);
    const rad = -aim * 40 * Math.PI / 180 * Math.max(open, 0);
    const c = Math.cos(rad), s = Math.sin(rad);
    const hx = 12 + (aim < 0 ? 7.9 : -7.9), hy = 7.3;
    const rise = open * (3.2 * (1 - Math.abs(aim)) + 0.6);
    const slide = -aim * open * 0.9;
    this.lid.setAttribute('transform', `matrix(${c} ${s} ${-s} ${c} ${hx - (hx * c - hy * s) + slide} ${hy - (hx * s + hy * c) - rise + canDrop})`);
    this.icon.setAttribute('transform', `translate(${this.iconX} ${19 - (3.3 + 21.4) * this.unit / 2}) scale(${this.unit})`);
    this.label.setAttribute('opacity', clamp(this.labelA.value, 0, 1));
  }
}

// ---------------------------------------------------------------- a thumbnail in the air
class Sprite {
  constructor(stage, card, pointer) {
    this.stage = stage;
    card.render();
    const l = card.l.value, t = card.t.value;
    this.w = card.r.value - l; this.h = card.b.value - t;
    this.gx = pointer.x - l; this.gy = pointer.y - t;
    this.ax = new Spring(pointer.x); this.ay = new Spring(pointer.y);
    this.scale = new Spring(1).to(0.72, K.dragLift);
    this.tilt = new Spring(0); this.alpha = new Spring(1); this.badge = new Spring(0); this.pull = new Spring(0);
    this.speed = 0; this.drift = null; this.accepting = false; this.inBin = false;
    this.root = el('div', 'sprite', stage.spriteLayer);
    this.shadowEl = el('div', 'sshadow', this.root);
    Object.assign(this.shadowEl.style, { width: `${this.w}px`, height: `${this.h}px`, borderRadius: '12px' });
    this.clip = card.clip.cloneNode(true);
    this.clip.style.left = '0px'; this.clip.style.top = '0px';
    this.root.appendChild(this.clip);
    this.badgeEl = el('div', 'badge', stage.spriteLayer);
    this.badgeEl.innerHTML = `<svg width="34" height="34" viewBox="-17 -17 34 34" style="overflow:visible">
      <circle cx="0" cy="1.4" r="11" fill="rgba(0,0,0,.18)" filter="blur(1.6px)"/><circle r="11" fill="#fff"/><circle r="9.5" fill="#0a84ff"/>
      <path d="M-5.06 0H5.06M0 -5.06V5.06" stroke="#fff" stroke-width="2.1" stroke-linecap="round"/></svg>`;
  }

  frame(dt, pointer) {
    if (this.drift) {
      this.drift.vx *= Math.exp(-16 * dt); this.drift.vy *= Math.exp(-16 * dt);
      this.ax.snap(this.ax.value + this.drift.vx * dt); this.ay.snap(this.ay.value + this.drift.vy * dt);
      return;
    }
    Object.assign(this.ax, { target: pointer.x, spec: K.catchUp });
    Object.assign(this.ay, { target: pointer.y, spec: K.catchUp });
    const vx = (pointer.x - (this.lastPx ?? pointer.x)) / dt;
    this.lastPx = pointer.x;
    this.speed += (vx - this.speed) * (1 - Math.exp(-dt / 0.035));
    Object.assign(this.tilt, { target: clamp(this.speed * 2.6 / 2600, -2.6, 2.6), spec: K.tilt });
  }

  setAccepting(on) {
    if (on === this.accepting) return;
    this.accepting = on;
    this.badge.to(on ? 1 : 0, on ? K.badgeIn : K.badgeOut);
  }

  vanish(velocity) {
    this.drift = { vx: velocity.x, vy: velocity.y };
    this.scale.to(0.72 * 0.8, K.dropVanish);
    this.alpha.to(0, K.dropVanish);
    this.badge.to(0, K.badgeOut);
  }

  render() {
    const s = this.scale.value, p = clamp(this.pull.value, 0, 1);
    const bin = this.stage.bin;
    const iconX = bin.cx + bin.left.value + 1 + bin.iconX + 10, iconY = bin.cy;
    // Drawn into the bin: pulled toward its icon, narrowing on the way in, while it stays on the pointer.
    const pe = smooth(p);
    const x = this.ax.value + (iconX - this.ax.value) * pe;
    const y = this.ay.value + (iconY - this.ay.value) * pe;
    const shrink = 1 - pe * 0.97;
    const sx = s * shrink, sy = s * shrink * (1 + 0.225 * Math.sin(Math.PI * pe));
    const gx = this.gx + (this.w / 2 - this.gx) * pe, gy = this.gy + (this.h - this.gy) * pe;
    const persp = pe > 0 ? ` perspective(600px) rotateX(${-28 * Math.sin(Math.PI * Math.min(pe, 0.5))}deg)` : '';
    this.root.style.transform = `translate(${x}px, ${y}px) rotate(${this.tilt.value}deg)${persp} scale(${sx}, ${sy}) translate(${-gx}px, ${-gy}px)`;
    this.root.style.opacity = clamp(this.alpha.value, 0, 1) * (p > 0.85 ? 0 : 1);
    const lift = clamp((1 - s) / 0.28, 0, 1);
    this.shadowEl.style.boxShadow = `0 ${5 + 6 * lift}px ${18 + 12 * lift}px rgba(0,0,0,${0.26 + 0.08 * lift}), 0 ${1 + 1.5 * lift}px ${3 + 4 * lift}px rgba(0,0,0,${0.14 + 0.02 * lift})`;
    this.shadowEl.style.opacity = 1 - pe;
    const b = this.badge.value;
    const rad = this.tilt.value * Math.PI / 180;
    const ox = -gx * sx, oy = -gy * sy;
    const cx = x + ox * Math.cos(rad) - oy * Math.sin(rad) + 3, cy = y + ox * Math.sin(rad) + oy * Math.cos(rad) + 3;
    this.badgeEl.style.transform = `translate(${cx - 17}px, ${cy - 17}px) scale(${Math.max(b, 0)})`;
    this.badgeEl.style.transformOrigin = '17px 17px';
    this.badgeEl.style.opacity = clamp(b * 1.6, 0, 1) * clamp(this.alpha.value * 1.5, 0, 1);
  }

  remove() { this.root.remove(); this.badgeEl.remove(); }
}

// ---------------------------------------------------------------- the key overlay
const KEY_ICONS = {
  Win: '<svg width="13" height="13"><g fill="#f3f2ef"><rect width="6" height="6" rx="1"/><rect x="7" width="6" height="6" rx="1"/><rect y="7" width="6" height="6" rx="1"/><rect x="7" y="7" width="6" height="6" rx="1"/></g></svg>',
  Shift: '<svg width="14" height="14"><path d="M7 1.8L1.8 7.2h3v5h4.4v-5h3z" fill="none" stroke="#f3f2ef" stroke-width="1.4" stroke-linejoin="round"/></svg>',
  Enter: '<svg width="16" height="14"><path d="M13.5 2v5.5H3M6.5 4L3 7.5 6.5 11" fill="none" stroke="#f3f2ef" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  PrtSc: '<svg width="15" height="13" viewBox="0 0 15 13"><path d="M1.5 4.5V2.8a1.3 1.3 0 0 1 1.3-1.3h1.7M10.5 1.5h1.7a1.3 1.3 0 0 1 1.3 1.3v1.7M13.5 8.5v1.7a1.3 1.3 0 0 1-1.3 1.3h-1.7" fill="none" stroke="#f3f2ef" stroke-width="1.5" stroke-linecap="round"/><rect x="1.5" y="7" width="6" height="4.5" rx="1.2" fill="#f3f2ef"/></svg>',
  'Left Click': '<svg width="12" height="16" viewBox="0 0 12 16"><rect x="1" y="1" width="10" height="14" rx="5" fill="none" stroke="#f3f2ef" stroke-width="1.4"/><path d="M6 1.2V6.5H1.2V6A4.8 4.8 0 0 1 6 1.2z" fill="#f3f2ef"/><path d="M6 1v5.5M1 6.5h10" stroke="#f3f2ef" stroke-width="1.2"/></svg>',
  Scroll: '<svg width="12" height="16" viewBox="0 0 12 16"><rect x="1" y="1" width="10" height="14" rx="5" fill="none" stroke="#f3f2ef" stroke-width="1.4"/><rect x="4.9" y="3.2" width="2.2" height="4.4" rx="1.1" fill="#f3f2ef"/></svg>',
};
const KEY_BIG = {
  Left: '<svg width="20" height="16"><path d="M17 8H3.5M8.5 3L3.5 8l5 5" fill="none" stroke="#f3f2ef" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>',
  Right: '<svg width="20" height="16"><path d="M3 8h13.5M11.5 3l5 5-5 5" fill="none" stroke="#f3f2ef" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"/></svg>',
};

class Hud {
  constructor(stage) {
    this.root = el('div', 'hud', stage.fx);
    this.alpha = new Spring(0);
    this.keys = {};
  }
  show(names) {
    this.root.innerHTML = '';
    this.keys = {};
    for (const name of names) {
      const key = el('div', 'kc', this.root);
      el('div', 'depth', key);
      const face = el('div', 'kf', key);
      if (KEY_BIG[name]) face.innerHTML = KEY_BIG[name];
      else if (name.length === 1) face.innerHTML = `<span class="big">${name}</span>`;
      else face.innerHTML = `${KEY_ICONS[name] || ''}<span class="lbl">${name}</span>`;
      this.keys[name] = { face, press: new Spring(0) };
    }
    if (this.alpha.value < 0.05) this.alpha.snap(0);
    this.alpha.to(1, K.hudIn);
  }
  down(...names) { for (const n of names) this.keys[n]?.press.to(1, K.keyDown); }
  up(...names) { for (const n of names) this.keys[n]?.press.to(0, K.keyUp); }
  tap(name, hold = 0.11) { this.down(name); later(hold, () => this.up(name)); }
  hide() { this.alpha.to(0, K.hudOut); }
  render() {
    const a = this.alpha.value;
    this.root.style.opacity = clamp(a, 0, 1);
    this.root.style.transform = `translate(${-this.root.offsetWidth / 2}px, ${(1 - a) * -8}px) scale(${0.94 + 0.06 * clamp(a, 0, 1.2)})`;
    for (const k of Object.values(this.keys)) k.face.style.transform = `translateY(${clamp(k.press.value, -0.3, 1.2) * 5}px)`;
  }
}

// ---------------------------------------------------------------- scenes
const SCENES = {};
let stage = null;
let scene = null;

function pathAt(path, t) {
  let prev = path[0];
  if (t <= prev.t) return { x: prev.x, y: prev.y, hidden: prev.hidden };
  for (let i = 1; i < path.length; i++) {
    const p = path[i];
    if (t <= p.t) {
      const u = (t - prev.t) / (p.t - prev.t);
      const e = p.ease === 'linear' ? u : p.ease === 'out' ? 1 - Math.pow(1 - u, 3) : ease(u);
      const arc = (p.arc ?? 0) * Math.sin(Math.PI * e);
      const dx = p.x - prev.x, dy = p.y - prev.y, len = Math.hypot(dx, dy) || 1;
      return { x: prev.x + dx * e - (dy / len) * arc, y: prev.y + dy * e + (dx / len) * arc, hidden: p.hidden };
    }
    prev = p;
  }
  return { x: prev.x, y: prev.y, hidden: prev.hidden };
}

function reset(name) {
  clock.now = 0; clock.springs = []; clock.events = [];
  stage = new Stage(document.getElementById('root'));
  scene = SCENES[name || new URLSearchParams(location.search).get('scene') || 'capture']();
  clock.events = scene.events.slice().sort((a, b) => a.t - b.t);
  stage.pointer = pathAt(scene.path, 0);
  stage.lastPointer = { ...stage.pointer };
  if (scene.hud) Object.assign(stage.hud.root.style, { left: `${scene.hud[0]}px`, top: `${scene.hud[1]}px` });
  scene.setup?.(stage);
}

const DT = 1 / 480;
function advance(to) {
  while (clock.now + DT <= to + 1e-9) {
    clock.now += DT;
    stage.pointer = pathAt(scene.path, clock.now);
    stage.pointerVelocity = { x: (stage.pointer.x - stage.lastPointer.x) / DT, y: (stage.pointer.y - stage.lastPointer.y) / DT };
    stage.lastPointer = { ...stage.pointer };
    if (scene.cursors) for (const [t, kind] of scene.cursors) if (t <= clock.now) stage.cursorKind = kind;
    while (clock.events.length && clock.events[0].t <= clock.now) clock.events.shift().fn(stage);
    if (scene.scroll) stage.scrollY = scene.scroll(clock.now);
    if (stage.sprite) stage.sprite.frame(DT, stage.pointer);
    scene.tick?.(stage, DT);
    stage.bin.frame();
    for (const s of clock.springs) s.step(DT);
  }
}

window.frame = (t) => {
  if (!stage || t < clock.now - 1e-6) reset();
  advance(t);
  stage.render();
  return clock.now;
};

window.ready = window.ready || (async () => {
  await document.fonts.ready;
  makePhotos();
  if (document.getElementById('root')) window.frame(0);
  await new Promise((r) => setTimeout(r, 300));
  return true;
})();
