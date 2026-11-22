// The desktop the demos run on: a browser with a long article on the left and a team chat on the right, snapped
// side by side above the taskbar. All names, people, and text are made up.
const VIEW_TOP = 108, VIEW_H = 644;

const ink = (c) => ({ stroke: c, fill: 'none', 'stroke-width': 1.3, 'stroke-linecap': 'round', 'stroke-linejoin': 'round' });
const attrs = (o) => Object.entries(o).map(([k, v]) => `${k}="${v}"`).join(' ');
const svg = (w, h, body, view) => `<svg width="${w}" height="${h}" viewBox="${view || `0 0 ${w} ${h}`}">${body}</svg>`;
const path = (d, c = '#3c4043', extra = {}) => `<path d="${d}" ${attrs({ ...ink(c), ...extra })}/>`;

const I = {
  min: svg(10, 10, path('M0 5.5h10', '#1f2328', { 'stroke-width': 1 })),
  max: svg(10, 10, '<rect x=".5" y=".5" width="9" height="9" rx="1.5" fill="none" stroke="#1f2328"/>'),
  close: svg(10, 10, path('M.5.5l9 9M9.5.5l-9 9', '#1f2328', { 'stroke-width': 1 })),
  x: svg(10, 10, path('M2 2l6 6M8 2l-6 6', '#5f6368', { 'stroke-width': 1.2 })),
  plus: (c = '#3c4043') => svg(14, 14, path('M7 2v10M2 7h10', c)),
  back: svg(16, 16, path('M13 8H3.5M7.5 3.5L3 8l4.5 4.5')),
  fwd: svg(16, 16, path('M3 8h9.5M8.5 3.5L13 8l-4.5 4.5', '#b9bcc1')),
  reload: svg(16, 16, path('M13 8a5 5 0 1 1-1.6-3.7M13 2.5v3.2H9.8')),
  tune: svg(12, 12, path('M2 3.5h5M9.5 3.5H10M2 8.5h1M5.5 8.5H10', '#3c4043', { 'stroke-width': 1.2 }) + '<circle cx="8.2" cy="3.5" r="1.4" fill="none" stroke="#3c4043" stroke-width="1.2"/><circle cx="4.3" cy="8.5" r="1.4" fill="none" stroke="#3c4043" stroke-width="1.2"/>'),
  star: svg(16, 16, path('M8 2.2l1.8 3.7 4 .6-2.9 2.8.7 4L8 11.4l-3.6 1.9.7-4L2.2 6.5l4-.6z')),
  puzzle: svg(16, 16, path('M3 5.5h2.2a1.6 1.6 0 1 1 3.2 0h2.1v2.1a1.6 1.6 0 1 1 0 3.2v2.2H3z')),
  dots: svg(16, 16, '<circle cx="3.5" cy="8" r="1.2" fill="#3c4043"/><circle cx="8" cy="8" r="1.2" fill="#3c4043"/><circle cx="12.5" cy="8" r="1.2" fill="#3c4043"/>'),
  folder: (c = '#e0a82e') => svg(14, 14, `<path d="M1.5 3.5a1 1 0 0 1 1-1h3l1.3 1.4h4.7a1 1 0 0 1 1 1V11a1 1 0 0 1-1 1h-9a1 1 0 0 1-1-1z" fill="${c}"/>`),
  search: (c = '#8b8a96', s = 14) => svg(s, s, `<circle cx="${s * 0.43}" cy="${s * 0.43}" r="${s * 0.3}" fill="none" stroke="${c}" stroke-width="1.3"/>` + path(`M${s * 0.66} ${s * 0.66}l${s * 0.24} ${s * 0.24}`, c), `0 0 ${s} ${s}`),
  bookmark: svg(16, 16, path('M4.5 2.5h7v11L8 10.8l-3.5 2.7z', '#6b6c70')),
  share: svg(16, 16, path('M8 2.5v8M4.8 5.6L8 2.5l3.2 3.1M3 9.5v3a1 1 0 0 0 1 1h8a1 1 0 0 0 1-1v-3', '#6b6c70')),
  chat: (c) => svg(18, 18, path('M3 4.5A2.5 2.5 0 0 1 5.5 2h7A2.5 2.5 0 0 1 15 4.5v5a2.5 2.5 0 0 1-2.5 2.5H8l-3.5 3v-3A2.5 2.5 0 0 1 3 9.5z', c)),
  home: (c) => svg(18, 18, path('M3 8l6-5 6 5v7H11v-4H7v4H3z', c)),
  bell: (c) => svg(18, 18, path('M5 12.5V8a4 4 0 0 1 8 0v4.5l1.3 1.5H3.7zM7.5 15.5a1.6 1.6 0 0 0 3 0', c)),
  dm: (c) => svg(18, 18, path('M2.5 4.5h13v9h-13zM2.5 5l6.5 5 6.5-5', c)),
  more: (c) => svg(18, 18, `<circle cx="4" cy="9" r="1.3" fill="${c}"/><circle cx="9" cy="9" r="1.3" fill="${c}"/><circle cx="14" cy="9" r="1.3" fill="${c}"/>`),
  thread: (c) => svg(14, 14, path('M2 3h10v6H6l-3 2.5V9H2z', c, { 'stroke-width': 1.2 })),
  at: (c) => svg(14, 14, path('M9.3 7a2.3 2.3 0 1 1-4.6 0a2.3 2.3 0 0 1 4.6 0v.8a1.6 1.6 0 0 0 3.2 0V7a5.5 5.5 0 1 0-2.2 4.4', c, { 'stroke-width': 1.2 })),
  draft: (c) => svg(14, 14, path('M2.5 11.5l1-3 6.5-6.5 2 2-6.5 6.5zM8.5 3.5l2 2', c, { 'stroke-width': 1.2 })),
  compose: svg(16, 16, path('M10.5 2.5l3 3L6 13H3v-3z', '#55545f')),
  caret: svg(10, 10, path('M2.5 3.8L5 6.3l2.5-2.5', '#55545f')),
  hash: (c = '#6d6c78') => svg(14, 14, path('M5.5 1.8L4.3 12.2M9.7 1.8L8.5 12.2M2.2 5h10M1.8 9h10', c, { 'stroke-width': 1.1 })),
  starSmall: svg(14, 14, path('M7 2l1.5 3 3.3.5-2.4 2.3.6 3.3L7 9.5l-3 1.6.6-3.3L2.2 5.5l3.3-.5z', '#8b8a96', { 'stroke-width': 1.1 })),
  call: svg(16, 16, path('M5.2 2.5l1.6 3-1.3 1.4a8 8 0 0 0 3.6 3.6l1.4-1.3 3 1.6-.7 2.3c-5.3.6-10.2-4.3-9.6-9.6z', '#55545f', { 'stroke-width': 1.2 })),
  info: svg(16, 16, '<circle cx="8" cy="8" r="6" fill="none" stroke="#55545f" stroke-width="1.2"/>' + path('M8 7.2v4M8 5v.2', '#55545f', { 'stroke-width': 1.4 })),
  bold: svg(12, 12, path('M3.5 2h3.3a2.2 2.2 0 0 1 0 4.4H3.5zM3.5 6.4h3.8a2.3 2.3 0 0 1 0 4.6H3.5z', '#6d6c78', { 'stroke-width': 1.4 })),
  italic: svg(12, 12, path('M7.5 2L4.5 10.5M5 2h4.5M2.5 10.5H7', '#6d6c78', { 'stroke-width': 1.2 })),
  strike: svg(12, 12, path('M2 6.2h8M8.6 3.6c-.4-1-1.4-1.6-2.6-1.6-1.6 0-2.6.8-2.6 2 0 2.6 5.6 1.6 5.6 4.2 0 1.2-1.2 2-2.8 2-1.3 0-2.4-.6-2.8-1.6', '#6d6c78', { 'stroke-width': 1.1 })),
  link: svg(12, 12, path('M5 7.2a2.2 2.2 0 0 0 3.1 0l1.8-1.8a2.2 2.2 0 0 0-3.1-3.1l-.6.6M7 4.8a2.2 2.2 0 0 0-3.1 0L2.1 6.6a2.2 2.2 0 0 0 3.1 3.1l.6-.6', '#6d6c78', { 'stroke-width': 1.1 })),
  list: svg(12, 12, path('M4.5 3h6M4.5 6h6M4.5 9h6M1.8 3h.2M1.8 6h.2M1.8 9h.2', '#6d6c78', { 'stroke-width': 1.2 })),
  code: svg(12, 12, path('M4 3L1.5 6 4 9M8 3l2.5 3L8 9', '#6d6c78', { 'stroke-width': 1.2 })),
  plusRound: svg(14, 14, '<circle cx="7" cy="7" r="6" fill="#efeef4"/>' + path('M7 4v6M4 7h6', '#55545f', { 'stroke-width': 1.2 })),
  emoji: svg(14, 14, '<circle cx="7" cy="7" r="5.6" fill="none" stroke="#6d6c78" stroke-width="1.1"/><circle cx="5" cy="5.8" r=".7" fill="#6d6c78"/><circle cx="9" cy="5.8" r=".7" fill="#6d6c78"/>' + path('M4.6 8.3a2.8 2.8 0 0 0 4.8 0', '#6d6c78', { 'stroke-width': 1.1 })),
  clip: svg(14, 14, path('M9.5 4.5L5.2 8.8a1.3 1.3 0 0 0 1.8 1.8l4.6-4.6a2.5 2.5 0 0 0-3.5-3.5L3.4 7.2a3.6 3.6 0 0 0 5.1 5.1L12 8.8', '#6d6c78', { 'stroke-width': 1.1 })),
  send: svg(14, 14, path('M2 7.2L12.5 2.5 9.2 12.3 7.3 8.3z', '#a7a6b3', { 'stroke-width': 1.1 })),
  image: svg(14, 14, '<rect x="1.5" y="2.5" width="11" height="9" rx="1.6" fill="none" stroke="#6b6a78" stroke-width="1.1"/><circle cx="5" cy="5.6" r="1" fill="#6b6a78"/>' + path('M2 10.5l3.2-3 2.3 2.1 1.8-1.6 3.2 2.9', '#6b6a78', { 'stroke-width': 1.1 })),
  play: svg(12, 12, '<path d="M4 2.5v7l6-3.5z" fill="#1d1e21"/>'),
  upload: svg(22, 22, path('M11 15V5M6.5 9.5L11 5l4.5 4.5M4 17.5h14', '#fff', { 'stroke-width': 1.8 })),
};

