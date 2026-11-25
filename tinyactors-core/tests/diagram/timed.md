# Graph

## Charts

### Chart 1: timed

- **Name**: timed
- **Width**: 583
- **Height**: 220

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 583 | 220 |
| work | compound | /scxml | compound | 40 | 14 | 358 | 192 |
| a | compound | work | compound | 124 | 78 | 260 | 88 |
| a1 | atomic | a | atomic | 164 | 118 | 64 | 34 |
| a2 | atomic | a | atomic | 306 | 118 | 64 | 34 |
| h | deep history | work | history | 54 | 122 | 26 | 26 |
| hs | shallow history | work | history | 54 | 166 | 26 | 26 |
| paused | atomic | /scxml | atomic | 499 | 118 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| work | paused | pause |  | forward | 398,131 499,131 |
| a1 | a2 | next |  | forward | 228,135 306,135 |
| h | a |  |  | forward | 79,131 124,131 |
| hs | a |  |  | forward | 80,179 112,179 112,156 124,156 |
| paused | h | resume |  | back | 499,141 479,141 479,199 86,199 86,139 79,139 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| work | cancel |  |  |

#### Problems

_none_
