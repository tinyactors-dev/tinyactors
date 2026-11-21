---
name: ant-animation
description: Animate the design system's rigged ant (ant-resting.svg, logo.svg) on the website. Covers walking with feet planted, turning while walking and in place, idling, greeting, eating, hibernating, waking and leaving. Use before writing or changing any code that moves an ant (the presence ants, the anthill's sprites, a hero animation) and before rendering a motion study or video of one. It holds the rig's conventions, a gait model fitted to measured ant locomotion, the mistakes already made with their fixes, and scripts that pose the rig, plan paths in Node.js and render stills or an MP4.
---

# Animating the ant

The ant is `.agents/skills/tinyactors-design/assets/ant-resting.svg` at the
repository root; `logo.svg` has the same rig. Every moving part has an
`id` and a `data-pivot` joint. This skill holds code that moves it the
way an ant moves, and the measurements that code is fitted to.

- **[references/gait.md](references/gait.md):** the numbers behind the gait
  (stride, step rate, duty factor), with sources, and where to find footage.
- **`scripts/rig.mjs`:** poses a clone of `#ant` from joint angles. It is
  the only file that touches the DOM.
- **`scripts/ant.mjs`:** the body. Speed and turn rate go in; the position,
  the planted feet and every joint angle come out. It has no DOM, so it
  runs the same in the page, in a Worker and in Node.js.
- **`scripts/scene.mjs`:** the motion study's cue list: two ants and nine
  chapters, in 31 seconds.
- **`scripts/eat.mjs`:** the eating study: an ant walks to a pile, takes
  three bites and carries a crumb away, in 16 seconds. It steers after a
  point as the presence layer does.
- **`scripts/stage.html`:** draws the scene on the design system's tokens
  and fonts, with pheromone trails and lifecycle tags. Open it served
  (`render.mjs` serves it) to watch the scene loop.
- **`scripts/render.mjs`:** renders frame by frame with Playwright:
  `node scripts/render.mjs stills 2,6,17.5 <outdir>` or
  `node scripts/render.mjs video 60 <outdir> [scene]` (an MP4 at 1920x1080,
  which needs ffmpeg; about 3.5 minutes for 31 s at 60 fps). The scene is
  `scene` (the default) or `eat`; the stage takes it as `?scene=`.
- **`scripts/plan.mjs`:** prints the scene's paths, so cues are tuned
  before rendering.

## The rig

- **Coordinates:** SVG units, 1024 square. The ant faces up (−y). It turns
  about `CENTER` = (512, 530), on the thorax. Body length, mandibles to
  gaster tip, is 633 units.
- **Placing an ant on a stage:**
  `translate(x y) rotate(heading + sway) scale(s) translate(-512 -530)`.
  `heading` is degrees clockwise from up, so 90 faces right. With s = 0.21
  an ant is about 133 px long. The presence ants will be much smaller.
- **Joints:**
  - six legs: `leg-{front,middle,hind}-{left,right}`
  - `head`, `gaster` and `petiole`
  - `antenna-left`/`-right`, each a shaft and a club
  - `mandible-left`/`-right`
- **Left and right** are the ant's own sides, and match the viewer's
  while it faces up. For antennae and mandibles `rig.mjs` takes "forward"
  angles and mirrors the right side itself. Legs take the absolute angle
  the gait solved.
- **The feet at rest** are not in the SVG's metadata. `ant.mjs` (`LEGS`)
  holds them, read from each leg path's tip. They must change if the
  artwork does.

## The gait

Taken from high-speed video studies of ants (references/gait.md):

1. **An alternating tripod.** Front left, middle right and hind left
   step together, against the other three.
2. **Duty factor 0.6.** A foot is on the ground for 60% of a cycle and
   in the air for 40%.
3. **The ground drives the legs.** Stride length comes from speed:
   `strideBL = 0.4 + 0.06 · speedBL`, in body lengths. Step rate is
   speed divided by stride, never a clock of its own.
4. **Planted feet.** During stance a foot is fixed in world coordinates,
   and the leg is rotated about its pivot (and scaled from 0.72 to 1.12
   of its length) to reach it. A lifting foot slides by nothing.
5. **Placing a step.** A swinging foot heads for where its rest point
   will be halfway through the coming stance, predicted from the current
   speed and turn rate. That one rule gives the turns:
   - turning while walking, the outer legs take long steps and the inner
     legs short ones;
   - turning in place, the two sides step against each other.

   The "speed the feet see" counts both motions: `|v| + |ω|·290·s`.
