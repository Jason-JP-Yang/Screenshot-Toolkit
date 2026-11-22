// The demos. Each returns a pointer path, cursor changes, timed events, and optional per-step logic.
const R = {
  full: { kind: 'full', rect: [0, 0, 1280, 800] },
  notb: { kind: 'notb', rect: [0, 0, 1280, 752] },
  browser: { kind: 'app', rect: [0, 0, 640, 752] },
  relay: { kind: 'zone', rect: [640, 0, 1280, 752] },
  scroll: { kind: 'scroll', rect: [0, VIEW_TOP, 628, 752] },
  long: { kind: 'long', rect: [0, VIEW_TOP, 628, 752] },
};
const WINDOWS = [R.full, R.notb, R.browser, R.relay];
const OVER_CHAT = (p) => p.x > 881 && p.y > 92 && p.y < 752;

function bursts(list) {
  return (t) => {
    let y = list.length ? list[0][2] : 0;
    for (const [a, b, from, to] of list) {
      if (t < a) return y;
      if (t <= b) return from + (to - from) * (1 - Math.pow(1 - (t - a) / (b - a), 3));
      y = to;
    }
    return y;
  };
}

function scenario() {
  const events = [];
  return { events, at: (t, fn) => events.push({ t, fn }) };
}

// Picking a thumbnail up, carrying it, and letting go over a window or the rubbish bin.
function drag(S) {
  const card = S.front();
  S.removeCard(card);
  card.remove();
  const sprite = new Sprite(S, card, S.pointer);
  sprite.card = card;
  S.sprite = sprite;
  S.layout(K.reflow);
  S.capsule.hide();
  S.bin.show();
}

function carry(S, accepts) {
  const sprite = S.sprite;
  if (!sprite || sprite.released) return;
  const p = S.pointer;
  const engaged = S.bin.track(p.x, p.y);
  S.bin.aim(sprite.ax.value, sprite.ay.value);
  if (engaged !== sprite.inBin) {
    sprite.inBin = engaged;
    S.bin.squash.kick(engaged ? 2.2 : -1.6);
  }
  if (engaged) sprite.pull.to(1, K.binPull);
  else {
    const c = S.bin.closeness;
    const target = c > 0.35 ? 0.14 * (c - 0.35) / 0.65 : 0;
    if (Math.abs(sprite.pull.target - target) > 1e-4) sprite.pull.to(target, K.binPull);
  }
  const accepting = !engaged && accepts(p);
  sprite.setAccepting(accepting);
  if (S.drop.target !== (accepting ? 1 : 0)) S.drop.to(accepting ? 1 : 0, K.fade);
}

function release(S, onDrop) {
  const sprite = S.sprite;
  sprite.released = true;
  S.drop.to(0, K.fade);
  if (sprite.inBin) {
    sprite.pull.to(1, K.binSwallow);
    S.bin.gulp();
  } else {
    sprite.vanish(S.pointerVelocity);
    S.bin.hide();
    onDrop?.(sprite);
  }
  later(0.6, () => { sprite.remove(); if (S.sprite === sprite) S.sprite = null; });
}

function exit(S, card, opacity) {
  S.removeCard(card);
  S.exiting.push(card);
  card.slide.to(-(card.r.target + 48), K.exit);
  card.opacity.to(opacity, K.fade);
  later(0.7, () => { card.remove(); S.exiting = S.exiting.filter((c) => c !== card); });
  if (S.cards.length) { S.layout(K.reflow); S.capsule.bind(S.front().regions, S.front().sel); } else S.capsule.hide();
}

