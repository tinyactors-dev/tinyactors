// The anthill's ants, drawn from the design system's rigged ant
// (ant-resting.svg: each leg a path with its own pivot, data-pivot). The
// walk is a tripod gait: the left front, right middle and left hind legs
// swing together, against the other three. Each frame of the cycle is the
// SVG with its legs (and antennae, a little) rotated about their pivots,
// in each colony's colour, rasterised once into an atlas: a row per
// colony, a column per frame. The renderer only samples it.

export const FRAMES = 8;
export const CELL = 64; // pixels per frame in the atlas

const TRIPOD = { 'leg-front-left': 1, 'leg-middle-right': 1, 'leg-hind-left': 1, 'leg-front-right': -1, 'leg-middle-left': -1, 'leg-hind-right': -1 };
const SWING = 16; // degrees each leg swings either way
const INK = '#302B28'; // the ant's own ink in the SVG

/** An atlas canvas: FRAMES columns, a row per colour in `colors`. */
export async function atlas(svgText, colors) {
  const canvas = new OffscreenCanvas(CELL * FRAMES, CELL * colors.length);
  const context = canvas.getContext('2d');
  for (let row = 0; row < colors.length; row++) {
    for (let frame = 0; frame < FRAMES; frame++) {
      const image = await rasterize(pose(svgText, frame, colors[row]));
      context.drawImage(image, frame * CELL, row * CELL, CELL, CELL);
    }
  }
  return canvas;
}

/** The SVG at `frame` of the walk, its ink replaced by `color`. */
export function pose(svgText, frame, color) {
  const document = new DOMParser().parseFromString(svgText, 'image/svg+xml');
  const svg = document.documentElement;
  svg.querySelector('metadata')?.remove();
  svg.querySelector('#background')?.remove(); // the illustration's paper: a sprite is the ant alone
  svg.setAttribute('viewBox', '128 104 766 766'); // the ant, as the logo mark crops it, made square
  const phase = Math.sin((2 * Math.PI * frame) / FRAMES);
  for (const [id, side] of Object.entries(TRIPOD)) {
    const leg = svg.querySelector(`#${id}`);
    if (!leg) continue;
    const [x, y] = leg.getAttribute('data-pivot').split(' ').map(Number);
    const outward = leg.classList.contains('side-left') ? 1 : -1;
    leg.removeAttribute('style');
    leg.setAttribute('transform', `rotate(${side * outward * SWING * phase} ${x} ${y})`);
  }
  for (const antenna of svg.querySelectorAll('.antenna')) {
    const [x, y] = antenna.getAttribute('data-pivot').split(' ').map(Number);
    antenna.removeAttribute('style');
    antenna.setAttribute('transform', `rotate(${4 * Math.cos((2 * Math.PI * frame) / FRAMES)} ${x} ${y})`);
  }
  return new XMLSerializer().serializeToString(svg).replaceAll(INK, color);
}

async function rasterize(svgText) {
  const url = URL.createObjectURL(new Blob([svgText], { type: 'image/svg+xml' }));
  try {
    const image = new Image(CELL, CELL);
    image.src = url;
    await image.decode();
    return image;
  } finally {
    URL.revokeObjectURL(url);
  }
}
