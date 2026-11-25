# Graph

## Charts

### Chart 1: campaign

- **Name**: campaign
- **Width**: 1210
- **Height**: 668

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1210 | 668 |
| starting | atomic | /scxml | atomic | 293 | 40 | 286 | 34 |
| stale | atomic | /scxml | atomic | 180 | 146 | 246 | 34 |
| running | parallel | /scxml | parallel | 183 | 262 | 890 | 286 |
| checkpointing | compound | running | region | 197 | 302 | 164 | 232 |
| saving | atomic | checkpointing | atomic | 211 | 368 | 136 | 58 |
| control | compound | running | region | 379 | 302 | 154 | 232 |
| active | atomic | control | atomic | 393 | 368 | 126 | 34 |
| held | atomic | control | atomic | 424 | 464 | 64 | 34 |
| dispatch | compound | running | region | 551 | 302 | 508 | 232 |
| spawning | atomic | dispatch | atomic | 565 | 368 | 480 | 34 |
| drained | final | dispatch | final | 614 | 486 | 77 | 34 |
| finished | final | /scxml | final | 287 | 620 | 85 | 34 |
| aborted | final | /scxml | final | 132 | 620 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| starting | running |  | !resume | forward | 349,74 349,116 474,116 474,262 |
| starting | running |  | `Date.now() - resume.at < keyTTLMs * 0.8` | forward | 486,74 486,136 474,136 474,252 484,252 484,262 |
| starting | stale |  |  | forward | 303,74 303,146 |
| stale | running | campaign.force |  | forward | 363,180 363,262 |
| stale | aborted | campaign.abort |  | forward | 243,180 243,242 168,242 168,620 |
| running | finished | done.state.dispatch |  | forward | 330,548 330,620 |
| running | aborted | campaign.abort |  | forward | 193,548 193,620 |
| saving | saving | checkpoint.due |  | self | 219,426 219,442 339,442 339,426 |
| active | held | campaign.pause |  | forward | 435,402 435,464 |
| held | active | campaign.resume |  | back | 477,464 477,402 |
| spawning | drained |  | finished() | forward | 653,402 653,486 |
| spawning | spawning | done.spawn |  | self | 714,402 714,418 800,418 800,402 |
| spawning | spawning | error.communication | spawnFailed(_event.sendid) | self | 810,402 810,418 896,418 896,402 |
| spawning | spawning | poll wake |  | self | 906,402 906,418 992,418 992,402 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| saving | store.saved |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | active → held on campaign.pause | held → active on campaign.resume |
| labelOnEdge | held → active on campaign.resume | active → held on campaign.pause |
