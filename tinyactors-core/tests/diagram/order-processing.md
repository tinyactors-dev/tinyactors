# Graph

## Charts

### Chart 1: order-processing

- **Name**: order-processing
- **Width**: 534
- **Height**: 474

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 534 | 474 |
| processing | parallel | /scxml | parallel | 14 | 40 | 506 | 420 |
| picking | compound | processing | region | 28 | 80 | 128 | 286 |
| queued | atomic | picking | atomic | 57 | 146 | 70 | 34 |
| collecting | atomic | picking | atomic | 42 | 232 | 100 | 34 |
| collected | atomic | picking | atomic | 46 | 318 | 92 | 34 |
| invoicing | compound | processing | region | 174 | 80 | 186 | 286 |
| draft | atomic | invoicing | atomic | 195 | 146 | 64 | 34 |
| sent | atomic | invoicing | atomic | 188 | 232 | 64 | 34 |
| settled | atomic | invoicing | atomic | 188 | 318 | 77 | 34 |
| voided | atomic | invoicing | atomic | 277 | 146 | 70 | 34 |
| shipping | compound | processing | region | 378 | 80 | 128 | 286 |
| awaiting | atomic | shipping | atomic | 400 | 146 | 85 | 34 |
| labelled | atomic | shipping | atomic | 406 | 232 | 85 | 34 |
| dispatched | atomic | shipping | atomic | 392 | 318 | 100 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| queued | collecting | start |  | forward | 68,180 68,222 92,222 92,232 |
| queued | voided | cancel |  | back | 107,180 107,193 150,193 150,406 294,406 294,180 |
| collecting | collected | done |  | forward | 92,266 92,318 |
| collecting | awaiting | missing |  | back | 128,266 128,283 147,283 147,426 388,426 388,197 408,197 408,180 |
| collected | labelled | ship |  | back | 92,352 92,436 389,436 389,284 416,284 416,266 |
| draft | sent | issue |  | forward | 220,180 220,232 |
| sent | settled | paid |  | forward | 227,266 227,318 |
| sent | collecting | recount |  | back | 194,266 194,276 177,276 177,386 153,386 153,276 118,276 118,266 |
| voided | queued | reopen |  | back | 311,180 311,416 148,416 148,195 97,195 97,180 |
| awaiting | labelled | label |  | forward | 449,180 449,232 |
| awaiting | draft | bill |  | back | 418,180 418,191 383,191 383,396 356,396 356,191 249,191 249,180 |
| labelled | dispatched | dispatch |  | forward | 466,266 466,308 442,308 442,318 |
| labelled | voided | refund |  | back | 426,266 426,274 380,274 380,376 329,376 329,180 |
| dispatched | queued | lost |  | back | 442,352 442,446 144,446 144,200 117,200 117,180 |

#### Inside

_none_

#### Problems

_none_
