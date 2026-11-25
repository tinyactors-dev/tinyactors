# Graph

## Charts

### Chart 1: campaign

- **Name**: campaign
- **Width**: 1694
- **Height**: 608

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1694 | 608 |
| starting | atomic | /scxml | atomic | 40 | 213 | 85 | 64 |
| stale | atomic | /scxml | atomic | 359 | 131 | 64 | 58 |
| running | parallel | /scxml | parallel | 599 | 110 | 610 | 484 |
| checkpointing | compound | running | region | 613 | 150 | 582 | 112 |
| saving | atomic | checkpointing | atomic | 653 | 190 | 119 | 58 |
| control | compound | running | region | 613 | 280 | 582 | 88 |
| active | atomic | control | atomic | 653 | 320 | 70 | 34 |
| held | atomic | control | atomic | 884 | 320 | 64 | 34 |
| dispatch | compound | running | region | 613 | 386 | 582 | 194 |
| spawning | atomic | dispatch | atomic | 653 | 426 | 85 | 140 |
| drained | final | dispatch | final | 1104 | 479 | 77 | 34 |
| finished | final | /scxml | final | 1595 | 143 | 85 | 34 |
| aborted | final | /scxml | final | 1595 | 14 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| starting | running |  | !resume | forward | 125,245 339,245 339,267 599,267 |
| starting | running |  | `Date.now() - resume.at < keyTTLMs * 0.8` | forward | 125,261 329,261 329,267 559,267 559,277 599,277 |
| starting | stale |  |  | forward | 125,226 349,226 349,179 359,179 |
| stale | running | campaign.force |  | forward | 423,169 599,169 |
| stale | aborted | campaign.abort |  | forward | 423,150 589,150 589,95 1565,95 1565,27 1595,27 |
| running | finished | done.state.dispatch |  | forward | 1209,370 1585,370 1585,168 1595,168 |
| running | aborted | campaign.abort |  | forward | 1209,218 1575,218 1575,40 1595,40 |
| saving | saving | checkpoint.due |  | self | 772,206 788,206 788,232 772,232 |
| active | held | campaign.pause |  | forward | 722,332 884,332 |
| held | active | campaign.resume |  | back | 884,343 722,343 |
| spawning | drained |  | finished() | forward | 738,442 1094,442 1094,488 1104,488 |
| spawning | spawning | done.spawn |  | self | 738,452 754,452 754,478 738,478 |
| spawning | spawning | error.communication | spawnFailed(_event.sendid) | self | 738,488 754,488 754,514 738,514 |
| spawning | spawning | poll wake |  | self | 738,524 754,524 754,550 738,550 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| saving | store.saved |  |  |