function face(initials, color, cls = 'face') { return `<div class="${cls}" style="background:${color}">${initials}</div>`; }

function chart() {
  const rows = [['Notification, then editor', 3.4, '#cfc8bb'], ['Floating thumbnail', 1.1, '#cfc8bb'], ['Thumbnail and selector', 0.7, '#b5532f']];
  const x0 = 150, w = 270, scale = w / 4;
  let s = '';
  for (let t = 0; t <= 4; t++) {
    const x = x0 + t * scale;
    s += `<line x1="${x}" y1="4" x2="${x}" y2="104" stroke="${t ? '#efece6' : '#cfc8bb'}" stroke-width="1"/>`;
    s += `<text x="${x}" y="120" text-anchor="middle" font-family="Segoe UI Variable Text" font-size="10.5" fill="#919297">${t} s</text>`;
  }
  rows.forEach(([label, v, c], i) => {
    const y = 12 + i * 32;
    s += `<text x="0" y="${y + 13}" font-family="Segoe UI Variable Text" font-size="12" fill="#4a4b50">${label}</text>`;
    s += `<rect x="${x0}" y="${y}" width="${v * scale}" height="18" rx="3" fill="${c}"/>`;
    s += `<text x="${x0 + v * scale + 6}" y="${y + 13}" font-family="Segoe UI Variable Text" font-size="11.5" font-weight="600" fill="#26272b">${v.toFixed(1)}</text>`;
  });
  return svg(440, 126, s);
}

