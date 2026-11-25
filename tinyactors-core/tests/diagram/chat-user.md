# Graph

## Charts

### Chart 1: user

- **Name**: user
- **Width**: 360
- **Height**: 610

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 360 | 610 |
| offline | atomic | /scxml | atomic | 130 | 154 | 78 | 34 |
| online | compound | /scxml | compound | 14 | 270 | 310 | 326 |
| active | atomic | online | atomic | 28 | 428 | 99 | 58 |
| away | atomic | online | atomic | 46 | 548 | 64 | 34 |
| resume | shallow history | online | history | 65 | 358 | 26 | 26 |
| reconnecting | atomic | /scxml | atomic | 60 | 14 | 262 | 58 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| offline | online | connect |  | forward | 156,188 156,270 |
| online | offline | disconnect |  | back | 182,270 182,188 |
| online | reconnecting | connection.lost |  | back | 236,270 236,124 126,124 126,72 |
| active | away | idle |  | forward | 73,486 73,548 |
| away | active | activity |  | back | 100,548 100,486 |
| resume | active |  |  | forward | 78,384 78,428 |
| reconnecting | resume | connect |  | forward | 90,72 90,134 236,134 236,260 78,260 78,358 |
| reconnecting | offline | disconnect |  | forward | 172,72 172,154 |
| reconnecting | offline | grace.over |  | forward | 264,72 264,144 198,144 198,154 |

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
