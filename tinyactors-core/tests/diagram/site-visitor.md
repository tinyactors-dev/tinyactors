# Graph

## Charts

### Chart 1: visitor

- **Name**: visitor
- **Width**: 1572
- **Height**: 354

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1572 | 354 |
| here | compound | /scxml | compound | 40 | 14 | 1347 | 326 |
| awake | compound | here | compound | 80 | 54 | 958 | 272 |
| roaming | atomic | awake | atomic | 625 | 170 | 77 | 94 |
| eating | atomic | awake | atomic | 94 | 212 | 70 | 84 |
| approaching | atomic | awake | atomic | 326 | 120 | 108 | 120 |
| greeting | atomic | awake | atomic | 869 | 94 | 99 | 58 |
| greeted | atomic | awake | atomic | 869 | 175 | 155 | 58 |
| asleep | atomic | here | atomic | 1155 | 174 | 218 | 58 |
| gone | final | /scxml | final | 1494 | 173 | 64 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| here | gone | leave |  | forward | 1387,190 1494,190 |
| awake | asleep | hide |  | forward | 1038,184 1155,184 |
| roaming | roaming | arrive |  | self | 702,224 718,224 718,250 702,250 |
| roaming | eating | found |  | back | 625,235 605,235 605,275 306,275 306,262 164,262 |
| roaming | approaching | greet |  | back | 625,198 433,198 |
| roaming | greeting | bump |  | forward | 702,180 839,180 839,128 869,128 |
| roaming | greeted | greeting |  | forward | 702,200 869,200 |
| eating | roaming | chewed empty |  | forward | 164,279 316,279 316,275 615,275 615,254 625,254 |
| eating | approaching | greet |  | forward | 164,222 286,222 286,180 326,180 |
| eating | greeted | greeting |  | forward | 164,246 296,246 296,300 595,300 595,307 859,307 859,223 869,223 |
| approaching | approaching | arrive |  | self | 433,157 449,157 449,183 433,183 |
| approaching | greeting | near |  | forward | 433,147 585,147 585,155 829,155 829,113 869,113 |
| approaching | roaming | lost give-up |  | forward | 433,213 625,213 |
| approaching | greeted | greeting |  | forward | 433,188 575,188 575,287 819,287 819,188 869,188 |
| greeting | roaming | done |  | back | 869,142 849,142 849,190 702,190 |
| greeted | roaming | done |  | back | 869,212 702,212 |
| asleep | awake | show |  | back | 1155,222 1038,222 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| greeting | greeting |  |  |
| greeted | greeting |  |  |
| greeted | greet |  |  |
| asleep | greeting |  |  |
| asleep | greet |  |  |
| asleep | arrive |  |  |

#### Problems

_none_