function articleHTML() {
  return `
  <div class="card">
    <img class="heroimg" src="${PHOTOS.lake}">
    <div class="article">
      <div class="crumbs"><span class="chip">ESSAY</span>Journal&nbsp;›&nbsp;Design&nbsp;·&nbsp;Issue 41</div>
      <h1>Designing for the second glance</h1>
      <div class="dek">Notes on calm software, careful motion, and the tools that step aside the moment real work begins.</div>
      <div class="byline">${face('ML', '#a07a54')}<div class="who"><b>Mara Lindqvist</b><span class="follow">Follow</span><br>Nov 18, 2026 · 8 min read</div>
        <div class="acts"><span class="ib">${I.bookmark}</span><span class="ib">${I.share}</span></div></div>
      <p class="dropcap">The first glance is where most software spends its effort: the splash screen, the onboarding, the confident hero. The second glance is quieter. It happens a hundred times a day, half-attended, between one thought and the next, and it is where a tool either earns its place or slowly wears it out.</p>
      <p>We spent a season studying that second glance. We sat with designers, support leads, and engineers while they captured, filed, and passed along small pieces of their screens, and we noticed that the best moments were the ones nobody remembered afterward.</p>
      <div class="fig"><img src="${PHOTOS.forest}"><div class="figcap"><span>Morning fog over the ridge behind the studio, where most of these notes were written.</span><span>Photo: Ines&nbsp;Okafor</span></div></div>
      <p>None of the tools people loved were clever. They were quick, predictable, and polite: they answered immediately, never asked a question they could answer themselves, and left as soon as the job was done.</p>
      <div class="pull">Good motion doesn't decorate a change. It answers the question the change just raised.</div>
      <h2>Motion that explains itself</h2>
      <p>When something leaves the screen, the eye wants to know where it went. A thumbnail that shrinks into a corner says more than a notification can: it shows the result, where it lives now, and that nothing else needs your attention.</p>
      <p>We tuned every curve by feel first and numbers second. A little overshoot reads as alive; too much reads as a toy. The difference is a few hundredths of a second, and it is the whole difference.</p>
      <div class="chart"><div class="ct">Time from capture to first action</div><div class="cs">Median across 48 sessions, lower is better</div>${chart()}<div class="src">Source: Northwind field study, autumn 2026</div></div>
      <h2>The corner of the screen</h2>
      <p>Corners are the calmest places on a display. Nothing important lives there, so a small card can wait without competing with the work. It stays long enough to be useful, longer if you hover over it, and then it simply slides away.</p>
      <p>From that corner, everything else is a gesture away: drag it into a conversation, swipe it aside, or open it full size. There is no editor to dismiss and no file name to invent.</p>
      <div class="box"><div class="bt">WHAT WE TOOK AWAY</div><ul>
        <li>Answer in the first frame, then do the slow work.</li><li>Let the result wait where it is easy to reach and easy to ignore.</li><li>Every setting removed is a question nobody has to answer.</li></ul></div>
      <div class="pair"><img src="${PHOTOS.bokeh}"><img src="${PHOTOS.sea}"></div>
      <div class="cap2">Left: the city on the walk home. Right: the view we kept returning to.</div>
      <h2>What we left out</h2>
      <p>Every feature we removed made the remaining ones easier to trust. No tray icon. No settings page with forty switches. No sound. The fewer surfaces a tool has, the fewer ways it has to interrupt.</p>
      <p>The result feels less like an application and more like a property of the system, the way a good hinge feels like part of the door.</p>
      <div class="tags"><span>Interaction design</span><span>Motion</span><span>Research</span></div>
      <div class="bio">${face('ML', '#a07a54')}<div><div class="bn">Mara Lindqvist</div><div class="bd">Design lead at Northwind Studio. Writes about tools, attention, and the small decisions that add up to calm software.</div></div></div>
    </div>
    <div class="next"><div class="nt">READ NEXT</div><div class="cards">
      <div><img src="${PHOTOS.sea}"><div class="k">FIELD NOTES</div><div class="h">A week without notifications</div><div class="m">6 min read</div></div>
      <div><img src="${PHOTOS.forest}"><div class="k">ESSAY</div><div class="h">The case for fewer settings</div><div class="m">9 min read</div></div>
      <div><img src="${PHOTOS.bokeh}"><div class="k">INTERVIEW</div><div class="h">Building tools that wait</div><div class="m">12 min read</div></div>
    </div></div>
  </div>
  <div class="news"><div class="nl">One essay a week, nothing else.<small>Join 18,000 readers. Unsubscribe any time.</small></div><div class="in">you@example.com</div><div class="go">Subscribe</div></div>
  <div class="sitefoot"><span>© 2026 Northwind Journal</span><span>Privacy · Terms · RSS</span></div>`;
}