// Capture, choose the browser window after a teammate asks for it, and drag the screenshot into the chat.
SCENES.capture = () => {
  const { events, at } = scenario();
  at(0.4, (S) => S.hud.show(['PrtSc']));
  at(0.66, (S) => S.hud.down('PrtSc'));
  at(0.68, (S) => S.capture(WINDOWS, 0));
  at(0.8, (S) => S.hud.up('PrtSc'));
  at(1.35, (S) => S.hud.hide());
  at(0.9, (S) => { S.typing = [1.0, 2.05]; });
  at(2.05, (S) => S.incoming.to(1, K.chat));
  at(2.55, (S) => S.hud.show(['Win', 'Shift', 'Right']));
  at(2.7, (S) => S.hud.down('Win'));
  at(2.78, (S) => S.hud.down('Shift'));
  at(2.92, (S) => { S.hud.tap('Right'); S.select(1); });
  at(3.42, (S) => { S.hud.tap('Right'); S.select(2); });
  at(3.75, (S) => S.hud.up('Win', 'Shift'));
  at(4.05, (S) => S.hud.hide());
  at(4.75, (S) => S.hud.show(['Left Click']));
  at(4.95, (S) => S.hud.down('Left Click'));
  at(5.02, (S) => drag(S));
  at(6.45, (S) => {
    S.hud.up('Left Click');
    release(S, (sprite) => {
      const shot = S.parts.shot;
      const copy = sprite.card.snapshot.cloneNode(true);
      copy.style.transform = `scale(${212 / 640})`;
      copy.style.transformOrigin = '0 0';
      shot.innerHTML = '';
      shot.appendChild(copy);
      S.chat.to(1, K.chat, 0.14);
    });
  });
  at(6.95, (S) => S.hud.hide());
  return {
    events,
    path: [
      { t: 0, x: 330, y: 420 },
      { t: 4.1, x: 330, y: 420 },
      { t: 4.8, x: 92, y: 650, arc: -30 },
      { t: 4.95, x: 92, y: 650 },
      { t: 5.02, x: 98, y: 643, ease: 'linear' },
      { t: 6.25, x: 1010, y: 430, arc: 70 },
      { t: 6.45, x: 1016, y: 426, ease: 'out' },
      { t: 7.1, x: 1016, y: 426 },
      { t: 7.8, x: 1068, y: 478, arc: 10 },
    ],
    cursors: [[0, 'text'], [4.45, 'arrow'], [7.5, 'text']],
    tick: (S) => carry(S, OVER_CHAT),
  };
};

// A scrolling screenshot of the article: a fixed pattern behind it, a see-through bar, and a floating player.
SCENES.scrolling = () => {
  const { events, at } = scenario();
  const plan = [[4.0, 4.75, 0, 440], [4.85, 5.65, 440, 960], [5.75, 6.55, 960, 1480]];
  const scroll = bursts(plan);
  let recording = false;
  let lastRead = 0;
  at(0.4, (S) => S.hud.show(['PrtSc']));
  at(0.66, (S) => S.hud.down('PrtSc'));
  at(0.68, (S) => S.capture(WINDOWS, 0));
  at(0.8, (S) => S.hud.up('PrtSc'));
  at(1.25, (S) => { const c = S.front(); c.regions = [...WINDOWS, R.scroll]; S.capsule.bind(c.regions, c.sel); });
  at(1.35, (S) => S.hud.hide());
  at(1.75, (S) => S.hud.show(['Win', 'Shift', 'Left']));
  at(1.9, (S) => S.hud.down('Win'));
  at(1.98, (S) => S.hud.down('Shift'));
  at(2.12, (S) => { S.hud.tap('Left'); S.select(4); });
  at(2.45, (S) => S.hud.up('Win', 'Shift'));
  at(2.7, (S) => S.hud.hide());
  at(2.95, (S) => S.hud.show(['Win', 'Enter']));
  at(3.1, (S) => S.hud.down('Win'));
  at(3.2, (S) => {
    S.hud.tap('Enter');
    S.front().beginLong(R.scroll.rect);
    S.capsule.showRecording(false);
    recording = true;
  });
  at(3.45, (S) => S.hud.up('Win'));
  at(3.65, (S) => S.hud.hide());
  at(3.85, (S) => S.hud.show(['Scroll']));
  for (const [a, b] of plan) {
    at(a, (S) => S.hud.down('Scroll'));
    at(b - 0.25, (S) => S.hud.up('Scroll'));
  }
  at(4.05, (S) => S.capsule.showRecording(true));
  at(6.75, (S) => S.hud.hide());
  at(7.0, (S) => S.hud.show(['Win', 'Enter']));
  at(7.15, (S) => S.hud.down('Win'));
  at(7.25, (S) => { S.hud.tap('Enter'); recording = false; S.capsule.showOutcome('ok'); });
  at(7.5, (S) => S.hud.up('Win'));
  at(7.85, (S) => S.hud.hide());
  at(8.75, (S) => { const c = S.front(); c.regions = [...WINDOWS, R.long]; S.capsule.bind(c.regions, 4); });
  return {
    events,
    path: [
      { t: 0, x: 300, y: 470 },
      { t: 9.0, x: 300, y: 470 },
      { t: 9.7, x: 392, y: 380, arc: 14 },
    ],
    cursors: [[0, 'text']],
    scroll,
    hud: [350, 22],
    tick: (S) => {
      if (!recording) return;
      const moving = Math.abs(scroll(clock.now) - scroll(clock.now - 0.03)) > 0.5;
      if (clock.now - lastRead < (moving ? 0.025 : 0.09)) return;
      lastRead = clock.now;
      const card = S.front();
      const rows = VIEW_H + Math.floor(S.scrollY / 24) * 24;
      if (rows !== card.long.rows) { card.long.rows = rows; S.layout(K.follow); }
    },
  };
};

