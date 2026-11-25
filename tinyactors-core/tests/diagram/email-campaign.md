# Graph

## Charts

### Chart 1: campaign

- **Name**: campaign
- **Width**: 1694
- **Height**: 612

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1694 | 612 |
| starting | atomic | /scxml | atomic | 40 | 224 | 85 | 64 |
| stale | atomic | /scxml | atomic | 359 | 137 | 64 | 58 |
| running | parallel | /scxml | parallel | 599 | 114 | 610 | 484 |
| checkpointing | compound | running | region | 613 | 154 | 582 | 112 |
| saving | atomic | checkpointing | atomic | 653 | 194 | 119 | 58 |
| control | compound | running | region | 613 | 284 | 582 | 88 |
| active | atomic | control | atomic | 653 | 324 | 70 | 34 |
| held | atomic | control | atomic | 884 | 324 | 64 | 34 |
| dispatch | compound | running | region | 613 | 390 | 582 | 194 |
| spawning | atomic | dispatch | atomic | 653 | 430 | 85 | 140 |
| drained | final | dispatch | final | 1104 | 483 | 77 | 34 |
| finished | final | /scxml | final | 1595 | 149 | 85 | 34 |
| aborted | final | /scxml | final | 1595 | 14 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| starting | running |  | !resume | forward | 125,256 339,256 339,279 599,279 |
| starting | running |  | `Date.now() - resume.at < keyTTLMs * 0.8` | forward | 125,272 329,272 329,279 559,279 559,289 599,289 |
| starting | stale |  |  | forward | 125,237 349,237 349,185 359,185 |
| stale | running | campaign.force |  | forward | 423,176 599,176 |
| stale | aborted | campaign.abort |  | forward | 423,156 589,156 589,99 1565,99 1565,27 1595,27 |
| running | finished | done.state.dispatch |  | forward | 1209,374 1585,374 1585,175 1597,175 |
| running | aborted | campaign.abort |  | forward | 1209,221 1575,221 1575,40 1597,40 |
| saving | saving | checkpoint.due |  | self | 772,210 788,210 788,236 772,236 |
| active | held | campaign.pause |  | forward | 722,335 884,335 |
| held | active | campaign.resume |  | back | 884,346 722,346 |
| spawning | drained |  | finished() | forward | 738,446 1094,446 1094,491 1106,491 |
| spawning | spawning | done.spawn |  | self | 738,456 754,456 754,482 738,482 |
| spawning | spawning | error.communication | spawnFailed(_event.sendid) | self | 738,492 754,492 754,518 738,518 |
| spawning | spawning | poll wake |  | self | 738,528 754,528 754,554 738,554 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| saving | store.saved |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| notCentred | starting → stale | stale |
| notCentred | running → finished on done.state.dispatch | finished |
| notCentred | spawning → drained | drained |
| labelOnEdge | starting → running | starting → running |
