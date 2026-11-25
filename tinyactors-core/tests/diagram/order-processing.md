# Graph

## Charts

### Chart 1: order-processing

- **Name**: order-processing
- **Width**: 675
- **Height**: 434

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 675 | 434 |
| processing | parallel | /scxml | parallel | 40 | 14 | 621 | 406 |
| picking | compound | processing | region | 54 | 54 | 513 | 88 |
| queued | atomic | picking | atomic | 94 | 94 | 70 | 34 |
| collecting | atomic | picking | atomic | 248 | 94 | 100 | 34 |
| collected | atomic | picking | atomic | 426 | 94 | 92 | 34 |
| invoicing | compound | processing | region | 54 | 160 | 513 | 140 |
| draft | atomic | invoicing | atomic | 94 | 200 | 64 | 34 |
| sent | atomic | invoicing | atomic | 248 | 200 | 64 | 34 |
| settled | atomic | invoicing | atomic | 390 | 203 | 77 | 34 |
| voided | atomic | invoicing | atomic | 94 | 252 | 70 | 34 |
| shipping | compound | processing | region | 54 | 318 | 513 | 88 |
| awaiting | atomic | shipping | atomic | 94 | 358 | 85 | 34 |
| labelled | atomic | shipping | atomic | 263 | 358 | 85 | 34 |
| dispatched | atomic | shipping | atomic | 453 | 358 | 100 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| queued | collecting | start |  | forward | 164,101 238,101 238,111 248,111 |
| queued | voided | cancel |  | back | 164,114 175,114 175,137 597,137 597,261 164,261 |
| collecting | collected | done |  | forward | 348,103 416,103 416,111 426,111 |
| collecting | awaiting | missing |  | back | 348,120 364,120 364,133 627,133 627,353 195,353 195,367 179,367 |
| collected | labelled | ship |  | back | 518,111 637,111 637,355 366,355 366,367 348,367 |
| draft | sent | issue |  | forward | 158,214 238,214 238,217 248,217 |
| sent | settled | paid |  | forward | 312,220 390,220 |
| sent | collecting | recount |  | back | 312,210 319,210 319,188 577,188 577,140 355,140 355,111 348,111 |
| voided | queued | reopen |  | back | 164,269 607,269 607,136 177,136 177,108 164,108 |
| awaiting | labelled | label |  | forward | 179,384 253,384 253,375 263,375 |
| awaiting | draft | bill |  | back | 179,375 194,375 194,352 617,352 617,292 173,292 173,224 158,224 |
| labelled | dispatched | dispatch |  | forward | 348,384 443,384 443,375 453,375 |
| labelled | voided | refund |  | back | 348,375 357,375 357,347 587,347 587,278 164,278 |
| dispatched | queued | lost |  | back | 553,375 647,375 647,130 184,130 184,121 164,121 |

#### Inside

_none_

#### Problems

_none_
