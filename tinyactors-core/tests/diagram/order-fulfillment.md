# Graph

## Charts

### Chart 1: order

- **Name**: order
- **Width**: 740
- **Height**: 1284

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 740 | 1284 |
| validating | atomic | /scxml | atomic | 408 | 40 | 292 | 34 |
| payment | compound | /scxml | compound | 212 | 146 | 419 | 210 |
| charging | atomic | payment | atomic | 226 | 212 | 391 | 34 |
| backoff | atomic | payment | atomic | 445 | 308 | 77 | 34 |
| fulfilment | compound | /scxml | compound | 138 | 458 | 272 | 330 |
| picking | atomic | fulfilment | atomic | 171 | 524 | 225 | 34 |
| cancelling | atomic | fulfilment | atomic | 152 | 620 | 160 | 34 |
| handedOver | atomic | fulfilment | atomic | 182 | 716 | 100 | 58 |
| refunding | compound | /scxml | compound | 262 | 910 | 188 | 234 |
| requesting | atomic | refunding | atomic | 276 | 1000 | 160 | 34 |
| waiting | atomic | refunding | atomic | 317 | 1096 | 77 | 34 |
| delivered | final | /scxml | final | 102 | 910 | 92 | 34 |
| cancelled | final | /scxml | final | 254 | 1236 | 92 | 34 |
| failed | final | /scxml | final | 364 | 1236 | 94 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| validating | failed |  | items.length === 0 | forward | 418,74 418,126 659,126 659,398 509,398 509,830 498,830 498,1186 419,1186 419,1236 |
| validating | failed |  | !(total > 0) | forward | 558,74 558,136 659,136 659,448 509,448 509,900 498,900 498,1226 441,1226 441,1236 |
| validating | payment |  |  | forward | 621,74 621,146 |
| payment | refunding | cancel |  | forward | 422,356 422,428 459,428 459,880 402,880 402,910 |
| charging | fulfilment | done.invoke.charge |  | forward | 291,246 291,418 274,418 274,458 |
| charging | failed | payment.declined |  | forward | 422,246 422,438 484,438 484,890 473,890 473,1216 405,1216 405,1236 |
| charging | backoff | payment.timeout payment.error | `attempt < maxAttempts` | forward | 487,246 487,308 |
| charging | refunding | payment.timeout payment.error |  | forward | 356,246 356,408 459,408 459,840 412,840 412,910 |
| backoff | charging | payment.retry |  | back | 513,308 513,298 552,298 552,246 |
| fulfilment | refunding | shipping.outOfStock |  | forward | 319,788 319,910 |
| fulfilment | delivered | done.invoke.ship |  | forward | 148,788 148,910 |
| picking | handedOver | shipping.handedOver |  | forward | 181,558 181,600 335,600 335,706 272,706 272,716 |
| picking | cancelling | cancel |  | forward | 291,558 291,610 232,610 232,620 |
| cancelling | refunding | shipping.cancelled |  | forward | 259,654 259,660 145,660 145,860 299,860 299,910 |
| cancelling | handedOver | shipping.handedOver |  | forward | 205,654 205,716 |
| requesting | cancelled | refunded | outcome === 'cancelled' | forward | 308,1034 308,1196 300,1196 300,1236 |
| requesting | failed | refunded |  | forward | 404,1034 404,1206 393,1206 393,1236 |
| requesting | waiting | error.communication |  | forward | 340,1034 340,1096 |
| waiting | requesting | refund.retry |  | back | 372,1096 372,1034 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| handedOver | cancel |  |  |
| refunding | cancel |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| edgesOverlap | validating → failed | validating → failed |
| edgesOverlap | payment → refunding on cancel | charging → failed on payment.declined |
| edgesOverlap | payment → refunding on cancel | charging → refunding on payment.timeout payment.error |
| labelOnEdge | charging → backoff on payment.timeout payment.error | validating → failed |
| labelOnEdge | charging → backoff on payment.timeout payment.error | validating → failed |
| labelOnEdge | charging → backoff on payment.timeout payment.error | charging → failed on payment.declined |
| labelOnEdge | charging → backoff on payment.timeout payment.error | charging → refunding on payment.timeout payment.error |
| labelOnEdge | charging → backoff on payment.timeout payment.error | backoff → charging on payment.retry |
| labelOnEdge | backoff → charging on payment.retry | validating → failed |
| labelOnEdge | backoff → charging on payment.retry | validating → failed |
| labelOnEdge | cancelling → handedOver on shipping.handedOver | cancelling → refunding on shipping.cancelled |
| labelOnEdge | requesting → waiting on error.communication | requesting → cancelled on refunded |
| labelOnEdge | requesting → waiting on error.communication | requesting → failed on refunded |
| labelOnEdge | requesting → waiting on error.communication | waiting → requesting on refund.retry |
| labelOnEdge | waiting → requesting on refund.retry | requesting → failed on refunded |
| labelOnEdge | waiting → requesting on refund.retry | requesting → waiting on error.communication |
