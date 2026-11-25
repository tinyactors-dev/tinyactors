# Graph

## Charts

### Chart 1: user

- **Name**: user
- **Width**: 926
- **Height**: 203

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 926 | 203 |
| offline | atomic | /scxml | atomic | 277 | 111 | 77 | 34 |
| online | compound | /scxml | compound | 536 | 14 | 376 | 160 |
| active | atomic | online | atomic | 620 | 102 | 99 | 58 |
| away | atomic | online | atomic | 834 | 114 | 64 | 34 |
| resume | shallow history | online | history | 550 | 118 | 26 | 26 |
| reconnecting | atomic | /scxml | atomic | 14 | 104 | 115 | 84 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| offline | online | connect |  | forward | 354,121 536,121 |
| online | offline | disconnect |  | back | 536,134 354,134 |
| online | reconnecting | connection.lost |  | back | 536,164 516,164 516,176 247,176 247,114 129,114 |
| active | away | idle |  | forward | 719,124 834,124 |
| away | active | activity |  | back | 834,138 719,138 |
| resume | active |  |  | forward | 576,131 620,131 |
| reconnecting | resume | connect |  | forward | 129,171 267,171 267,176 526,176 526,131 550,131 |
| reconnecting | offline | disconnect |  | forward | 129,124 277,124 |
| reconnecting | offline | grace.over |  | forward | 129,135 277,135 |

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
