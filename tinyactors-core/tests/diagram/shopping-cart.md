# Graph

## Charts

### Chart 1: cart

- **Name**: cart
- **Width**: 2670
- **Height**: 495

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 2670 | 495 |
| cart | compound | /scxml | compound | 40 | 14 | 2196 | 467 |
| shopping | atomic | cart | atomic | 80 | 219 | 282 | 82 |
| checkingOut | atomic | cart | atomic | 754 | 253 | 174 | 146 |
| compensating | atomic | cart | atomic | 1340 | 175 | 282 | 82 |
| releaseBackoff | atomic | cart | atomic | 1940 | 150 | 282 | 82 |
| retrying | atomic | cart | atomic | 1940 | 250 | 85 | 34 |
| reserving | atomic | cart | atomic | 80 | 330 | 282 | 136 |
| placed | final | /scxml | final | 2564 | 227 | 70 | 34 |
| failed | final | /scxml | final | 2564 | 279 | 70 | 34 |
| cancelled | final | /scxml | final | 2564 | 331 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| shopping | checkingOut | checkout | Object.keys(lines).length > 0 && Object.values(lines).every(function (line) { return line.status === 'held'; }) | forward | 362,274 754,274 |
| shopping | compensating | cancel |  | forward | 362,247 684,247 684,238 1240,238 1240,184 1340,184 |
| checkingOut | checkingOut | confirmed | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation && lines[_event.data.sku].status === 'confirming' | self | 928,338 944,338 944,364 928,364 |
| checkingOut | compensating | confirm.refused | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation | forward | 928,288 1250,288 1250,192 1340,192 |
| checkingOut | compensating | error.communication |  | forward | 928,298 1260,298 1260,200 1340,200 |
| checkingOut | compensating | checkout.timeout |  | forward | 928,308 1270,308 1270,208 1340,208 |
| checkingOut | compensating | cancel |  | forward | 928,318 1280,318 1280,216 1340,216 |
| checkingOut | placed |  | Object.values(lines).every(function (line) { return line.status === 'confirmed'; }) | forward | 928,328 2554,328 2554,244 2564,244 |
| compensating | releaseBackoff | error.communication release.timeout |  | forward | 1622,189 1940,189 |
| compensating | cancelled |  | Object.values(lines).every(function (line) { return line.status === 'released'; }) && cancelled | forward | 1622,244 2534,244 2534,342 2565,342 |
| compensating | retrying |  | `Object.values(lines).every(function (line) { return line.status === 'released'; }) && retry && attempt < maxAttempts` | forward | 1622,218 1900,218 1900,260 1940,260 |
| compensating | failed |  | Object.values(lines).every(function (line) { return line.status === 'released'; }) | forward | 1622,230 1628,230 1628,143 2544,143 2544,296 2564,296 |
| releaseBackoff | compensating | release.again |  | back | 1940,203 1622,203 |
| retrying | reserving | retry |  | back | 1940,274 1930,274 1930,311 1330,311 1330,462 734,462 734,427 362,427 |
| retrying | cancelled | cancel |  | forward | 2025,267 2524,267 2524,353 2565,353 |
| reserving | compensating | reserve.refused | lines[_event.data.sku] !== undefined && lines[_event.data.sku].reservation === _event.data.reservation | forward | 362,349 694,349 694,432 1290,432 1290,225 1340,225 |
| reserving | compensating | error.communication |  | forward | 362,369 704,369 704,432 1300,432 1300,233 1340,233 |
| reserving | compensating | reserving.timeout |  | forward | 362,388 714,388 714,432 1310,432 1310,241 1340,241 |
| reserving | compensating | cancel |  | forward | 362,407 724,407 724,432 1320,432 1320,249 1340,249 |
| reserving | checkingOut |  | Object.values(lines).every(function (line) { return line.status === 'held'; }) | forward | 362,442 744,442 744,389 754,389 |

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

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | checkingOut → compensating on error.communication | checkingOut → compensating on confirm.refused |
| labelOnEdge | checkingOut → compensating on error.communication | checkingOut → compensating on checkout.timeout |
| labelOnEdge | checkingOut → compensating on error.communication | checkingOut → compensating on cancel |
| labelOnEdge | checkingOut → compensating on error.communication | reserving → compensating on reserve.refused |
| labelOnEdge | checkingOut → compensating on error.communication | reserving → compensating on error.communication |
| labelOnEdge | checkingOut → compensating on error.communication | reserving → compensating on reserving.timeout |
| labelOnEdge | checkingOut → compensating on error.communication | reserving → compensating on cancel |
| labelOnEdge | checkingOut → compensating on checkout.timeout | shopping → compensating on cancel |
| labelOnEdge | checkingOut → compensating on checkout.timeout | checkingOut → compensating on confirm.refused |
| labelOnEdge | checkingOut → compensating on checkout.timeout | checkingOut → compensating on error.communication |
| labelOnEdge | checkingOut → compensating on checkout.timeout | checkingOut → compensating on cancel |
| labelOnEdge | checkingOut → compensating on checkout.timeout | reserving → compensating on reserve.refused |
| labelOnEdge | reserving → compensating on error.communication | reserving → compensating on reserve.refused |
| labelOnEdge | reserving → compensating on error.communication | reserving → compensating on reserving.timeout |
| labelOnEdge | reserving → compensating on error.communication | reserving → compensating on cancel |
| labelOnEdge | reserving → compensating on error.communication | reserving → checkingOut |
