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
| active | ended | cancel |  | forward | 1559,197 1932,197 |
| active | ended | deadline.passed |  | forward | 1559,207 1890,207 1890,199 1931,199 |
| arming | waiting |  |  | forward | 150,241 194,241 |
| waiting | ended | drain |  | forward | 271,205 277,205 277,337 1840,337 1840,206 1930,206 |
| waiting | running | due |  | forward | 271,215 572,215 |
| waiting | waiting | reschedule | kind === 'recurring' | self | 271,225 287,225 287,251 271,251 |
| waiting | waiting | reschedule |  | self | 271,261 287,261 287,287 271,287 |
| attempting | succeeded | result.ok | _event.sendid === runID | forward | 712,200 1092,200 1092,183 1102,183 |
| attempting | failing | result.failed | _event.sendid === runID | forward | 712,268 1102,268 |
| attempting | failing | error.communication | _event.sendid === runID | forward | 712,278 1102,278 |
| attempting | failing | attempt.timeout |  | forward | 712,288 1102,288 |
| failing | ended |  | draining | forward | 1179,278 1910,278 1910,209 1931,209 |
| failing | backingOff |  | `attempt < maxAttempts` | forward | 1179,298 1421,298 1421,248 1431,248 |
| failing | runEnded |  | kind === 'recurring' | forward | 1179,268 1411,268 1411,192 1431,192 |
| failing | ended |  |  | forward | 1179,288 1920,288 1920,211 1932,211 |
| backingOff | ended | drain |  | forward | 1531,241 1900,241 1900,207 1931,207 |
| backingOff | attempting | retry |  | back | 1431,237 1391,237 1391,243 1082,243 1082,226 712,226 |
| succeeded | ended |  | draining | forward | 1194,183 1200,183 1200,159 1880,159 1880,201 1931,201 |
| succeeded | runEnded |  | kind === 'recurring' | forward | 1194,175 1431,175 |
| succeeded | ended |  |  | forward | 1194,192 1204,192 1204,159 1860,159 1860,202 1930,202 |
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
| labelOnEdge | attempting → failing on attempt.timeout | waiting → ended on drain |
