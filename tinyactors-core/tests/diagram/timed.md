# Graph

## Charts

### Chart 1: timed

- **Name**: timed
- **Width**: 168
- **Height**: 498

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 168 | 498 |
| work | compound | /scxml | compound | 23 | 40 | 131 | 348 |
| a | compound | work | compound | 37 | 174 | 92 | 200 |
| a1 | atomic | a | atomic | 51 | 240 | 64 | 34 |
| a2 | atomic | a | atomic | 51 | 326 | 64 | 34 |
| h | deep history | work | history | 70 | 104 | 26 | 26 |
| hs | shallow history | work | history | 114 | 104 | 26 | 26 |
| paused | atomic | /scxml | atomic | 53 | 450 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| work | paused | pause |  | forward | 109,388 109,450 |
| a1 | a2 | next |  | forward | 83,274 83,326 |
| h | a |  |  | forward | 78,129 78,174 |
| hs | a |  |  | forward | 127,130 127,162 119,162 119,174 |
| paused | h | resume |  | back | 76,450 76,440 30,440 30,136 88,136 88,129 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| work | cancel |  |  |

#### Problems

_none_
