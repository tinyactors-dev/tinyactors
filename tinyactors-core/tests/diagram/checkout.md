# Graph

## Charts

### Chart 1: checkout

- **Name**: checkout
- **Width**: 764
- **Height**: 382

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 764 | 382 |
| checkout | parallel | /scxml | parallel | 40 | 14 | 710 | 354 |
| cart | compound | checkout | region | 54 | 54 | 302 | 88 |
| open | atomic | cart | atomic | 229 | 94 | 64 | 34 |
| locked | atomic | cart | atomic | 68 | 94 | 70 | 34 |
| payment | compound | checkout | region | 416 | 54 | 310 | 88 |
| authorising | atomic | payment | atomic | 456 | 94 | 108 | 34 |
| captured | atomic | payment | atomic | 627 | 94 | 85 | 34 |
| fraud | compound | checkout | region | 54 | 160 | 302 | 88 |
| scoring | atomic | fraud | atomic | 94 | 200 | 77 | 34 |
| reviewing | atomic | fraud | atomic | 249 | 200 | 92 | 34 |
| email | compound | checkout | region | 416 | 160 | 310 | 88 |
| idle | atomic | email | atomic | 456 | 200 | 64 | 34 |
| emailing | atomic | email | atomic | 598 | 200 | 85 | 34 |
| ledger | compound | checkout | region | 54 | 266 | 302 | 88 |
| pending | atomic | ledger | atomic | 94 | 306 | 77 | 34 |
| posted | atomic | ledger | atomic | 249 | 306 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| open | authorising | pay |  | back | 293,111 456,111 |
| locked | open | unlock |  | forward | 138,103 219,103 219,111 229,111 |
| authorising | captured | ok |  | forward | 563,111 627,111 |
| authorising | locked | hold |  | back | 456,103 376,103 376,137 149,137 149,111 138,111 |
| captured | emailing | notify |  | back | 712,111 736,111 736,217 682,217 |
| scoring | reviewing | flag |  | forward | 171,214 239,214 239,217 249,217 |
| reviewing | locked | block |  | back | 342,217 396,217 396,133 154,133 154,120 138,120 |
| idle | emailing | send |  | forward | 520,217 588,217 588,213 598,213 |
| emailing | scoring | bounce |  | back | 598,223 584,223 584,193 386,193 386,241 185,241 185,224 171,224 |
| pending | posted | post |  | forward | 171,323 249,323 |
| posted | authorising | reverse |  | back | 319,323 406,323 406,120 456,120 |

#### Inside

_none_

#### Problems

_none_
