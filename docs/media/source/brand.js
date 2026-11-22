// The program's icon, after RenderLogo in src/graphics/image.cpp, on its 256-unit grid.
function logoSvg(size) {
  const outer = 55.5, stroke = 13, line = outer + stroke / 2, bend = 16, tip = outer + 51.5 - stroke / 2;
  const corner = line + bend, s = 256, bottom = s - outer;
  const cardRight = outer + 82, cardTop = bottom - 71.5, cardRadius = (bend + stroke / 2) * 0.65;
  const ink = '#f3f2ef';
  const arm = (d) => `<path d="${d}" fill="none" stroke="${ink}" stroke-width="${stroke}" stroke-linecap="round"/>`;
  return `<svg width="${size}" height="${size}" viewBox="0 0 256 256">
    <rect x="16" y="16" width="224" height="224" rx="54" fill="#16171a"/>
    <rect x="17" y="17" width="222" height="222" rx="53" fill="none" stroke="rgba(255,255,255,.12)" stroke-width="2"/>
    ${arm(`M${line} ${tip}V${corner}A${bend} ${bend} 0 0 1 ${corner} ${line}H${tip}`)}
    ${arm(`M${s - line} ${tip}V${corner}A${bend} ${bend} 0 0 0 ${s - corner} ${line}H${s - tip}`)}
    ${arm(`M${s - line} ${s - tip}V${s - corner}A${bend} ${bend} 0 0 1 ${s - corner} ${s - line}H${s - tip}`)}
    <rect x="${outer}" y="${cardTop}" width="${cardRight - outer}" height="${bottom - cardTop}" rx="${cardRadius}" fill="${ink}"/>
  </svg>`;
}

function winKeySvg(color) {
  return `<svg width="13" height="13"><g fill="${color}"><rect width="6" height="6" rx="1"/><rect x="7" width="6" height="6" rx="1"/><rect y="7" width="6" height="6" rx="1"/><rect x="7" y="7" width="6" height="6" rx="1"/></g></svg>`;
}
