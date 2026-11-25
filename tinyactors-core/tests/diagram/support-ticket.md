# Graph

## Charts

### Chart 1: ticket

- **Name**: ticket
- **Width**: 292
- **Height**: 736

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 292 | 736 |
| open | compound | /scxml | compound | 51 | 320 | 191 | 316 |
| triage | atomic | open | atomic | 83 | 454 | 126 | 58 |
| working | atomic | open | atomic | 65 | 564 | 163 | 58 |
| hist | deep history | open | history | 133 | 384 | 26 | 26 |
| escalated | parallel | /scxml | parallel | 14 | 14 | 264 | 254 |
| review | compound | escalated | region | 28 | 54 | 113 | 200 |
| waiting | atomic | review | atomic | 42 | 120 | 85 | 34 |
| approved | final | review | final | 42 | 206 | 85 | 34 |
| sla | compound | escalated | region | 159 | 54 | 105 | 200 |
| ticking | atomic | sla | atomic | 173 | 120 | 77 | 34 |
| closed | final | /scxml | final | 111 | 688 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| open | closed | close |  | forward | 146,636 146,688 |
| triage | working | assign |  | forward | 146,512 146,564 |
| hist | triage |  |  | forward | 146,410 146,454 |
| escalated | hist | back |  | forward | 146,268 146,384 |
| waiting | approved | reviewed |  | forward | 85,154 85,206 |
| ticking | ticking | tick |  | self | 186,154 186,170 238,170 238,154 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| open | ping |  |  |
| triage | note | `n < 3` |  |
| working | escalate |  | waiting |

#### Problems

_none_