6. **Stopping.** When the ant stops, the cycle runs on at about 2.2 Hz
   until all six feet are down. Otherwise legs freeze in the air.

The rest of the body:
- The head leads a turn (yaw ≈ 0.16 × turn rate, at most 18°) and settles
  quickly.
- The gaster lags (−0.1 × turn rate) and settles slowly.
- The body sways about 1.2° per stride.
- Antennae sweep at half the step rate and lean into turns.

## The behaviours

Each behaviour is a target `ant.mjs` eases toward (`ant.target`), so a
change of mind never jumps.

| Behaviour | Targets | What it shows |
|---|---|---|
| walk | `v` | the tripod gait, a pheromone dot every 13 px |
| turn while walking | `v`, `w` | the head leads, the gaster lags, the outer legs take longer strides |
| turn in place | `w`, with `v` 0 | the two sides step against each other |
| idle | `idle: 1` | the antennae explore, the head looks around, the mandibles twitch |
| eat | `eat: 1` for about 2.5 s, facing the food | the antennae drum forward on the food (2.4 Hz), the head nods (1.8 Hz) and dips (`headDip`, drawn smaller from above), the mandibles chew (3.5 Hz), the body rocks; the renderer shows a crumb in the mandibles |
| greet (antennation) | `greet: 1` on both ants, about 150 px apart, face to face | antennae tapping at 4.5 Hz, mandibles at 2 Hz |
| hibernate | `tuck: 1, alpha: 0.42` | the legs fold along the body and shorten, the antennae fold back, the ant desaturates |
| wake | `tuck: 0, alpha: 1` | it unfolds, flicks its antennae, and its feet are set back under the body |
| leave | turn in place, then `v` | the actor is done: it walks out of view |

These map onto an actor's lifecycle on the presence page: a visible tab
is running, a hidden tab hibernates, a closed tab is done.

## Mistakes already made

- **Legs on a sine wave skate.** The first version swung each leg ±16° on
  its own oscillator at an arbitrary stride. Speed and step rate were in
  a plausible range, yet the feet slid and the legs looked too busy for
  the motion. Planting the feet (gait rule 4) fixed it. A gait whose
  leg angles do not come from where the feet are will skate.
- **The SVG's `style="transform-origin: …"` moves attribute transforms.**
  A CSS transform-origin also applies to the `transform` attribute, so
  `rig.mjs` removes each part's `style` and rotates about the pivot
  explicitly (`rotate(a x y)`).
- **Ids collide** when several ants share a document: `rig.mjs` strips
  every `id` after collecting its parts.
- **A pose is needed before the first frame:** call `advance(ant, 0)`
  after `makeAnt`.
- **Spawning an ant at full speed** stretches its legs for one step, as
  its feet start at rest. Spawn it offscreen or at rest.
- **The scene is simulated forward:** stills must be asked for in
  ascending time, and a frame never goes back. Render a looping preview
  by reloading the page at the end.
- **Tags near the bottom edge** collide with the chapter bar: put them
  above the ant there.
- **Canvas flicker on mobile Safari.** Safari fires `resize` all through
  a scroll as its address bar slides, and assigning `canvas.width` clears
  the canvas even to the same size. A layer that draws at 30 fps then
  paints blank frames: 61 of 120 in a reproduction in Chromium. Touch the
  bitmap only when its size changes, and draw again in the same task. Size
  a fixed canvas to `100lvh`, not `innerHeight`, so positions mapped onto
  it hold still while the bar moves (website/presence/layer.mjs).
- **A scene's `time` must stay a getter.** `Object.assign(scene, { get
  time() {...} })` copies the getter's value once, and the chapter never
  moves; define it with `Object.defineProperty`.
- **An eating ant hides its food** if it stops too close: its centre
  goes about 90 px from the pile at scale 0.21 (70 units of mandibles
  ahead of the centre, plus the pile's radius).

## Open questions for the presence page

Each was asked of the maintainer and not yet decided:
- the ants' speed: the study walks at about 1 BL/s; real foragers walk 5
  to 10 BL/s;
- the hibernate pose: folded legs, or only a fade;
- each visitor's colour: the trail and tag only, or a tint on the pine
  parts.
