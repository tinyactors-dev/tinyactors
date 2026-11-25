# Graph

## Charts

### Chart 1: job

- **Name**: job
- **Width**: 809
- **Height**: 990

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 809 | 990 |
| active | compound | /scxml | compound | 14 | 40 | 781 | 770 |
| arming | atomic | active | atomic | 370 | 130 | 70 | 34 |
| waiting | atomic | active | atomic | 198 | 208 | 412 | 34 |
| running | compound | active | compound | 28 | 304 | 753 | 492 |
| attempting | atomic | running | atomic | 164 | 418 | 480 | 58 |
| failing | atomic | running | atomic | 406 | 632 | 361 | 34 |
| backingOff | atomic | running | atomic | 328 | 748 | 100 | 34 |
| succeeded | atomic | running | atomic | 154 | 632 | 180 | 34 |
| runEnded | atomic | running | atomic | 166 | 748 | 85 | 34 |
| ended | final | /scxml | final | 327 | 942 | 154 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| active | ended | cancel |  | forward | 402,810 402,942 |
| active | ended | deadline.passed |  | forward | 498,810 498,912 418,912 418,942 |
| arming | waiting |  |  | forward | 404,164 404,208 |
| waiting | ended | drain |  | forward | 224,242 224,248 21,248 21,892 428,892 428,942 |
| waiting | running | due |  | forward | 241,242 241,304 |
| waiting | waiting | reschedule | kind === 'recurring' | self | 254,242 254,258 503,258 503,242 |
| waiting | waiting | reschedule |  | self | 513,242 513,258 605,258 605,242 |
| attempting | succeeded | result.ok | _event.sendid === runID | forward | 244,476 244,632 |
| attempting | failing | result.failed | _event.sendid === runID | forward | 404,476 404,602 416,602 416,632 |
| attempting | failing | error.communication | _event.sendid === runID | forward | 484,476 484,632 |
| attempting | failing | attempt.timeout |  | forward | 564,476 564,632 |
| failing | ended |  | draining | forward | 622,666 622,922 440,922 440,942 |
| failing | backingOff |  | `attempt < maxAttempts` | forward | 550,666 550,738 418,738 418,748 |
| failing | runEnded |  | kind === 'recurring' | forward | 478,666 478,728 241,728 241,748 |
| failing | ended |  |  | forward | 695,666 695,932 452,932 452,942 |
| backingOff | ended | drain |  | forward | 378,782 378,862 392,862 392,942 |
| backingOff | attempting | retry |  | back | 385,748 385,708 391,708 391,612 324,612 324,476 |
| succeeded | ended |  | draining | forward | 244,666 244,672 35,672 35,882 368,882 368,942 |
| succeeded | runEnded |  | kind === 'recurring' | forward | 199,666 199,748 |
| succeeded | ended |  |  | forward | 289,666 289,872 380,872 380,942 |
| runEnded | ended |  | limit !== 0 && runs + skipped >= limit | forward | 209,782 209,902 356,902 356,942 |
| runEnded | waiting |  |  | back | 209,748 209,738 35,738 35,294 207,294 207,242 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| active | clock | !draining |  |
| running | due |  |  |
| running | drain |  |  |
| running | reschedule | kind === 'recurring' |  |
| attempting | result |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | attempting → succeeded on result.ok | backingOff → attempting on retry |
| labelOnEdge | attempting → failing on result.failed | attempting → failing on error.communication |
| labelOnEdge | attempting → failing on result.failed | backingOff → attempting on retry |
| labelOnEdge | attempting → failing on error.communication | attempting → failing on result.failed |
| labelOnEdge | attempting → failing on error.communication | attempting → failing on attempt.timeout |
