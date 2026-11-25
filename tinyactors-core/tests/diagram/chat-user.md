# Graph

## Charts

### Chart 1: user

- **Name**: user
- **Width**: 355
- **Height**: 610

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 355 | 610 |
| offline | atomic | /scxml | atomic | 55 | 154 | 78 | 34 |
| online | compound | /scxml | compound | 14 | 270 | 310 | 326 |
| active | atomic | online | atomic | 28 | 428 | 99 | 58 |
| away | atomic | online | atomic | 46 | 548 | 64 | 34 |
| resume | shallow history | online | history | 65 | 358 | 26 | 26 |
| reconnecting | atomic | /scxml | atomic | 60 | 14 | 262 | 58 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| offline | online | connect |  | forward | 65,188 65,270 |
| online | offline | disconnect |  | back | 111,270 111,188 |
| online | reconnecting | connection.lost |  | back | 236,270 236,260 231,260 231,144 290,144 290,72 |
| active | away | idle |  | forward | 67,486 67,548 |
| away | active | activity |  | back | 88,548 88,538 93,538 93,486 |
| resume | active |  |  | forward | 78,384 78,428 |
| reconnecting | resume | connect |  | forward | 70,72 70,114 236,114 236,240 78,240 78,358 |
| reconnecting | offline | disconnect |  | forward | 152,72 152,124 97,124 97,154 |
| reconnecting | offline | grace.over |  | forward | 244,72 244,134 123,134 123,154 |

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
