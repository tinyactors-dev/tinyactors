// Draws every ant on the GPU: one point per ant, which the fragment shader
// turns into the rigged ant's sprite (sprites.mjs), rotated to the ant's
// heading and stepping through its walk while it moves. The ants' trips live
// in two float textures, a texel per ant in each: (x0, y0, x1, y1) and
// (t0, t1, look, -). A message from the worker uploads only the trips that
// changed and scatters them into the textures in one draw of points; the
// frame reads them by the ant's index. So drawing costs the CPU nothing per
// ant, and an update costs what changed, not the population.
//
// createRenderer() answers null where WebGL2 or float render targets are
// missing; page.mjs then draws dots on a 2D canvas.

import { WIDTH, HEIGHT } from './world.mjs';
import { FRAMES } from './sprites.mjs';

const SIDE = 2048; // texels a row of the trip textures

const SCATTER_VERTEX = `#version 300 es
in float slot; in vec4 a; in vec3 b;
out vec4 va; out vec4 vb;
uniform vec2 size;
void main() {
  vec2 texel = vec2(mod(slot, size.x), floor(slot / size.x)) + 0.5;
  gl_Position = vec4(texel / size * 2.0 - 1.0, 0.0, 1.0);
  gl_PointSize = 1.0;
  va = a; vb = vec4(b, 0.0);
}`;
const SCATTER_FRAGMENT = `#version 300 es
precision highp float;
in vec4 va; in vec4 vb;
layout(location = 0) out vec4 outA;
layout(location = 1) out vec4 outB;
void main() { outA = va; outB = vb; }`;

const DRAW_VERTEX = `#version 300 es
precision highp float;
uniform highp sampler2D tripsA, tripsB;
uniform float time, size, unitPixels, scale, maxPoint;
uniform vec2 field, origin; // the view: its top left corner in the field, and its zoom
out float angle; flat out int frame; flat out int colony;
void main() {
  ivec2 texel = ivec2(gl_VertexID % ${SIDE}, gl_VertexID / ${SIDE});
  vec4 a = texelFetch(tripsA, texel, 0), b = texelFetch(tripsB, texel, 0);
  if (b.z < 0.5) { gl_Position = vec4(2.0, 2.0, 0.0, 1.0); gl_PointSize = 0.0; return; }
  vec2 from = a.xy, to = a.zw, way = to - from;
  float span = b.y - b.x;
  float f = span > 0.0 ? clamp((time - b.x) / span, 0.0, 1.0) : 1.0;
  vec2 at = from + way * f;
  vec2 seen = (at - origin) * scale;
  gl_Position = vec4(seen.x / field.x * 2.0 - 1.0, 1.0 - seen.y / field.y * 2.0, 0.0, 1.0);
  gl_PointSize = min(size * scale * unitPixels * 1.42, maxPoint);
  angle = atan(way.y, way.x);
  bool walking = f > 0.0 && f < 1.0 && length(way) > 0.5;
  float stride = length(way) * f / (size * 0.35) + float(gl_VertexID % 7);
  frame = walking ? int(mod(stride, ${FRAMES}.0)) : 0;
  colony = int(b.z) - 1;
}`;
const DRAW_FRAGMENT = `#version 300 es
precision highp float;
uniform sampler2D atlas;
in float angle; flat in int frame; flat in int colony;
out vec4 color;
void main() {
  // The sprite faces up; turn the lookup so it faces where it walks.
  vec2 p = (gl_PointCoord - 0.5) * 1.42;
  float turn = angle + 1.5707963;
  vec2 uv = vec2(cos(turn) * p.x + sin(turn) * p.y, -sin(turn) * p.x + cos(turn) * p.y) + 0.5;
  if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) discard;
  color = texture(atlas, vec2((float(frame) + uv.x) / ${FRAMES}.0, (float(colony) + uv.y) / 2.0));
  if (color.a < 0.1) discard;
}`;

