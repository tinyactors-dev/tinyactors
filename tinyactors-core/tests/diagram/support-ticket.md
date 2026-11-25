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
| open | compound | /scxml | compound | 457 | 74 | 478 | 136 |
| triage | atomic | open | atomic | 541 | 138 | 126 | 58 |
| working | atomic | open | atomic | 758 | 138 | 163 | 58 |
| hist | deep history | open | history | 471 | 154 | 26 | 26 |
| escalated | parallel | /scxml | parallel | 14 | 14 | 349 | 256 |
| review | compound | escalated | region | 28 | 54 | 321 | 88 |
| waiting | atomic | review | atomic | 68 | 94 | 77 | 34 |
| approved | final | review | final | 250 | 94 | 85 | 34 |
| sla | compound | escalated | region | 28 | 160 | 321 | 96 |
| ticking | atomic | sla | atomic | 68 | 200 | 77 | 42 |
| closed | final | /scxml | final | 1019 | 125 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| open | closed | close |  | forward | 935,151 1019,151 |
| triage | working | assign |  | forward | 667,167 758,167 |
| hist | triage |  |  | forward | 497,167 541,167 |
| escalated | hist | back |  | forward | 363,155 447,155 447,167 471,167 |
| waiting | approved | reviewed |  | forward | 145,111 250,111 |
| ticking | ticking | tick |  | self | 145,208 161,208 161,234 145,234 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| open | ping |  |  |
| triage | note | `n < 3` |  |
| working | escalate |  | waiting |
