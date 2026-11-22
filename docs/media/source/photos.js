// Procedural photographs and textures, so the demos carry no third-party images. Seeded, so every frame agrees.
function seeded(seed) {
  return () => {
    seed = (seed + 0x6d2b79f5) | 0;
    let t = Math.imul(seed ^ (seed >>> 15), 1 | seed);
    t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
    return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
  };
}

// Value noise on a 256 lattice. With a period it wraps, which makes tileable textures.
function valueNoise(seed) {
  const random = seeded(seed);
  const table = new Float32Array(256 * 256);
  for (let i = 0; i < table.length; i++) table[i] = random();
  return (x, y, period = 256) => {
    const xi = Math.floor(x), yi = Math.floor(y);
    const xf = x - xi, yf = y - yi;
    const u = xf * xf * (3 - 2 * xf), v = yf * yf * (3 - 2 * yf);
    const at = (a, b) => table[(((b % period) + period) % period) * 256 + (((a % period) + period) % period)];
    const top = at(xi, yi) + (at(xi + 1, yi) - at(xi, yi)) * u;
    const bottom = at(xi, yi + 1) + (at(xi + 1, yi + 1) - at(xi, yi + 1)) * u;
    return top + (bottom - top) * v;
  };
}

function fbm(noise, x, y, octaves = 5, period = 0) {
  let sum = 0, amp = 0.5, freq = 1, norm = 0;
  for (let o = 0; o < octaves; o++) {
    sum += amp * (period ? noise(x * freq, y * freq, period * freq) : noise(x * freq, y * freq));
    norm += amp; amp *= 0.5; freq *= 2;
  }
  return sum / norm;
}

const mix = (a, b, t) => a + (b - a) * t;
const mix3 = (a, b, t) => [mix(a[0], b[0], t), mix(a[1], b[1], t), mix(a[2], b[2], t)];
const sstep = (a, b, x) => { const t = Math.min(Math.max((x - a) / (b - a), 0), 1); return t * t * (3 - 2 * t); };

// Paints with `shade(x, y) -> [r, g, b]`, then adds grain and a soft vignette, as a camera would.
function paint(w, h, seed, shade, { grain = 7, vignette = 0.18 } = {}) {
  const canvas = document.createElement('canvas');
  canvas.width = w; canvas.height = h;
  const ctx = canvas.getContext('2d');
  const image = ctx.createImageData(w, h);
  const random = seeded(seed * 31 + 5);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const c = shade(x, y);
      const dx = x / w - 0.5, dy = y / h - 0.5;
      const v = 1 - vignette * Math.min((dx * dx + dy * dy) * 2.4, 1);
      const g = (random() - 0.5) * grain;
      const i = (y * w + x) * 4;
      image.data[i] = c[0] * v + g;
      image.data[i + 1] = c[1] * v + g;
      image.data[i + 2] = c[2] * v + g;
      image.data[i + 3] = 255;
    }
  }
  ctx.putImageData(image, 0, 0);
  return canvas;
}

// A ridge line: fractal noise along x, plus pine trees for the nearer ridges.
function ridge(noise, w, base, amp, scale, trees, seed) {
  const line = new Float32Array(w);
  for (let x = 0; x < w; x++) line[x] = base - amp * fbm(noise, x / scale + seed * 13.1, seed * 7.7, 5);
  if (trees) {
    const random = seeded(seed * 97);
    const tops = new Float32Array(w);
    for (let c = 0; c < w; c += 2 + random() * 5) {
      const height = trees * (0.45 + random() * 0.7);
      const half = height * (0.22 + random() * 0.08);
      for (let x = Math.max(0, Math.floor(c - half)); x < Math.min(w, Math.ceil(c + half)); x++) {
        tops[x] = Math.max(tops[x], height * (1 - Math.abs(x - c) / half));
      }
    }
    for (let x = 0; x < w; x++) line[x] -= tops[x];
  }
  return line;
}

