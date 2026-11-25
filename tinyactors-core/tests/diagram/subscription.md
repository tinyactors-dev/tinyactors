# Graph

## Charts

### Chart 1: subscription

- **Name**: subscription
- **Width**: 1622
- **Height**: 255

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1622 | 255 |
| subscription | compound | /scxml | compound | 40 | 14 | 1136 | 227 |
| active | atomic | subscription | atomic | 80 | 122 | 148 | 58 |
| paused | atomic | subscription | atomic | 431 | 102 | 70 | 34 |
| charging | atomic | subscription | atomic | 431 | 154 | 85 | 58 |
| pastDue | compound | subscription | compound | 834 | 139 | 328 | 88 |
| grace | atomic | pastDue | atomic | 874 | 179 | 64 | 34 |
| suspended | atomic | pastDue | atomic | 1056 | 179 | 92 | 34 |
| cancelled | final | /scxml | final | 1515 | 111 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| subscription | cancelled | cancel |  | forward | 1176,131 1495,131 1495,123 1515,123 |
| active | charging | renew |  | forward | 228,158 401,158 401,164 431,164 |
| active | paused | pause |  | forward | 228,133 411,133 411,115 431,115 |
| paused | active | resume |  | back | 431,126 421,126 421,144 228,144 |
| charging | active | done.invoke.charge |  | back | 431,174 391,174 391,169 228,169 |
| charging | cancelled | payment.declined | dunningAttempts + 1 >= maxRetries | forward | 516,164 522,164 522,95 1505,95 1505,133 1515,133 |
| charging | suspended | payment.declined | access === 'off' | forward | 516,193 814,193 814,172 1050,172 1050,190 1056,190 |
| charging | grace | payment.declined |  | forward | 516,202 824,202 824,196 874,196 |
| pastDue | charging | retry |  | back | 834,175 804,175 804,173 516,173 |
| pastDue | charging | card.updated |  | back | 834,185 794,185 794,183 516,183 |
| grace | suspended | grace.over |  | forward | 938,200 1056,200 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| subscription | error.platform.upgrade |  |  |
| subscription | grace.over |  |  |
| active | change |  |  |
| active | coupon |  |  |
