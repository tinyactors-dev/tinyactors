# Graph

## Charts

### Chart 1: ticket

- **Name**: ticket
- **Width**: 1102
- **Height**: 284

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1102 | 284 |
| open | compound | /scxml | compound | 457 | 62 | 478 | 136 |
| triage | atomic | open | atomic | 541 | 126 | 126 | 58 |
| working | atomic | open | atomic | 758 | 126 | 163 | 58 |
| hist | deep history | open | history | 471 | 142 | 26 | 26 |
| escalated | parallel | /scxml | parallel | 14 | 14 | 349 | 256 |
| review | compound | escalated | region | 28 | 54 | 321 | 88 |
| waiting | atomic | review | atomic | 68 | 94 | 77 | 34 |
| approved | final | review | final | 250 | 94 | 85 | 34 |
| sla | compound | escalated | region | 28 | 160 | 321 | 96 |
| ticking | atomic | sla | atomic | 68 | 200 | 77 | 42 |
| closed | final | /scxml | final | 1019 | 138 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| open | closed | close |  | forward | 935,155 1019,155 |
| triage | working | assign |  | forward | 667,155 758,155 |
| hist | triage |  |  | forward | 497,155 541,155 |
| escalated | hist | back |  | forward | 363,155 471,155 |
| waiting | approved | reviewed |  | forward | 145,111 250,111 |
| ticking | ticking | tick |  | self | 145,208 161,208 161,234 145,234 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| open | ping |  |  |
| triage | note | `n < 3` |  |
| working | escalate |  | waiting |

#### Problems

_none_
