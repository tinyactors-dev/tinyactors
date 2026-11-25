# Graph

## Charts

### Chart 1: cart

- **Name**: cart
- **Width**: 1140
- **Height**: 1060

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1140 | 1060 |
| cart | compound | /scxml | compound | 84 | 40 | 1042 | 890 |
| shopping | atomic | cart | atomic | 294 | 202 | 320 | 82 |
| checkingOut | atomic | cart | atomic | 405 | 438 | 480 | 58 |
| compensating | atomic | cart | atomic | 98 | 670 | 456 | 82 |
| releaseBackoff | atomic | cart | atomic | 118 | 834 | 282 | 82 |
| retrying | atomic | cart | atomic | 418 | 834 | 85 | 34 |
| reserving | atomic | cart | atomic | 632 | 202 | 480 | 82 |
| placed | final | /scxml | final | 455 | 1012 | 70 | 34 |
| failed | final | /scxml | final | 542 | 1012 | 70 | 34 |
| cancelled | final | /scxml | final | 630 | 1012 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| shopping | checkingOut | checkout | Object.keys(lines).length > 0 && Object.values(lines).every(function (line) { return line.status === 'held'; }) | forward | 486,284 486,438 |
| shopping | compensating | cancel |  | forward | 329,284 329,368 390,368 390,570 179,570 179,670 |
| checkingOut | checkingOut | confirmed | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation && lines[_event.data.sku].status === 'confirming' | self | 624,496 624,512 880,512 880,496 |
| checkingOut | compensating | confirm.refused | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation | forward | 426,496 426,580 225,580 225,670 |
| checkingOut | compensating | error.communication |  | forward | 469,496 469,590 270,590 270,670 |
| checkingOut | compensating | checkout.timeout |  | forward | 512,496 512,600 316,600 316,670 |
| checkingOut | compensating | cancel |  | forward | 555,496 555,610 362,610 362,670 |
| checkingOut | placed |  | Object.values(lines).every(function (line) { return line.status === 'confirmed'; }) | forward | 597,496 597,1002 490,1002 490,1012 |
| compensating | releaseBackoff | error.communication release.timeout |  | forward | 174,752 174,834 |
| compensating | cancelled |  | Object.values(lines).every(function (line) { return line.status === 'released'; }) && cancelled | forward | 478,752 478,758 91,758 91,972 666,972 666,1012 |
| compensating | retrying |  | `Object.values(lines).every(function (line) { return line.status === 'released'; }) && retry && attempt < maxAttempts` | forward | 326,752 326,794 428,794 428,834 |
| compensating | failed |  | Object.values(lines).every(function (line) { return line.status === 'released'; }) | forward | 402,752 402,992 577,992 577,1012 |
| releaseBackoff | compensating | release.again |  | back | 250,834 250,752 |
| retrying | reserving | retry |  | back | 493,834 493,824 597,824 597,660 948,660 948,418 974,418 974,284 |
| retrying | cancelled | cancel |  | forward | 460,868 460,982 686,982 686,1012 |
| reserving | compensating | reserve.refused | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation | forward | 700,284 700,378 918,378 918,620 407,620 407,670 |
| reserving | compensating | error.communication |  | forward | 769,284 769,388 918,388 918,630 453,630 453,670 |
| reserving | compensating | reserving.timeout |  | forward | 837,284 837,398 918,398 918,640 498,640 498,670 |
| reserving | compensating | cancel |  | forward | 906,284 906,408 918,408 918,650 544,650 544,670 |
| reserving | checkingOut |  | Object.values(lines).every(function (line) { return line.status === 'held'; }) | forward | 1043,284 1043,428 875,428 875,438 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| cart | reserved | !Object.values(lines).some(function (line) { return line.reservation === _event.data.reservation; }) |  |
| cart | reserved reserve.refused confirmed confirm.refused released expired reserve.timeout |  |  |
| cart | error.communication |  |  |
| cart | error.execution |  |  |
| shopping | add | lines[_event.data.sku] === undefined |  |
| shopping | add |  |  |
| shopping | reserved | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation && lines[_event.data.sku].status === 'pending' |  |
| shopping | reserve.refused | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation |  |
| shopping | reserve.timeout | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation && lines[_event.data.sku].status === 'pending' |  |
| shopping | error.communication | sends[_event.sendid] !== undefined && lines[sends[_event.sendid]] !== undefined && lines[sends[_event.sendid]].status === 'pending' |  |
| shopping | expired | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation |  |
| shopping | remove | lines[_event.data.sku] !== undefined |  |
| shopping | checkout |  |  |
| checkingOut | add remove checkout |  |  |
| compensating | released expired | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation |  |
| compensating | cancel |  |  |
| compensating | add remove checkout |  |  |
| releaseBackoff | released expired | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation |  |
| releaseBackoff | cancel |  |  |
| reserving | reserved | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation && lines[_event.data.sku].status === 'pending' |  |
| reserving | add remove checkout |  |  |

#### Problems

_none_
