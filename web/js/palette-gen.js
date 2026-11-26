// Random palettes of colours that go together, built from a colour-wheel harmony around a random hue.
// Tuned for LEDs: strong colours, no dark or washed-out ones, which look grey or muddy on the badge.

import { toRgb } from './color-picker.js';

// hue offsets from the base hue, in degrees
const SCHEMES = [
  { name: 'Analogous', hues: [0, 30, -30, 60, -60] },
  { name: 'Complementary', hues: [0, 180] },
  { name: 'Split complementary', hues: [0, 150, 210] },
  { name: 'Triadic', hues: [0, 120, 240] },
  { name: 'Tetradic', hues: [0, 90, 180, 270] },
  { name: 'Monochrome', hues: [0] },
];

const random = (lo, hi) => lo + Math.random() * (hi - lo);

function distance(a, b) {
  const d = (shift) => ((a >> shift) & 0xff) - ((b >> shift) & 0xff);
  return Math.hypot(d(16), d(8), d(0));
}

// Returns { name, colors } with `count` colors.
export function randomPalette(count) {
  const scheme = SCHEMES[Math.floor(Math.random() * SCHEMES.length)];
  const base = random(0, 360);
  const colors = [];
  for (let i = 0; i < count; i++) {
    const offset = scheme.hues[i % scheme.hues.length];
    // the second time round a hue, a paler or deeper shade of it
    const round = Math.floor(i / scheme.hues.length);
    let best = null;
    let bestDistance = -1;
    for (let attempt = 0; attempt < 20; attempt++) {
      const h = (base + offset + random(-10, 10) + 360) % 360;
      const s = round % 2 ? random(0.35, 0.65) : random(0.75, 1);
      const v = round % 2 ? random(0.85, 1) : random(0.6, 1);
      const color = toRgb(h, s, v);
      const nearest = Math.min(Infinity, ...colors.map((other) => distance(color, other)));
      if (nearest > bestDistance) {
        best = color;
        bestDistance = nearest;
      }
      if (nearest > 60) break;
    }
    colors.push(best);
  }
  return { name: scheme.name, colors };
}
