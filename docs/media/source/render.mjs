// Renders the pages in this folder to PNG frames through headless Edge over the DevTools protocol, then encodes
// them with ffmpeg. Needs Node 22+, Microsoft Edge, and ffmpeg on PATH.
//   node render.mjs jobs.json [job-name]
// Frames and the browser profile go to %TEMP%\sstk-media. Outputs are written where each job's "out" points,
// relative to this folder.
import { spawn, execFileSync } from 'node:child_process';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { pathToFileURL, fileURLToPath } from 'node:url';

const EDGE = process.env.EDGE_PATH || 'C:/Program Files (x86)/Microsoft/Edge/Application/msedge.exe';
const PORT = 9333;
const here = path.dirname(fileURLToPath(import.meta.url));
const work = path.join(os.tmpdir(), 'sstk-media');

const jobs = JSON.parse(fs.readFileSync(path.resolve(here, process.argv[2] || 'jobs.json'), 'utf8'));
const only = process.argv[3];

const edge = spawn(EDGE, [
  '--headless=new', `--remote-debugging-port=${PORT}`, `--user-data-dir=${path.join(work, 'edge-profile')}`,
  '--hide-scrollbars', '--force-color-profile=srgb', '--no-first-run', '--no-default-browser-check', '--disable-extensions',
  '--allow-file-access-from-files', 'about:blank',
], { stdio: 'ignore' });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function waitForEdge() {
  for (let i = 0; i < 300; i++) {
    try {
      const list = await (await fetch(`http://127.0.0.1:${PORT}/json/list`)).json();
      const page = list.find((t) => t.type === 'page');
      if (page) return page.webSocketDebuggerUrl;
    } catch {}
    await sleep(100);
  }
  throw new Error('Edge did not start');
}

function connect(url) {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(url);
    let id = 0;
    const pending = new Map();
    const listeners = [];
    ws.onmessage = (event) => {
      const msg = JSON.parse(event.data);
      if (msg.id && pending.has(msg.id)) {
        const { ok, fail } = pending.get(msg.id);
        pending.delete(msg.id);
        msg.error ? fail(new Error(JSON.stringify(msg.error))) : ok(msg.result);
      } else if (msg.method) {
        for (const l of listeners) l(msg);
      }
    };
    ws.onopen = () => resolve({
      send: (method, params = {}) => new Promise((ok, fail) => {
        const n = ++id;
        pending.set(n, { ok, fail });
        ws.send(JSON.stringify({ id: n, method, params }));
      }),
      on: (fn) => listeners.push(fn),
      close: () => ws.close(),
    });
    ws.onerror = reject;
  });
}

async function evaluate(cdp, expression) {
  const r = await cdp.send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
  if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || JSON.stringify(r.exceptionDetails));
  return r.result.value;
}

async function load(cdp, job) {
  await cdp.send('Emulation.setDeviceMetricsOverride', { width: job.width, height: job.height, deviceScaleFactor: job.dpr ?? 2, mobile: false });
  await cdp.send('Emulation.setDefaultBackgroundColorOverride', job.transparent ? { color: { r: 0, g: 0, b: 0, a: 0 } } : {});
  const url = pathToFileURL(path.join(here, job.page)).href + (job.query ? `?${job.query}` : '');
  const loaded = new Promise((r) => cdp.on((m) => { if (m.method === 'Page.loadEventFired') r(); }));
  await cdp.send('Page.navigate', { url });
  await loaded;
  await evaluate(cdp, 'window.ready');
}

async function shot(cdp, file, job) {
  const params = { format: 'png', fromSurface: true, captureBeyondViewport: false };
  if (job.clip) params.clip = { ...job.clip, scale: 1 };
  const { data } = await cdp.send('Page.captureScreenshot', params);
  fs.mkdirSync(path.dirname(file), { recursive: true });
  fs.writeFileSync(file, Buffer.from(data, 'base64'));
}

