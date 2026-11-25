# Graph

## Charts

### Chart 1: timed

- **Name**: timed
- **Width**: 583
- **Height**: 207

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 583 | 207 |
| work | compound | /scxml | compound | 40 | 14 | 358 | 179 |
| a | compound | work | compound | 124 | 78 | 260 | 88 |
| a1 | atomic | a | atomic | 164 | 118 | 64 | 34 |
| a2 | atomic | a | atomic | 306 | 118 | 64 | 34 |
| h | deep history | work | history | 54 | 109 | 26 | 26 |
| hs | shallow history | work | history | 54 | 153 | 26 | 26 |
| paused | atomic | /scxml | atomic | 499 | 87 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| work | paused | pause |  | forward | 398,101 479,101 479,99 499,99 |
| a1 | a2 | next |  | forward | 228,135 306,135 |
| h | a |  |  | forward | 80,126 124,126 |
| hs | a |  |  | forward | 80,166 112,166 112,156 124,156 |
| paused | h | resume |  | back | 499,109 489,109 489,71 86,71 86,118 80,118 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| work | cancel |  |  |