function siteBar() {
  return `<div class="sitebar"><div class="brand"><div class="mark">N</div><span>North<i>wind</i></span></div>
    <div class="nav"><span>Journal</span><span>Studio</span><span>Archive</span></div><span class="ib">${I.search('#3c4043', 16)}</span><div class="subscribe">Subscribe</div></div>`;
}
const LISTEN = `<div class="listen"><div class="play">${I.play}</div>Listen<small>8 min</small></div>`;

// The article as a scrolling screenshot keeps it: the pattern runs on unbroken, the site bar appears once at the
// top, and the floating player once at the bottom, where the screen last showed it.
function buildLongPage() {
  const el = document.createElement('div');
  el.style.cssText = `position:absolute;left:0;top:0;width:640px;height:6000px;background:url(${PHOTOS.topo}) 0 0/320px 320px`;
  const page = document.createElement('div');
  page.className = 'page';
  page.innerHTML = `<div style="height:56px"></div>${articleHTML()}`;
  el.appendChild(page);
  el.insertAdjacentHTML('beforeend', siteBar().replace('class="sitebar"', 'class="sitebar" style="position:absolute"') + '<div class="progress" style="width:0"></div>' + LISTEN);
  const listen = el.querySelector('.listen');
  listen.style.bottom = 'auto';
  return { el, setRows(rows) { listen.style.top = `${rows - 20 - 40}px`; } };
}