export function createRenderer(canvas, population, atlasCanvas, ground) {
  const gl = canvas.getContext('webgl2', { premultipliedAlpha: false, antialias: false });
  if (!gl || !gl.getExtension('EXT_color_buffer_float') || software(gl)) return null;
  const rows = Math.ceil(population / SIDE);

  const trips = [0, 1].map(() => {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, SIDE, rows);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    return texture;
  });
  const framebuffer = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, trips[0], 0);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, trips[1], 0);
  gl.drawBuffers([gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1]);
  if (gl.checkFramebufferStatus(gl.FRAMEBUFFER) !== gl.FRAMEBUFFER_COMPLETE) return null;
  gl.viewport(0, 0, SIDE, rows);
  gl.clearBufferfv(gl.COLOR, 0, [0, 0, 0, 0]);
  gl.clearBufferfv(gl.COLOR, 1, [0, 0, 0, 0]);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);

  const atlas = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, atlas);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, atlasCanvas);
  gl.generateMipmap(gl.TEXTURE_2D);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR_MIPMAP_LINEAR);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);

  const scatter = program(gl, SCATTER_VERTEX, SCATTER_FRAGMENT);
  const draw = program(gl, DRAW_VERTEX, DRAW_FRAGMENT);
  const deltaBuffer = gl.createBuffer();
  const scatterVAO = gl.createVertexArray();
  gl.bindVertexArray(scatterVAO);
  gl.bindBuffer(gl.ARRAY_BUFFER, deltaBuffer);
  // A delta is [slot, x0, y0, x1, y1, t0, t1, look] per ant (world.mjs, delta()).
  const attribute = (name, size, offset) => {
    const at = gl.getAttribLocation(scatter, name);
    gl.enableVertexAttribArray(at);
    gl.vertexAttribPointer(at, size, gl.FLOAT, false, 32, offset);
  };
  attribute('slot', 1, 0);
  attribute('a', 4, 4); // x0, y0, x1, y1
  attribute('b', 3, 20); // t0, t1, look
  gl.bindVertexArray(null);
  const drawVAO = gl.createVertexArray();
  const maxPoint = gl.getParameter(gl.ALIASED_POINT_SIZE_RANGE)[1];
  const [r, g, b] = [1, 3, 5].map((i) => parseInt(ground.slice(i, i + 2), 16) / 255);

  return {
    kind: 'webgl',
    /** Writes the changed trips into the textures. */
    apply(delta) {
      if (delta.length === 0) return;
      gl.bindBuffer(gl.ARRAY_BUFFER, deltaBuffer);
      gl.bufferData(gl.ARRAY_BUFFER, delta, gl.STREAM_DRAW);
      gl.useProgram(scatter);
      gl.bindVertexArray(scatterVAO);
      gl.uniform2f(gl.getUniformLocation(scatter, 'size'), SIDE, rows);
      gl.bindFramebuffer(gl.FRAMEBUFFER, framebuffer);
      gl.viewport(0, 0, SIDE, rows);
      gl.disable(gl.BLEND);
      gl.drawArrays(gl.POINTS, 0, delta.length / 8);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.bindVertexArray(null);
    },
    /** Draws every ant at simulated time `time` (seconds), through `view` ({left, top, scale}). */
    draw(time, antSize, view) {
      const width = canvas.width, height = canvas.height;
      gl.viewport(0, 0, width, height);
      gl.clearColor(r, g, b, 1);
      gl.clear(gl.COLOR_BUFFER_BIT);
      gl.useProgram(draw);
      gl.bindVertexArray(drawVAO);
      gl.enable(gl.BLEND);
      gl.blendFunc(gl.SRC_ALPHA, gl.ONE_MINUS_SRC_ALPHA);
      gl.activeTexture(gl.TEXTURE0); gl.bindTexture(gl.TEXTURE_2D, trips[0]);
      gl.activeTexture(gl.TEXTURE1); gl.bindTexture(gl.TEXTURE_2D, trips[1]);
      gl.activeTexture(gl.TEXTURE2); gl.bindTexture(gl.TEXTURE_2D, atlas);
      gl.uniform1i(gl.getUniformLocation(draw, 'tripsA'), 0);
      gl.uniform1i(gl.getUniformLocation(draw, 'tripsB'), 1);
      gl.uniform1i(gl.getUniformLocation(draw, 'atlas'), 2);
      gl.uniform1f(gl.getUniformLocation(draw, 'time'), time);
      gl.uniform1f(gl.getUniformLocation(draw, 'size'), antSize);
      gl.uniform1f(gl.getUniformLocation(draw, 'unitPixels'), width / WIDTH);
      gl.uniform2f(gl.getUniformLocation(draw, 'field'), WIDTH, HEIGHT);
      gl.uniform2f(gl.getUniformLocation(draw, 'origin'), view.left, view.top);
      gl.uniform1f(gl.getUniformLocation(draw, 'scale'), view.scale);
      gl.uniform1f(gl.getUniformLocation(draw, 'maxPoint'), maxPoint);
      gl.drawArrays(gl.POINTS, 0, population);
      gl.bindVertexArray(null);
    },
    destroy() {
      gl.getExtension('WEBGL_lose_context')?.loseContext();
    },
  };
}

// A browser without a GPU to use (a virtual machine, a blocked driver) runs
// WebGL on the CPU, slower than the 2D canvas's dots: those draw instead.
function software(gl) {
  const info = gl.getExtension('WEBGL_debug_renderer_info');
  const name = info ? `${gl.getParameter(info.UNMASKED_RENDERER_WEBGL)}` : '';
  if (/swiftshader|llvmpipe|softpipe|software/i.test(name)) {
    gl.getExtension('WEBGL_lose_context')?.loseContext();
    return true;
  }
  return false;
}

function program(gl, vertexSource, fragmentSource) {
  const shader = (type, source) => {
    const s = gl.createShader(type);
    gl.shaderSource(s, source);
    gl.compileShader(s);
    if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(s));
    return s;
  };
  const p = gl.createProgram();
  gl.attachShader(p, shader(gl.VERTEX_SHADER, vertexSource));
  gl.attachShader(p, shader(gl.FRAGMENT_SHADER, fragmentSource));
  gl.linkProgram(p);
  if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(p));
  return p;
}
