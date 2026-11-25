# Graph

## Charts

### Chart 1: checkout

- **Name**: checkout
- **Width**: 461
- **Height**: 578

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 461 | 578 |
| checkout | parallel | /scxml | parallel | 14 | 40 | 433 | 524 |
| cart | compound | checkout | region | 28 | 80 | 113 | 200 |
| open | atomic | cart | atomic | 46 | 206 | 64 | 34 |
| locked | atomic | cart | atomic | 42 | 120 | 71 | 34 |
| payment | compound | checkout | region | 159 | 80 | 136 | 200 |
| authorising | atomic | payment | atomic | 173 | 146 | 108 | 34 |
| captured | atomic | payment | atomic | 184 | 232 | 85 | 34 |
| fraud | compound | checkout | region | 312 | 80 | 120 | 200 |
| scoring | atomic | fraud | atomic | 334 | 146 | 77 | 34 |
| reviewing | atomic | fraud | atomic | 326 | 232 | 92 | 34 |
| email | compound | checkout | region | 28 | 350 | 113 | 200 |
| idle | atomic | email | atomic | 42 | 416 | 64 | 34 |
| emailing | atomic | email | atomic | 42 | 502 | 85 | 34 |
| ledger | compound | checkout | region | 159 | 350 | 136 | 200 |
| pending | atomic | ledger | atomic | 173 | 416 | 77 | 34 |
| posted | atomic | ledger | atomic | 177 | 502 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| open | authorising | pay |  | back | 78,240 78,310 165,310 165,193 188,193 188,180 |
| locked | open | unlock |  | forward | 61,154 61,196 78,196 78,206 |
| authorising | captured | ok |  | forward | 227,180 227,232 |
| authorising | locked | hold |  | back | 198,180 198,195 167,195 167,320 133,320 133,169 93,169 93,154 |
| captured | emailing | notify |  | back | 227,266 227,300 131,300 131,485 84,485 84,502 |
| scoring | reviewing | flag |  | forward | 373,180 373,232 |
| reviewing | locked | block |  | back | 373,266 373,340 129,340 129,174 103,174 103,154 |
| idle | emailing | send |  | forward | 74,450 74,502 |
| emailing | scoring | bounce |  | back | 106,502 106,491 137,491 137,330 322,330 322,197 347,197 347,180 |
| pending | posted | post |  | forward | 211,450 211,502 |
| posted | authorising | reverse |  | back | 223,502 223,482 282,482 282,290 161,290 161,188 208,188 208,180 |

#### Inside

_none_

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| edgesOverlap | open → authorising on pay | authorising → locked on hold |
| edgesOverlap | authorising → locked on hold | captured → emailing on notify |
| edgesOverlap | captured → emailing on notify | reviewing → locked on block |