const MESSAGES = [
  ['LH', '#b8893f', 'Lena Hart', '9:12 AM', 'Standup moved to 10:30. Agenda: the launch checklist, pricing page copy, and who owns the changelog this week.',
    '<div class="reacts"><span class="react">👍 4</span><span class="react">🗓️ 1</span></div>'],
  ['MS', '#8a63a8', 'Mateo Silva', '9:41 AM', 'Build 214 is on staging: <a>staging.northwind.example</a>',
    '<div class="preview"><div><div class="ps">Northwind staging</div><div class="pt">Onboarding, round three</div><div class="pd">Skips the empty state when you already have a workspace.</div></div></div>'],
  ['IO', '#c4643f', 'Ines Okafor', '9:58 AM', 'Release notes are in the doc. I tightened the intro and cut the FAQ down to five questions.',
    `<div class="thread"><span class="s"><span style="background:#3e7cb1">TB</span><span style="background:#4d9a6a">PR</span></span>4 replies <small>Last reply today at 10:02 AM</small></div>`],
  ['TB', '#3e7cb1', 'Theo Brandt', '10:06 AM', "Still torn on the launch post layout. Could someone grab the Northwind piece? I'd like to compare their type scale with ours."],
  ['PR', '#4d9a6a', 'Priya Raman', '10:11 AM', 'Their headline and body pairing is lovely. The serif at <code>32px</code> over <code>15.5px</code> does a lot of work.',
    '<div class="reacts"><span class="react mine">❤️ 3</span><span class="react">💯 1</span></div>'],
];

function message([ini, color, who, time, text, extra]) {
  return `<div class="msg">${face(ini, color)}<div class="body"><div class="who">${who}<span>${time}</span></div><div class="txt">${text}</div>${extra ?? ''}</div></div>`;
}

