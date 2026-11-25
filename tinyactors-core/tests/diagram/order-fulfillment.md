# Graph

## Charts

### Chart 1: order

- **Name**: order
- **Width**: 3085
- **Height**: 289

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 3085 | 289 |
| validating | atomic | /scxml | atomic | 40 | 209 | 100 | 64 |
| payment | compound | /scxml | compound | 346 | 149 | 637 | 88 |
| charging | atomic | payment | atomic | 386 | 189 | 85 | 34 |
| backoff | atomic | payment | atomic | 892 | 189 | 77 | 34 |
| fulfilment | compound | /scxml | compound | 1281 | 50 | 711 | 132 |
| picking | atomic | fulfilment | atomic | 1321 | 110 | 77 | 58 |
| cancelling | atomic | fulfilment | atomic | 1588 | 102 | 100 | 34 |
| handedOver | atomic | fulfilment | atomic | 1878 | 90 | 100 | 58 |
| refunding | compound | /scxml | compound | 2242 | 102 | 421 | 112 |
| requesting | atomic | refunding | atomic | 2282 | 166 | 100 | 34 |
| waiting | atomic | refunding | atomic | 2572 | 166 | 77 | 34 |
| delivered | final | /scxml | final | 2242 | 14 | 92 | 34 |
| cancelled | final | /scxml | final | 2978 | 105 | 92 | 34 |
| failed | final | /scxml | final | 2978 | 177 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| validating | failed |  | items.length === 0 | forward | 140,241 326,241 326,265 1261,265 1261,260 2222,260 2222,262 2958,262 2958,197 2978,197 |
| validating | failed |  | !(total > 0) | forward | 140,257 336,257 336,265 1271,265 1271,260 2232,260 2232,262 2968,262 2968,204 2978,204 |
| validating | payment |  |  | forward | 140,225 346,225 |
| payment | refunding | cancel |  | forward | 983,206 1251,206 1251,210 2212,210 2212,191 2242,191 |
| charging | fulfilment | done.invoke.charge |  | forward | 471,195 477,195 477,182 1231,182 1231,129 1281,129 |
| charging | failed | payment.declined |  | forward | 471,206 481,206 481,182 1241,182 1241,235 2202,235 2202,237 2948,237 2948,190 2978,190 |
| charging | backoff | payment.timeout payment.error | `attempt < maxAttempts` | forward | 471,212 872,212 872,202 892,202 |
| charging | refunding | payment.timeout payment.error |  | forward | 471,200 485,200 485,182 1221,182 1221,210 2162,210 2162,201 2242,201 |
| backoff | charging | payment.retry |  | back | 892,213 882,213 882,217 471,217 |
| fulfilment | refunding | shipping.outOfStock |  | forward | 1992,159 2172,159 2172,162 2242,162 |
| fulfilment | delivered | done.invoke.ship |  | forward | 1992,98 2192,98 2192,40 2242,40 |
| picking | handedOver | shipping.handedOver |  | forward | 1398,129 1568,129 1568,159 1858,159 1858,138 1878,138 |
| picking | cancelling | cancel |  | forward | 1398,144 1578,144 1578,126 1588,126 |
| cancelling | refunding | shipping.cancelled |  | forward | 1688,124 1694,124 1694,83 2182,83 2182,177 2242,177 |
| cancelling | handedOver | shipping.handedOver |  | forward | 1688,113 1878,113 |
| requesting | cancelled | refunded | outcome === 'cancelled' | forward | 2382,173 2388,173 2388,159 2928,159 2928,122 2978,122 |
| requesting | failed | refunded |  | forward | 2382,193 2392,193 2392,207 2938,207 2938,184 2978,184 |
| requesting | waiting | error.communication |  | forward | 2382,179 2572,179 |
| waiting | requesting | refund.retry |  | back | 2572,189 2552,189 2552,186 2382,186 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| handedOver | cancel |  |  |
| refunding | cancel |  |  |
