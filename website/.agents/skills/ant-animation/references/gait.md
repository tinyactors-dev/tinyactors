# How real ants walk: the numbers behind `ant.mjs`

Gathered 2026-11-21. Each number names its source; "unverified" marks
what no source gave.

## Wahl, Pfeffer & Wittlinger 2015

"Walking and running in the desert ant *Cataglyphis fortis*", J Comp
Physiol A 201(6):645-656, doi:10.1007/s00359-015-0999-2,
open access at https://pmc.ncbi.nlm.nih.gov/articles/PMC4439428.
High-speed video of 388 runs in a channel, filmed from the side and from
below.

- **Body:** alitrunk (mesosoma) 2.5 to 3.3 mm. The paper gives no total
  length; about three alitrunks, 8 to 10 mm, is our estimate.
- **Speeds:** 4.5 to 619.2 mm/s, so about 0.5 to 60 body lengths a second.
- **Stride length** (middle legs) grows almost linearly with speed. The
  paper's fit is s = 0.023·v + 5.93 (mm, mm/s; R² 0.93). At the slow
  end the measured points sit below the fit: 3.5 mm at 4.5 mm/s,
  9.1 mm at 95.2 mm/s, 19.8 mm at 589.5 to 597.4 mm/s.
  - In body lengths (our estimate): about 0.4 BL at a crawl, 1 BL at
    10 BL/s and 2 BL at 60 BL/s.
  - `ant.mjs` uses `strideBL = 0.4 + 0.06·speedBL`, clamped to 0.4 to 2.
- **Stride frequency:** f = −0.0001·v² + 0.11·v + 1.63 Hz (R² 0.97). That
  gives about 2 Hz at a crawl and 10.5 Hz at 95 mm/s. It plateaus around
  30 Hz from 300 to 400 mm/s.
- **Duty factor**, the part of a cycle a foot is down (fits, v in mm/s):
  - front: −0.0005·v + 0.59
  - middle: −0.0004·v + 0.66
  - hind: −0.0006·v + 0.57

  It falls below 0.5 (running, with aerial phases) at 132 mm/s for the
  hind legs, 182 for the front and 369 for the middle. At walking speeds
  it is about 0.6, which is `DUTY` in `ant.mjs`.
- **Speed comes from a shorter stance:** "walking speed is increased by
  shortening of the stance phase and of the stance phase overlap".
- **Tripod:** the alternating tripod holds at every speed, and tripod
  coordination strength approaches 0.7 to 0.85 when fast. At a crawl the
  pattern loosens: 27 % tripod at 5 to 10 mm/s, 58 % at 25 to 30 mm/s.
- **Stride amplitude:** the middle legs' amplitude doubles across the
  speed range; the front and hind legs' barely changes. The paper gives
  no angles in degrees.
- **Not in the paper:** body yaw per stride and curve walking. The
  paper cites Zollikofer 1988 and 1994 for curvature.

## Other sources

- **Zollikofer 1994**, "Stepping patterns in ants", J Exp Biol 192:
  95-106 and 107-118. The classic reference. Ants keep a tripod over a
  wide range of speeds and curvatures. The paper proposes stride length
  is held nearly constant, as an aid to path integration.
- **Reinhardt & Blickhan 2014**, "Level locomotion in wood ants", J Exp
  Biol 217:2358 (*Formica polyctena*). Wood ants raise stride length and
  frequency together and keep the tripod. The journal's page refused
  automated access (HTTP 403), so this comes from search summaries only:
  unverified in detail.
- **Seidl & Wehner 2008**, Frontiers in Zoology 5:8, on inclines.
- **Argentine ants on rough ground** slow down by stepping less often, not
  by taking shorter strides (from a search summary; unverified).

## Footage

Two freely licensed clips are on Wikimedia Commons:

- `File:Alternating_Tripod_Gait.webm`: *Cataglyphis fortis* filmed at
  500 fps and played back at 10 fps, with force and leg-trajectory
  overlays, CC BY-SA 3.0, Toni Wöhrl.
- `File:Ant_in_Slow_Motion.webm`: an unidentified ant through a macro
  lens, CC BY-SA 4.0, Maximilian Schönherr.

upload.wikimedia.org refuses clients without a user agent that names the
tool and a contact (its User-Agent policy), so this session did not
download them. To study one, download it in a browser and step through
it with `ffmpeg -i clip.webm -vf fps=… frames/%04d.png`.

## What the numbers mean for the animation

- **Ground speed and leg speed are one quantity.** Derive the step rate
  from speed and stride length (f = v / stride). Never drive the legs
  with a free-running oscillator.
- **A foot on the ground does not move.** With stride length fixed by
  speed, the leg's sweep follows from geometry. Solve it from the
  planted foot; don't choose an amplitude.
- **A slow ant is a dawdling ant.** Real foragers walk 5 to 10 BL/s.
  The website's ants walk about 1 BL/s so they don't distract, which is
  slow but within the measured range. Raise `v` for a busier ant; the
  legs follow by themselves.