function buildRelay() {
  const item = (icon, label, cls = '', extra = '') => `<div class="it ${cls}">${icon}${label}${extra}</div>`;
  const dm = (ini, color, name, online, extra = '') => `<div class="it ${extra ? 'bold' : ''}"><span class="mini" style="background:${color}">${ini}${online ? '<i style="background:#2bac76"></i>' : '<i style="background:#fff;border-color:#f2f1f6;box-shadow:inset 0 0 0 1.5px #8b8a96"></i>'}</span>${name}${extra}</div>`;
  return `
    <div class="titlebar"><div class="logo">${I.chat('#fff').replace('width="18" height="18"', 'width="12" height="12" viewBox="0 0 18 18"')}</div>Relay
      <div class="search">${I.search()}Search Northwind Studio</div></div>
    <div class="caption"><span>${I.min}</span><span>${I.max}</span><span>${I.close}</span></div>
    <div class="rail">
      <div class="ws on" style="background:#1d1e21;font-family:var(--serif-head);font-size:15px">N</div>
      <div class="ws" style="background:#d89a6a">A</div>
      <div class="ws" style="background:#e6e4ef;color:#6b6a78">${I.plus('#6b6a78')}</div>
      <div class="sep"></div>
      <div class="ws" style="background:#e2dff3">${I.home('#2b2560')}</div>
      <div class="ws">${I.dm('#55545f')}</div>
      <div class="ws">${I.bell('#55545f')}</div>
      <div class="ws">${I.more('#55545f')}</div>
      <div class="me">${face('AK', '#7b62c9', 'face" style="width:30px;height:30px;line-height:30px;border-radius:8px;font-size:10.5px;background:#7b62c9')}</div>
    </div>
    <div class="side">
      <div class="wsname">Northwind ${I.caret}<span class="grow"></span>${I.compose}</div>
      ${item(I.thread('#55545f'), 'Threads')}${item(I.at('#55545f'), 'Mentions &amp; reactions')}${item(I.draft('#55545f'), 'Drafts &amp; sent')}
      <div class="sec">${I.caret}Channels</div>
      ${item(I.hash(), 'announcements')}${item(I.hash('#1e1e26'), 'design', 'bold')}${item(I.hash('#2b2560'), 'launch-review', 'on')}${item(I.hash(), 'marketing')}${item(I.hash(), 'random')}
      <div class="sec">${I.caret}Direct messages</div>
      ${dm('IO', '#c4643f', 'Ines Okafor', true)}${dm('TB', '#3e7cb1', 'Theo Brandt', true)}${dm('PR', '#4d9a6a', 'Priya Raman', false)}${dm('MS', '#8a63a8', 'Mateo Silva', true, '<span class="n">2</span>')}
    </div>
    <div class="conv">
      <div class="conv-head"><span class="ch">#&nbsp;launch-review</span>${I.starSmall}<span class="grow"></span>
        <div class="stack"><span class="s"><span style="background:#c4643f">IO</span><span style="background:#3e7cb1">TB</span><span style="background:#4d9a6a">PR</span></span>12</div>
        <span class="ib">${I.call}</span><span class="ib">${I.info}</span></div>
      <div class="msgs"><div class="day"><span>Today</span></div>${MESSAGES.map(message).join('')}
        <div class="incoming" style="height:0;opacity:0">${message(['TB', '#3e7cb1', 'Theo Brandt', '10:23 AM', 'Only the browser window is needed, thanks!'])}</div>
        <div class="newwrap" style="height:0;opacity:0"><div class="msg">${face('AK', '#7b62c9')}<div class="body">
          <div class="who">Ari Kade<span>10:24 AM</span></div><div class="txt">Here's the article.</div>
          <div class="attach"><div class="shot"></div><div class="file">${I.image}Screenshot 2026-11-22 102412.png</div></div></div></div></div>
      </div>
      <div class="composer"><div class="tools"><span>${I.bold}</span><span>${I.italic}</span><span>${I.strike}</span><i></i><span>${I.link}</span><span>${I.list}</span><span>${I.code}</span></div>
        <div class="ph">Message #launch-review</div>
        <div class="row"><span>${I.plusRound}</span><span>${I.emoji}</span><span>${I.at('#6d6c78')}</span><span>${I.clip}</span><span class="grow"></span><span class="send">${I.send}</span></div></div>
      <div class="typing"><b>Theo Brandt</b> is typing<span class="dots">...</span></div>
      <div class="dropzone"><div class="dz"></div><div class="dt"><div class="di">${I.upload}</div>Share in #launch-review<small>Drop the file to send it to 12 people</small></div></div>
    </div>`;
}

