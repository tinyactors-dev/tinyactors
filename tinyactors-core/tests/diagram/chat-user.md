# Graph

## Charts

### Chart 1: user

- **Name**: user
- **Width**: 926
- **Height**: 202

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 926 | 202 |
| offline | atomic | /scxml | atomic | 277 | 114 | 77 | 34 |
| online | compound | /scxml | compound | 536 | 14 | 376 | 160 |
| active | atomic | online | atomic | 620 | 102 | 99 | 58 |
| away | atomic | online | atomic | 834 | 114 | 64 | 34 |
| resume | shallow history | online | history | 550 | 118 | 26 | 26 |
| reconnecting | atomic | /scxml | atomic | 14 | 104 | 115 | 84 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| offline | online | connect |  | forward | 354,125 516,125 516,121 536,121 |
| online | offline | disconnect |  | back | 536,137 354,137 |
| online | reconnecting | connection.lost |  | back | 536,164 506,164 506,176 237,176 237,121 129,121 |
| active | away | idle |  | forward | 719,124 834,124 |
| away | active | activity |  | back | 834,138 719,138 |
| resume | active |  |  | forward | 576,131 620,131 |
| reconnecting | resume | connect |  | forward | 129,138 247,138 247,176 526,176 526,131 550,131 |
| reconnecting | offline | disconnect |  | forward | 129,151 257,151 257,127 277,127 |
| reconnecting | offline | grace.over |  | forward | 129,168 267,168 267,138 277,138 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| online | reconnected |  |  |
| online | typing | !(_event.data.room in quiet) |  |
| online | typing |  |  |
| online | quiet.over |  |  |
| online | others.typing |  |  |
| online | indicator.expired |  |  |
| online | join.refused |  |  |
| online | join |  |  |
| online | joined |  |  |
| online | error.communication | _event.sendid === 'join' |  |
| online | dm.open |  |  |
| online | dm.opened |  |  |
| online | dms |  |  |
| online | post |  |  |
| online | roster.update |  |  |
| online | roster |  |  |
| online | message |  |  |
| active | activity |  |  |
| reconnecting | message |  |  |

#### Problems

_none_
