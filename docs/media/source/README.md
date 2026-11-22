# README media

These pages render every image and GIF in `docs/media`. Nothing here is part of the program or its build.

- `stage.html` with `engine.js` is a replica of the overlay: the thumbnail, the region bar, the rubbish bin, and dragging. It uses the springs, sizes, and colors from `include/sstk/config` and the layout rules in `src/overlay`, and runs on a simulated clock, so the same frame always comes out the same.
- `desktop.js` builds the desktop it runs on, and `photos.js` paints its photographs and the page's background pattern in code. The people, sites, and text are made up.
- `scenes.js` scripts the three demos. `banner.html`, `setup.html`, `sheet.html`, and `result.html` lay out the stills.
- `render.mjs` opens each page in headless Microsoft Edge, steps through it frame by frame over the DevTools protocol, and encodes the frames with ffmpeg. `jobs.json` lists every output.

## Rendering

Needs Node.js 22 or later, Microsoft Edge, and ffmpeg on `PATH`.

```powershell
node render.mjs jobs.json
```

```powershell
node render.mjs jobs.json capture
```

The second form renders one job by name. Frames and the browser profile are kept in `%TEMP%\sstk-media`. A GIF takes about a minute.

When the overlay changes visibly, update the matching numbers or drawing code in `engine.js` before rendering again.