function buildTaskbar() {
  const g = (body) => svg(22, 22, body);
  const apps = [
    ['', g('<g fill="#3a7bd5"><rect x="2" y="2" width="8.4" height="8.4" rx="1.4"/><rect x="11.6" y="2" width="8.4" height="8.4" rx="1.4"/><rect x="2" y="11.6" width="8.4" height="8.4" rx="1.4"/><rect x="11.6" y="11.6" width="8.4" height="8.4" rx="1.4"/></g>')],
    ['tsearch', `${I.search('#3d4654', 16)}<span>Search</span>`],
    ['', g('<rect x="2.5" y="5" width="11" height="12" rx="2" fill="none" stroke="#3d4654" stroke-width="1.5"/><rect x="9" y="3" width="10.5" height="12" rx="2" fill="#eef0f4" stroke="#3d4654" stroke-width="1.5"/>')],
    ['run', g('<path d="M2 6a2 2 0 0 1 2-2h4.6l2 2.2H18a2 2 0 0 1 2 2V17a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2z" fill="#e3a82b"/><path d="M2 9h18v8a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2z" fill="#f6c94f"/>')],
    ['focus run', g('<circle cx="11" cy="11" r="9.5" fill="#1d8a7f"/><path d="M11 1.5a9.5 9.5 0 0 1 0 19c3-2.4 4.4-5.8 4.4-9.5S14 3.9 11 1.5z" fill="#2fb3a3"/><path d="M5 14.5c2.8-.4 7-2.6 9.8-7" stroke="#eafbf8" stroke-width="1.8" fill="none" stroke-linecap="round"/>')],
    ['run', g('<rect x="1.5" y="1.5" width="19" height="19" rx="5.5" fill="#5b4fd8"/><path d="M6.4 7.8A1.7 1.7 0 0 1 8.1 6.1h5.8a1.7 1.7 0 0 1 1.7 1.7v3.9a1.7 1.7 0 0 1-1.7 1.7h-3.3L8.3 15.6v-2.2h-.2a1.7 1.7 0 0 1-1.7-1.7z" fill="#fff"/>')],
    ['', g('<rect x="3" y="1.5" width="16" height="19" rx="3.5" fill="#f08a3c"/><path d="M7 7.5h8M7 11h8M7 14.5h5" stroke="#fff" stroke-width="1.6" stroke-linecap="round"/>')],
    ['', g('<rect x="1.5" y="3" width="19" height="16" rx="3.5" fill="#25272d"/><path d="M6 8.5l2.8 2.6L6 13.7M10.5 14h5" stroke="#e8e9ec" stroke-width="1.5" fill="none" stroke-linecap="round" stroke-linejoin="round"/>')],
  ].map(([cls, body]) => `<div class="${cls}">${body}${cls.includes('run') ? '<i></i>' : ''}</div>`).join('');
  const weather = svg(28, 28, '<circle cx="11" cy="11" r="5.5" fill="#f5b83d"/><path d="M8 22h12.5a4.2 4.2 0 0 0 .4-8.4A6 6 0 0 0 9.6 15.6 3.3 3.3 0 0 0 8 22z" fill="#dfe6ef" stroke="#b9c3cf" stroke-width=".8"/>');
  const tray = `${svg(14, 14, path('M3.5 9L7 5.5 10.5 9', '#3d4654'))}<div class="lang">ENG<br>US</div>
    <div class="grp">${svg(16, 16, path('M1.5 6.2a9.5 9.5 0 0 1 13 0M3.8 8.8a6.2 6.2 0 0 1 8.4 0M6.1 11.3a2.9 2.9 0 0 1 3.8 0', '#3d4654') + '<circle cx="8" cy="13.4" r="1" fill="#3d4654"/>')}
      ${svg(16, 16, path('M2.5 6h2.5L8.5 3v10L5 10H2.5zM10.8 5.6a3.4 3.4 0 0 1 0 4.8M12.6 3.8a6 6 0 0 1 0 8.4', '#3d4654', { 'stroke-width': 1.2 }))}
      ${svg(22, 16, '<rect x="1.5" y="4.5" width="17" height="8" rx="2" fill="none" stroke="#3d4654" stroke-width="1.1"/><rect x="3" y="6" width="11" height="5" rx="1" fill="#3d4654"/><rect x="19" y="7" width="1.6" height="3" rx=".6" fill="#3d4654"/>')}</div>
    <div class="clock">10:24 AM<br>11/22/2026</div>${svg(16, 16, path('M4.5 11V7.5a3.5 3.5 0 0 1 7 0V11l1 1.3h-9zM6.8 14a1.3 1.3 0 0 0 2.4 0', '#3d4654', { 'stroke-width': 1.2 }))}`;
  return `<div class="widget">${weather}<div>14°C<small>Partly sunny</small></div></div><div class="tb-apps">${apps}</div><div class="tray">${tray}</div>`;
}