// Mountains over a lake at first light, with mist between the ridges and their reflection in the water.
function photoLake(w, h, seed = 3) {
  const noise = valueNoise(seed);
  const horizon = Math.round(h * 0.64);
  const layers = [
    { line: ridge(noise, w, horizon - h * 0.16, h * 0.34, w * 0.34, 0, 1), color: [156, 166, 184], haze: 0.62 },
    { line: ridge(noise, w, horizon - h * 0.06, h * 0.24, w * 0.22, 0, 2), color: [116, 128, 140], haze: 0.42 },
    { line: ridge(noise, w, horizon - h * 0.01, h * 0.14, w * 0.16, h * 0.035, 3), color: [70, 84, 86], haze: 0.2 },
    { line: ridge(noise, w, horizon + h * 0.005, h * 0.07, w * 0.1, h * 0.06, 4), color: [36, 47, 44], haze: 0.05 },
  ];
  const sky = (x, y) => {
    const t = y / horizon;
    let c = t < 0.55 ? mix3([104, 134, 170], [196, 186, 190], t / 0.55) : mix3([196, 186, 190], [248, 214, 178], (t - 0.55) / 0.45);
    const sx = x / w - 0.7, sy = y / h - 0.5;
    const glow = Math.exp(-(sx * sx * 5 + sy * sy * 22));
    c = mix3(c, [255, 236, 206], glow * 0.75);
    const cloud = sstep(0.52, 0.78, fbm(noise, x / 160 + 40, y / 46, 5)) * (1 - t * 0.7);
    return mix3(c, [250, 228, 214], cloud * 0.55);
  };
  const land = (x, y) => {
    let color = sky(x, y);
    for (const layer of layers) {
      const top = layer.line[x];
      if (y < top) continue;
      const depth = Math.min((y - top) / (h * 0.25), 1);
      const tex = 0.86 + 0.24 * fbm(noise, x / 26 + layer.haze * 50, y / 18, 4);
      let c = layer.color.map((v) => v * tex * (1 - depth * 0.18));
      const mist = layer.haze + (1 - layer.haze) * sstep(horizon - h * 0.12, horizon, y) * 0.35;
      c = mix3(c, sky(x, y), Math.min(mist, 0.85));
      color = c;
    }
    return color;
  };
  const upper = [];
  for (let y = 0; y < horizon; y++) { const row = []; for (let x = 0; x < w; x++) row.push(land(x, y)); upper.push(row); }
  return paint(w, h, seed, (x, y) => {
    if (y < horizon) return upper[y][x];
    const d = y - horizon;
    const wobble = Math.sin(d * 0.55 + fbm(noise, x / 30, d / 4, 3) * 6) * (1 + d * 0.035);
    const my = Math.max(0, Math.min(horizon - 1, Math.round(horizon - 1 - d * 1.05)));
    const mx = Math.max(0, Math.min(w - 1, Math.round(x + wobble)));
    let c = upper[my][mx].map((v) => v * 0.78);
    c = mix3(c, [64, 86, 104], 0.22 + d / h * 0.4);
    const streak = sstep(0.62, 0.8, fbm(noise, x / 90, d / 2.2 + 300, 3)) * 0.25;
    return mix3(c, [240, 222, 204], streak);
  });
}

// Pines dissolving into fog, layer behind layer.
function photoForest(w, h, seed = 9) {
  const noise = valueNoise(seed);
  const layers = [0, 1, 2, 3, 4].map((i) => ({
    line: ridge(noise, w, h * (0.42 + i * 0.12), h * 0.12, w * 0.3, h * (0.16 + i * 0.05), 10 + i),
    shade: 150 - i * 30, haze: 0.78 - i * 0.18,
  }));
  return paint(w, h, seed, (x, y) => {
    let c = mix3([214, 220, 222], [190, 200, 202], y / h);
    for (const layer of layers) {
      if (y < layer.line[x]) continue;
      const tex = 0.9 + 0.18 * fbm(noise, x / 9, y / 22, 3);
      const base = [layer.shade * 0.62 * tex, layer.shade * 0.74 * tex, layer.shade * 0.7 * tex];
      const fog = layer.haze + 0.25 * fbm(noise, x / 120 + layer.shade, y / 40, 4);
      c = mix3(base, [214, 220, 222], Math.min(Math.max(fog, 0), 0.92));
    }
    return c;
  }, { grain: 8, vignette: 0.12 });
}

