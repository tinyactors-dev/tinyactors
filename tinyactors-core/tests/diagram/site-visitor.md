# Graph

## Charts

### Chart 1: visitor

- **Name**: visitor
- **Width**: 514
- **Height**: 860

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 514 | 860 |
| here | compound | /scxml | compound | 14 | 40 | 486 | 720 |
| awake | compound | here | compound | 28 | 106 | 458 | 520 |
| roaming | atomic | awake | atomic | 168 | 408 | 212 | 34 |
| eating | atomic | awake | atomic | 205 | 146 | 249 | 34 |
| approaching | atomic | awake | atomic | 56 | 262 | 318 | 34 |
| greeting | atomic | awake | atomic | 42 | 554 | 99 | 58 |
| greeted | atomic | awake | atomic | 187 | 554 | 155 | 58 |
| asleep | atomic | here | atomic | 148 | 688 | 218 | 58 |
| gone | final | /scxml | final | 225 | 812 | 64 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| here | gone | leave |  | forward | 257,760 257,812 |
| awake | asleep | hide |  | forward | 236,626 236,688 |
| roaming | roaming | arrive |  | self | 310,442 310,458 375,458 375,442 |
| roaming | eating | found |  | back | 308,408 308,388 412,388 412,242 354,242 354,180 |
| roaming | approaching | greet |  | back | 240,408 240,368 270,368 270,296 |
| roaming | greeting | bump |  | forward | 178,442 178,524 76,524 76,554 |
| roaming | greeted | greeting |  | forward | 244,442 244,554 |
| eating | roaming | chewed empty |  | forward | 404,180 404,252 397,252 397,398 370,398 370,408 |
| eating | approaching | greet |  | forward | 215,180 215,262 |
| eating | greeted | greeting |  | forward | 304,180 304,222 427,222 427,338 423,338 423,484 332,484 332,554 |
| approaching | approaching | arrive |  | self | 121,296 121,312 186,312 186,296 |
| approaching | greeting | near |  | forward | 86,296 86,358 153,358 153,514 61,514 61,554 |
| approaching | roaming | lost give-up |  | forward | 323,296 323,378 274,378 274,408 |
| approaching | greeted | greeting |  | forward | 222,296 222,348 403,348 403,504 229,504 229,554 |
| greeting | roaming | done |  | back | 131,554 131,534 204,534 204,442 |
| greeted | roaming | done |  | back | 275,554 275,544 283,544 283,442 |
| asleep | awake | show |  | back | 283,688 283,626 |

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

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | eating → roaming on chewed empty | roaming → eating on found |
