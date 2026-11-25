# Graph

## Charts

### Chart 1: subscription

- **Name**: subscription
- **Width**: 1622
- **Height**: 242

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1622 | 242 |
| subscription | compound | /scxml | compound | 40 | 14 | 1136 | 214 |
| active | atomic | subscription | atomic | 80 | 126 | 148 | 58 |
| paused | atomic | subscription | atomic | 431 | 102 | 70 | 34 |
| charging | atomic | subscription | atomic | 431 | 154 | 85 | 58 |
| pastDue | compound | subscription | compound | 834 | 126 | 328 | 88 |
| grace | atomic | pastDue | atomic | 874 | 166 | 64 | 34 |
| suspended | atomic | pastDue | atomic | 1056 | 166 | 92 | 34 |
| cancelled | final | /scxml | final | 1515 | 141 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| subscription | cancelled | cancel |  | forward | 1176,151 1517,151 |
| active | charging | renew |  | forward | 228,164 431,164 |
| active | paused | pause |  | forward | 228,136 411,136 411,115 431,115 |
| paused | active | resume |  | back | 431,126 421,126 421,146 228,146 |
| charging | active | done.invoke.charge |  | back | 431,174 228,174 |
| charging | cancelled | payment.declined | dunningAttempts + 1 >= maxRetries | forward | 516,164 522,164 522,221 1505,221 1505,161 1515,161 |
| charging | suspended | payment.declined | access === 'off' | forward | 516,193 814,193 814,159 1050,159 1050,177 1056,177 |
| charging | grace | payment.declined |  | forward | 516,202 824,202 824,183 874,183 |
| pastDue | charging | retry |  | back | 834,173 516,173 |
| pastDue | charging | card.updated |  | back | 834,183 516,183 |
| grace | suspended | grace.over |  | forward | 938,183 1046,183 1046,187 1056,187 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| subscription | error.platform.upgrade |  |  |
| subscription | grace.over |  |  |
| active | change |  |  |
| active | coupon |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | pastDue → charging on card.updated | charging → suspended on payment.declined |