function buildBrowser() {
  const tab = (left, width, fav, title, on) => `<div class="tab ${on ? 'on' : ''}" style="left:${left}px;width:${width}px">${fav}<span class="title">${title}</span>${on ? I.x : ''}${on ? '' : '<span class="sep"></span>'}</div>`;
  const favN = '<div class="fav" style="background:#1d1e21;font-family:var(--serif-head);font-size:10px">N</div>';
  const favDoc = `<div class="fav" style="background:#2f6fd6">${svg(10, 10, path('M2.5 2.5h5M2.5 5h5M2.5 7.5h3', '#fff', { 'stroke-width': 1.2 }))}</div>`;
  const favCode = `<div class="fav" style="background:#24292f">${svg(10, 10, path('M3.5 2.5L1.5 5l2 2.5M6.5 2.5l2 2.5-2 2.5', '#fff', { 'stroke-width': 1.2 }))}</div>`;
  const mark = (c, t) => `<span><span class="fav" style="background:${c};width:14px;height:14px;font-size:8px;line-height:14px">${t}</span>`;
  return `
    <div class="tabstrip">${tab(6, 168, favN, 'Designing for the second glance · Northwind', true)}${tab(174, 150, favDoc, 'Q4 launch plan · Docs')}${tab(324, 150, favCode, 'Pull request #482 · northwind/web')}
      <div class="newtab" style="left:480px">${I.plus()}</div>
      <div class="caption"><span>${I.min}</span><span>${I.max}</span><span>${I.close}</span></div></div>
    <div class="toolbar"><span class="ib">${I.back}</span><span class="ib">${I.fwd}</span><span class="ib">${I.reload}</span>
      <div class="omni"><span class="site">${I.tune}</span><span><b>northwind.example</b>/journal/second-glance</span><span class="grow"></span><span class="ib" style="width:24px;height:24px">${I.star}</span></div>
      <span class="ib">${I.puzzle}</span><span class="ib"><div class="avatar-s" style="background:#7b62c9">AK</div></span><span class="ib">${I.dots}</span></div>
    <div class="bookmarks"><span>${I.folder()}Work</span>${mark('#2f6fd6', 'D')}Docs</span>${mark('#e2584c', 'M')}Mail</span>${mark('#24292f', 'G')}Repos</span>${mark('#5b4fd8', 'R')}Relay</span>${mark('#1d8a7f', 'A')}Analytics</span><span>${I.folder()}Reading</span></div>
    <div class="viewport"><div class="pagebg" style="background-image:url(${PHOTOS.topo})"></div>${siteBar()}<div class="progress"></div>${LISTEN}<div class="scrollbar"></div></div>`;
}

function buildDesktop() {
  const desk = document.createElement('div');
  desk.className = 'desk';
  desk.innerHTML = `<div class="wallpaper"></div><div class="win browser">${buildBrowser()}</div><div class="win relay">${buildRelay()}</div><div class="taskbar">${buildTaskbar()}</div>`;
  const viewport = desk.querySelector('.viewport');
  const page = document.createElement('div');
  page.className = 'page';
  page.innerHTML = `<div style="height:56px"></div>${articleHTML()}`;
  viewport.insertBefore(page, viewport.querySelector('.sitebar'));
  return desk;
}

function setScroll(desk, y) {
  const page = desk.querySelector('.viewport > .page');
  const total = page.offsetHeight;
  page.style.transform = `translateY(${-y}px)`;
  const h = VIEW_H * VIEW_H / total;
  const bar = desk.querySelector('.scrollbar');
  bar.style.height = `${h}px`;
  bar.style.top = `${60 + (y / (total - VIEW_H)) * (VIEW_H - 64 - h)}px`;
  desk.querySelector('.viewport > .progress').style.width = `${640 * Math.min(y / (total - VIEW_H), 1)}px`;
}
