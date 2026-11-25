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
| arming | atomic | active | atomic | 80 | 187 | 70 | 34 |
| waiting | atomic | active | atomic | 194 | 152 | 77 | 104 |
| running | compound | active | compound | 572 | 78 | 973 | 252 |
| attempting | atomic | running | atomic | 612 | 190 | 100 | 110 |
| failing | atomic | running | atomic | 1102 | 258 | 77 | 58 |
| backingOff | atomic | running | atomic | 1431 | 224 | 100 | 34 |
| succeeded | atomic | running | atomic | 1102 | 166 | 92 | 34 |
| runEnded | atomic | running | atomic | 1431 | 166 | 85 | 34 |
| ended | final | /scxml | final | 1930 | 162 | 64 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| active | ended | cancel |  | forward | 1559,165 1930,165 |
| active | ended | deadline.passed |  | forward | 1559,209 1890,209 1890,169 1930,169 |
| arming | waiting |  |  | forward | 150,204 194,204 |
| waiting | ended | drain |  | forward | 271,158 277,158 277,71 1850,71 1850,182 1930,182 |
| waiting | running | due |  | forward | 271,178 572,178 |
| waiting | waiting | reschedule | kind === 'recurring' | self | 271,188 287,188 287,214 271,214 |
| waiting | waiting | reschedule |  | self | 271,224 287,224 287,250 271,250 |
| attempting | succeeded | result.ok | _event.sendid === runID | forward | 712,204 1092,204 1092,190 1102,190 |
| attempting | failing | result.failed | _event.sendid === runID | forward | 712,249 1072,249 1072,268 1102,268 |
| attempting | failing | error.communication | _event.sendid === runID | forward | 712,267 1062,267 1062,278 1102,278 |
| attempting | failing | attempt.timeout |  | forward | 712,285 1052,285 1052,288 1102,288 |
| failing | ended |  | draining | forward | 1179,269 1910,269 1910,189 1930,189 |
| failing | backingOff |  | `attempt < maxAttempts` | forward | 1179,303 1421,303 1421,248 1431,248 |
| failing | runEnded |  | kind === 'recurring' | forward | 1179,292 1411,292 1411,192 1431,192 |
| failing | ended |  |  | forward | 1179,281 1920,281 1920,193 1930,193 |
| backingOff | ended | drain |  | forward | 1531,241 1900,241 1900,186 1930,186 |
| backingOff | attempting | retry |  | back | 1431,237 1391,237 1391,243 1082,243 1082,226 712,226 |
| succeeded | ended |  | draining | forward | 1194,175 1200,175 1200,159 1860,159 1860,172 1930,172 |
| succeeded | runEnded |  | kind === 'recurring' | forward | 1194,192 1401,192 1401,175 1431,175 |
| succeeded | ended |  |  | forward | 1194,183 1204,183 1204,159 1870,159 1870,176 1930,176 |
| runEnded | ended |  | limit !== 0 && runs + skipped >= limit | forward | 1516,183 1880,183 1880,179 1930,179 |
| runEnded | waiting |  |  | back | 1431,183 1417,183 1417,159 562,159 562,168 271,168 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| active | clock | !draining |  |
| running | due |  |  |
| running | drain |  |  |
| running | reschedule | kind === 'recurring' |  |
| attempting | result |  |  |
