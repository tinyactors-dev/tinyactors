# Graph

## Charts

### Chart 1: job

- **Name**: job
- **Width**: 2008
- **Height**: 358

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 2008 | 358 |
| active | compound | /scxml | compound | 40 | 14 | 1519 | 330 |
| arming | atomic | active | atomic | 80 | 224 | 70 | 34 |
| waiting | atomic | active | atomic | 194 | 189 | 77 | 104 |
| running | compound | active | compound | 572 | 78 | 973 | 252 |
| attempting | atomic | running | atomic | 612 | 190 | 100 | 110 |
| failing | atomic | running | atomic | 1102 | 258 | 77 | 58 |
| backingOff | atomic | running | atomic | 1431 | 224 | 100 | 34 |
| succeeded | atomic | running | atomic | 1102 | 166 | 92 | 34 |
| runEnded | atomic | running | atomic | 1431 | 166 | 85 | 34 |
| ended | final | /scxml | final | 1930 | 187 | 64 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| active | ended | cancel |  | forward | 1559,190 1937,190 |
| active | ended | deadline.passed |  | forward | 1559,209 1890,209 1890,194 1934,194 |
| arming | waiting |  |  | forward | 150,241 194,241 |
| waiting | ended | drain |  | forward | 271,205 277,205 277,337 1840,337 1840,207 1931,207 |
| waiting | running | due |  | forward | 271,215 572,215 |
| waiting | waiting | reschedule | kind === 'recurring' | self | 271,225 287,225 287,251 271,251 |
| waiting | waiting | reschedule |  | self | 271,261 287,261 287,287 271,287 |
| attempting | succeeded | result.ok | _event.sendid === runID | forward | 712,204 1092,204 1092,190 1102,190 |
| attempting | failing | result.failed | _event.sendid === runID | forward | 712,249 1072,249 1072,268 1102,268 |
| attempting | failing | error.communication | _event.sendid === runID | forward | 712,267 1062,267 1062,278 1102,278 |
| attempting | failing | attempt.timeout |  | forward | 712,285 1052,285 1052,288 1102,288 |
| failing | ended |  | draining | forward | 1179,281 1910,281 1910,214 1934,214 |
| failing | backingOff |  | `attempt < maxAttempts` | forward | 1179,303 1421,303 1421,248 1431,248 |
| failing | runEnded |  | kind === 'recurring' | forward | 1179,269 1411,269 1411,192 1431,192 |
| failing | ended |  |  | forward | 1179,293 1920,293 1920,218 1937,218 |
| backingOff | ended | drain |  | forward | 1531,241 1900,241 1900,211 1932,211 |
| backingOff | attempting | retry |  | back | 1431,237 1391,237 1391,243 1082,243 1082,226 712,226 |
| succeeded | ended |  | draining | forward | 1194,183 1200,183 1200,159 1880,159 1880,197 1932,197 |
| succeeded | runEnded |  | kind === 'recurring' | forward | 1194,175 1431,175 |
| succeeded | ended |  |  | forward | 1194,192 1204,192 1204,159 1850,159 1850,201 1931,201 |
| runEnded | ended |  | limit !== 0 && runs + skipped >= limit | forward | 1516,183 1870,183 1870,204 1930,204 |
| runEnded | waiting |  |  | back | 1431,183 1417,183 1417,159 562,159 562,195 271,195 |

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
| arrowAlongSide | active → ended on cancel | ended |
| notCentred | active → ended on cancel | ended |
| notCentred | attempting → succeeded on result.ok | succeeded |
| arrowAlongSide | failing → ended | ended |
| notCentred | failing → ended | ended |
| labelOnEdge | active → ended on cancel | runEnded → ended |
| labelOnEdge | attempting → failing on error.communication | attempting → failing on attempt.timeout |
| labelOnEdge | succeeded → ended | succeeded → ended |
| labelOnEdge | runEnded → ended | active → ended on cancel |
| labelOnEdge | runEnded → ended | succeeded → ended |
