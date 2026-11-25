# Graph

## Charts

### Chart 1: order

- **Name**: order
- **Width**: 3085
- **Height**: 282

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 3085 | 282 |
| validating | atomic | /scxml | atomic | 40 | 204 | 100 | 64 |
| payment | compound | /scxml | compound | 346 | 136 | 637 | 88 |
| charging | atomic | payment | atomic | 386 | 176 | 85 | 34 |
| backoff | atomic | payment | atomic | 892 | 176 | 77 | 34 |
| fulfilment | compound | /scxml | compound | 1281 | 47 | 711 | 132 |
| picking | atomic | fulfilment | atomic | 1321 | 96 | 77 | 58 |
| cancelling | atomic | fulfilment | atomic | 1588 | 99 | 100 | 34 |
| handedOver | atomic | fulfilment | atomic | 1878 | 87 | 100 | 58 |
| refunding | compound | /scxml | compound | 2242 | 92 | 421 | 112 |
| requesting | atomic | refunding | atomic | 2282 | 156 | 100 | 34 |
| waiting | atomic | refunding | atomic | 2572 | 156 | 77 | 34 |
| delivered | final | /scxml | final | 2242 | 14 | 92 | 34 |
| cancelled | final | /scxml | final | 2978 | 130 | 92 | 34 |
| failed | final | /scxml | final | 2978 | 183 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| validating | failed |  | items.length === 0 | forward | 140,236 326,236 326,252 1261,252 1261,257 2222,257 2222,252 2958,252 2958,202 2978,202 |
| validating | failed |  | !(total > 0) | forward | 140,252 1271,252 1271,257 2232,257 2232,252 2968,252 2968,205 2979,205 |
| validating | payment |  |  | forward | 140,214 346,214 |
| payment | refunding | cancel |  | forward | 983,193 1241,193 1241,207 2202,207 2202,182 2242,182 |
| charging | fulfilment | done.invoke.charge |  | forward | 471,182 477,182 477,169 1231,169 1231,126 1281,126 |
| charging | failed | payment.declined |  | forward | 471,204 481,204 481,217 1251,217 1251,232 2212,232 2212,227 2948,227 2948,198 2978,198 |
| charging | backoff | payment.timeout payment.error | `attempt < maxAttempts` | forward | 471,193 882,193 882,189 892,189 |
| charging | refunding | payment.timeout payment.error |  | forward | 471,187 485,187 485,169 1221,169 1221,207 2162,207 2162,192 2242,192 |
| backoff | charging | payment.retry |  | back | 892,199 471,199 |
| fulfilment | refunding | shipping.outOfStock |  | forward | 1992,152 2242,152 |
| fulfilment | delivered | done.invoke.ship |  | forward | 1992,83 2192,83 2192,31 2242,31 |
| picking | handedOver | shipping.handedOver |  | forward | 1398,106 1578,106 1578,156 1868,156 1868,135 1878,135 |
| picking | cancelling | cancel |  | forward | 1398,116 1588,116 |
| cancelling | refunding | shipping.cancelled |  | forward | 1688,122 1694,122 1694,80 2182,80 2182,167 2242,167 |
| cancelling | handedOver | shipping.handedOver |  | forward | 1688,110 1878,110 |
| requesting | cancelled | refunded | outcome === 'cancelled' | forward | 2382,163 2388,163 2388,149 2938,149 2938,147 2978,147 |
| requesting | failed | refunded |  | forward | 2382,184 2392,184 2392,197 2928,197 2928,195 2979,195 |
| requesting | waiting | error.communication |  | forward | 2382,170 2572,170 |
| waiting | requesting | refund.retry |  | back | 2572,179 2552,179 2552,177 2382,177 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| handedOver | cancel |  |  |
| refunding | cancel |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | backoff → charging on payment.retry | charging → failed on payment.declined |
| labelOnEdge | requesting → waiting on error.communication | waiting → requesting on refund.retry |
| labelOnEdge | waiting → requesting on refund.retry | requesting → failed on refunded |