// A burst of three shots while reading. One goes in the rubbish bin, one is swiped away, one leaves on its own.
SCENES.bin = () => {
  const { events, at } = scenario();
  at(0.3, (S) => S.hud.show(['PrtSc']));
  for (const t of [0.55, 1.1, 1.65]) {
    at(t - 0.02, (S) => S.hud.tap('PrtSc', 0.1));
    at(t, (S) => S.capture(WINDOWS, 0));
  }
  at(2.15, (S) => S.hud.hide());
  at(3.0, (S) => S.hud.show(['Left Click']));
  at(3.12, (S) => S.hud.down('Left Click'));
  at(3.2, (S) => drag(S));
  at(4.82, (S) => { S.hud.up('Left Click'); release(S); });
  at(5.0, (S) => { if (S.cards.length) S.capsule.bind(S.front().regions, S.front().sel); });
  let swipe = null;
  at(5.72, (S) => { S.hud.down('Left Click'); swipe = { card: S.front(), x: S.pointer.x }; });
  at(6.02, (S) => { S.hud.up('Left Click'); const card = swipe.card; swipe = null; exit(S, card, 0.9); });
  at(6.45, (S) => S.hud.hide());
  at(7.25, (S) => exit(S, S.front(), 0.86));
  return {
    events,
    path: [
      { t: 0, x: 330, y: 300 },
      { t: 2.3, x: 330, y: 300 },
      { t: 3.0, x: 146, y: 652, arc: -40 },
      { t: 3.12, x: 146, y: 652 },
      { t: 3.2, x: 152, y: 646, ease: 'linear' },
      { t: 4.3, x: 636, y: 712, arc: -36 },
      { t: 4.85, x: 638, y: 713 },
      { t: 5.6, x: 150, y: 650, arc: 60 },
      { t: 5.75, x: 150, y: 650 },
      { t: 6.0, x: 36, y: 660, ease: 'linear' },
      { t: 6.15, x: 30, y: 661, ease: 'out' },
      { t: 6.9, x: 470, y: 380, arc: -30 },
    ],
    cursors: [[0, 'text'], [2.7, 'arrow'], [6.75, 'text']],
    scroll: bursts([[0.75, 1.0, 0, 190], [1.28, 1.55, 190, 430]]),
    hud: [480, 300],
    tick: (S) => {
      carry(S, OVER_CHAT);
      if (swipe) swipe.card.slide.snap(Math.min(S.pointer.x - swipe.x, 0));
    },
  };
};

// Still frames for the banner.
SCENES.still = () => {
  const { events, at } = scenario();
  const q = new URLSearchParams(location.search);
  const deck = Number(q.get('deck') || 1);
  const regions = q.get('named') ? [...WINDOWS, R.scroll] : WINDOWS;
  for (let i = 0; i < deck; i++) at(0.1 + i * 0.3, (S) => S.capture(regions, 0));
  return {
    events,
    path: [{ t: 0, x: 1400, y: 900, hidden: true }],
    scroll: () => Number(q.get('scroll') || 0),
    setup: (S) => { if (q.get('named')) S.capsule.named.add('scroll'); },
  };
};