function ffmpeg(args) {
  execFileSync('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-y', ...args], { stdio: 'inherit' });
}

function encode(job, dir) {
  const fade = job.loopFade ?? 0;
  const inputs = ['-framerate', String(job.fps), '-i', path.join(dir, 'f%04d.png')];
  let graph = '[0]format=rgb24[v]';
  if (fade > 0) {
    inputs.push('-loop', '1', '-framerate', String(job.fps), '-t', String(fade + 0.1), '-i', path.join(dir, 'f0000.png'));
    graph = `[0]format=rgb24[a];[1]format=rgb24[b];[a][b]xfade=transition=fade:duration=${fade}:offset=${(job.duration - fade).toFixed(3)}[v]`;
  }
  const out = path.resolve(here, job.out);
  fs.mkdirSync(path.dirname(out), { recursive: true });
  if (out.endsWith('.mp4')) {
    // H.264 with a silent stereo track, which Instagram and most players expect.
    const audio = inputs.filter((a) => a === '-i').length;
    ffmpeg([...inputs, '-f', 'lavfi', '-t', String(job.duration), '-i', 'anullsrc=r=48000:cl=stereo', '-filter_complex',
      `${graph};[v]scale=${job.outWidth}:-2:flags=lanczos:out_color_matrix=bt709:out_range=tv,format=yuv420p[o]`,
      '-map', '[o]', '-map', `${audio}:a`, '-c:v', 'libx264', '-preset', 'slow', '-crf', '15', '-profile:v', 'high', '-r', String(job.fps),
      '-colorspace', 'bt709', '-color_primaries', 'bt709', '-color_trc', 'bt709', '-c:a', 'aac', '-b:a', '128k', '-shortest', '-movflags', '+faststart', out]);
    console.log(`  -> ${job.out} ${(fs.statSync(out).size / 1048576).toFixed(2)} MB`);
    return;
  }
  const scale = `${job.outFps ? `fps=${job.outFps},` : ''}scale=${job.outWidth}:-1:flags=lanczos`;
  ffmpeg([...inputs, '-filter_complex',
    `${graph};[v]${scale},split[s0][s1];[s0]palettegen=max_colors=256:stats_mode=${job.stats ?? 'full'}[p];[s1][p]paletteuse=dither=${job.dither ?? 'none'}:diff_mode=rectangle`,
    '-loop', '0', out]);
  console.log(`  -> ${job.out} ${(fs.statSync(out).size / 1048576).toFixed(2)} MB`);
}

async function run() {
  const cdp = await connect(await waitForEdge());
  await cdp.send('Page.enable');
  await cdp.send('Runtime.enable');
  for (const job of jobs) {
    if (only && job.name !== only) continue;
    const started = Date.now();
    if (job.kind === 'still') {
      await load(cdp, job);
      if (job.time !== undefined) await evaluate(cdp, `frame(${job.time})`);
      if (job.eval) console.log(JSON.stringify(await evaluate(cdp, job.eval), null, 1));
      await shot(cdp, path.resolve(here, job.out), job);
      console.log(`${job.name}: ${job.out} (${Date.now() - started} ms)`);
      continue;
    }
    const dir = path.join(work, 'frames', job.name);
    if (!job.encodeOnly) {
      await load(cdp, job);
      fs.rmSync(dir, { recursive: true, force: true });
      fs.mkdirSync(dir, { recursive: true });
      const count = Math.round(job.duration * job.fps);
      for (let i = 0; i < count; i++) {
        await evaluate(cdp, `frame(${(i / job.fps).toFixed(5)})`);
        await shot(cdp, path.join(dir, `f${String(i).padStart(4, '0')}.png`), job);
      }
      console.log(`${job.name}: ${count} frames (${Date.now() - started} ms)`);
    }
    if (job.out) encode(job, dir);
  }
  cdp.close();
}

run().then(() => { edge.kill(); process.exit(0); }, (e) => { console.error(e); edge.kill(); process.exit(1); });
