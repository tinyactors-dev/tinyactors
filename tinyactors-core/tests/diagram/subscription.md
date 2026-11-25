# Graph

## Charts

### Chart 1: subscription

- **Name**: subscription
- **Width**: 663
- **Height**: 734

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 663 | 734 |
| subscription | compound | /scxml | compound | 133 | 40 | 517 | 584 |
| active | atomic | subscription | atomic | 173 | 154 | 148 | 58 |
| paused | atomic | subscription | atomic | 147 | 294 | 70 | 34 |
| charging | atomic | subscription | atomic | 234 | 294 | 401 | 34 |
| pastDue | compound | subscription | compound | 372 | 410 | 126 | 200 |
| grace | atomic | pastDue | atomic | 386 | 476 | 98 | 34 |
| suspended | atomic | pastDue | atomic | 388 | 562 | 92 | 34 |
| cancelled | final | /scxml | final | 345 | 686 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| subscription | cancelled | cancel |  | forward | 391,624 391,686 |
| active | charging | renew |  | forward | 276,212 276,294 |
| active | paused | pause |  | forward | 183,212 183,294 |
| paused | active | resume |  | back | 206,294 206,284 212,284 212,212 |
| charging | active | done.invoke.charge |  | back | 310,294 310,212 |
| charging | cancelled | payment.declined | dunningAttempts + 1 >= maxRetries | forward | 301,328 301,666 401,666 401,686 |
| charging | suspended | payment.declined | access === 'off' | forward | 368,328 368,380 379,380 379,556 419,556 419,562 |
| charging | grace | payment.declined |  | forward | 435,328 435,476 |
| pastDue | charging | retry |  | back | 446,410 446,390 502,390 502,328 |
| pastDue | charging | card.updated |  | back | 488,410 488,400 568,400 568,328 |
| grace | suspended | grace.over |  | forward | 435,510 435,562 |

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
| labelAstray | charging → suspended on payment.declined |  |