// Wind-shaped dunes in late sun.
function photoDunes(w, h, seed = 5) {
  const noise = valueNoise(seed);
  const bands = [0.38, 0.55, 0.74].map((b, i) => ({
    crest: (x) => h * b - h * 0.08 * Math.sin(x / w * (2.4 + i) + i * 1.7) - h * 0.04 * fbm(noise, x / 140 + i * 9, i, 4),
    tilt: 0.6 + i * 0.25, light: [226 - i * 6, 160 - i * 8, 104 - i * 8], dark: [158 - i * 14, 92 - i * 10, 62 - i * 8],
  }));
  return paint(w, h, seed, (x, y) => {
    let c = mix3([150, 184, 214], [236, 214, 190], y / (h * 0.45));
    for (const band of bands) {
      const crest = band.crest(x);
      if (y < crest) continue;
      const edge = crest + (x - w * 0.5) * 0.18 * band.tilt + h * 0.06;
      const lit = sstep(edge - 3, edge + 3, y);
      const ripple = Math.sin(x * 0.11 + y * 0.42 + fbm(noise, x / 50, y / 30, 3) * 9) * 7;
      const base = mix3(band.dark, band.light, lit);
      c = base.map((v) => v + ripple * (0.4 + lit * 0.6));
    }
    return c;
  }, { grain: 9, vignette: 0.2 });
}

// Out-of-focus city lights at dusk.
function photoBokeh(w, h, seed = 21) {
  const random = seeded(seed);
  const lights = Array.from({ length: 46 }, () => ({
    x: random() * w, y: random() * h * 0.9, r: 8 + random() * 34,
    c: [[255, 196, 120], [255, 232, 196], [120, 200, 210], [255, 150, 96]][Math.floor(random() * 4)], a: 0.25 + random() * 0.55,
  }));
  return paint(w, h, seed, (x, y) => {
    let c = mix3([38, 30, 40], [70, 52, 54], y / h);
    for (const l of lights) {
      const d = Math.hypot(x - l.x, y - l.y) / l.r;
      if (d > 1.1) continue;
      const disc = (1 - sstep(0.86, 1.0, d)) * (0.75 + 0.25 * d) * l.a;
      c = c.map((v, i) => v + (l.c[i] - v * 0.3) * disc * 0.6);
    }
    return c;
  }, { grain: 10, vignette: 0.3 });
}

// A calm sea under a high, bright overcast.
function photoSea(w, h, seed = 13) {
  const noise = valueNoise(seed);
  const horizon = h * 0.46;
  return paint(w, h, seed, (x, y) => {
    if (y < horizon) {
      const t = y / horizon;
      const cloud = fbm(noise, x / 220, y / 70, 5);
      return mix3(mix3([150, 170, 186], [226, 228, 226], t), [244, 242, 238], sstep(0.45, 0.8, cloud) * 0.5);
    }
    const d = (y - horizon) / (h - horizon);
    const wave = fbm(noise, x / (70 - d * 40), y / (3 + d * 10), 4);
    const base = mix3([140, 164, 176], [46, 84, 100], Math.pow(d, 0.7));
    return mix3(base, [222, 230, 232], sstep(0.6, 0.85, wave) * (0.5 - d * 0.3));
  }, { grain: 7, vignette: 0.16 });
}

// A seamless topographic pattern, the fixed background of the article page.
function topoTile(size, seed = 17) {
  const noise = valueNoise(seed);
  const period = 4;
  const field = (x, y) => fbm(noise, (x / size) * period, (y / size) * period, 4, period) * 14;
  return paint(size, size, seed, (x, y) => {
    const v = field(x, y);
    const gx = field(x + 0.5, y) - field(x - 0.5, y), gy = field(x, y + 0.5) - field(x, y - 0.5);
    const width = Math.hypot(gx, gy) || 1e-3;
    const f = v - Math.floor(v);
    const line = 1 - sstep(0.55, 1.35, Math.min(f, 1 - f) / width);
    const major = Math.floor(v) % 4 === 0 ? 1 : 0.55;
    return mix3([238, 233, 224], [208, 197, 180], line * major);
  }, { grain: 3, vignette: 0 });
}

const PHOTOS = {};
function makePhotos() {
  const url = (canvas) => canvas.toDataURL('image/jpeg', 0.92);
  PHOTOS.lake = url(photoLake(1120, 620));
  PHOTOS.forest = url(photoForest(560, 400));
  PHOTOS.dunes = url(photoDunes(560, 400));
  PHOTOS.bokeh = url(photoBokeh(560, 400));
  PHOTOS.sea = url(photoSea(560, 400));
  PHOTOS.topo = topoTile(320).toDataURL('image/png');
}
