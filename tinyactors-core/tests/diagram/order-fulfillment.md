# Graph

## Charts

### Chart 1: order

- **Name**: order
- **Width**: 875
- **Height**: 1284

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 875 | 1284 |
| validating | atomic | /scxml | atomic | 413 | 40 | 292 | 34 |
| payment | compound | /scxml | compound | 217 | 146 | 419 | 210 |
| charging | atomic | payment | atomic | 231 | 212 | 391 | 34 |
| backoff | atomic | payment | atomic | 466 | 308 | 77 | 34 |
| fulfilment | compound | /scxml | compound | 143 | 458 | 272 | 330 |
| picking | atomic | fulfilment | atomic | 176 | 524 | 225 | 34 |
| cancelling | atomic | fulfilment | atomic | 157 | 620 | 160 | 34 |
| handedOver | atomic | fulfilment | atomic | 187 | 716 | 100 | 58 |
| refunding | compound | /scxml | compound | 267 | 910 | 188 | 234 |
| requesting | atomic | refunding | atomic | 281 | 1000 | 160 | 34 |
| waiting | atomic | refunding | atomic | 319 | 1096 | 77 | 34 |
| delivered | final | /scxml | final | 107 | 910 | 92 | 34 |
| cancelled | final | /scxml | final | 259 | 1236 | 92 | 34 |
| failed | final | /scxml | final | 369 | 1236 | 94 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| validating | failed |  | items.length === 0 | forward | 423,74 423,126 669,126 669,398 519,398 519,830 503,830 503,1186 424,1186 424,1236 |
| validating | failed |  | !(total > 0) | forward | 563,74 563,136 659,136 659,448 509,448 509,900 498,900 498,1226 446,1226 446,1236 |
| validating | payment |  |  | forward | 626,74 626,146 |
| payment | refunding | cancel |  | forward | 427,356 427,428 459,428 459,880 407,880 407,910 |
| charging | fulfilment | done.invoke.charge |  | forward | 296,246 296,418 279,418 279,458 |
| charging | failed | payment.declined |  | forward | 427,246 427,438 489,438 489,890 478,890 478,1216 410,1216 410,1236 |
| charging | backoff | payment.timeout payment.error | `attempt < maxAttempts` | forward | 492,246 492,308 |
| charging | refunding | payment.timeout payment.error |  | forward | 361,246 361,408 469,408 469,840 417,840 417,910 |
| backoff | charging | payment.retry |  | back | 518,308 518,298 557,298 557,246 |
| fulfilment | refunding | shipping.outOfStock |  | forward | 324,788 324,910 |
| fulfilment | delivered | done.invoke.ship |  | forward | 153,788 153,910 |
| picking | handedOver | shipping.handedOver |  | forward | 186,558 186,600 340,600 340,706 277,706 277,716 |
| picking | cancelling | cancel |  | forward | 296,558 296,610 237,610 237,620 |
| cancelling | refunding | shipping.cancelled |  | forward | 264,654 264,660 145,660 145,860 304,860 304,910 |
| cancelling | handedOver | shipping.handedOver |  | forward | 210,654 210,716 |
| requesting | cancelled | refunded | outcome === 'cancelled' | forward | 313,1034 313,1196 305,1196 305,1236 |
| requesting | failed | refunded |  | forward | 409,1034 409,1206 398,1206 398,1236 |
| requesting | waiting | error.communication |  | forward | 345,1034 345,1096 |
| waiting | requesting | refund.retry |  | back | 370,1096 370,1086 377,1086 377,1034 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| handedOver | cancel |  |  |
| refunding | cancel |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| edgesOverlap | payment → refunding on cancel | charging → failed on payment.declined |
| labelOnEdge | charging → backoff on payment.timeout payment.error | validating → failed |
| labelOnEdge | charging → backoff on payment.timeout payment.error | validating → failed |
| labelOnEdge | charging → backoff on payment.timeout payment.error | backoff → charging on payment.retry |
| labelOnEdge | charging → refunding on payment.timeout payment.error | charging → fulfilment on done.invoke.charge |
| labelOnEdge | backoff → charging on payment.retry | validating → failed |
| labelOnEdge | cancelling → handedOver on shipping.handedOver | picking → handedOver on shipping.handedOver |
| labelOnEdge | requesting → waiting on error.communication | requesting → cancelled on refunded |
| labelOnEdge | waiting → requesting on refund.retry | requesting → failed on refunded |
