# Graph

## Charts

### Chart 1: warehouse-order

- **Name**: warehouse-order
- **Width**: 1195
- **Height**: 462

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1195 | 462 |
| received | atomic | /scxml | atomic | 40 | 139 | 85 | 84 |
| held | atomic | /scxml | atomic | 253 | 14 | 64 | 34 |
| fulfilling | parallel | /scxml | parallel | 253 | 66 | 328 | 330 |
| picking | compound | fulfilling | region | 267 | 106 | 300 | 88 |
| locating | atomic | picking | atomic | 307 | 146 | 85 | 34 |
| picked | atomic | picking | atomic | 476 | 146 | 70 | 34 |
| invoicing | compound | fulfilling | region | 267 | 212 | 300 | 140 |
| drafting | atomic | invoicing | atomic | 307 | 252 | 85 | 34 |
| issued | atomic | invoicing | atomic | 483 | 252 | 70 | 34 |
| backorder | atomic | invoicing | atomic | 307 | 304 | 92 | 34 |
| packing | compound | /scxml | compound | 672 | 161 | 509 | 140 |
| weighing | atomic | packing | atomic | 712 | 245 | 85 | 34 |
| boxing | atomic | packing | atomic | 905 | 225 | 70 | 34 |
| labelled | atomic | packing | atomic | 1082 | 225 | 85 | 34 |
| shipped | final | /scxml | final | 253 | 414 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| received | fulfilling | accept |  | forward | 125,189 253,189 |
| received | shipped | express |  | forward | 125,210 213,210 213,423 253,423 |
| received | held | hold |  | forward | 125,152 233,152 233,27 253,27 |
| held | received | release |  | back | 253,38 243,38 243,169 125,169 |
| fulfilling | boxing | packed |  | forward | 581,244 662,244 662,236 905,236 |
| locating | picked | found |  | forward | 392,163 476,163 |
| locating | backorder | short |  | back | 335,180 335,369 338,369 338,338 |
| picked | picking | recount |  | toAncestor | 545,163 565,163 |
| drafting | issued | issue |  | forward | 392,269 483,269 |
| backorder | locating | restock |  | back | 368,338 368,379 363,379 363,180 |
| weighing | boxing | weighed |  | forward | 797,256 885,256 885,249 905,249 |
| boxing | labelled | boxed |  | forward | 974,239 1082,239 |
| labelled | weighing | relabel |  | back | 1082,249 1072,249 1072,282 895,282 895,268 797,268 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| packing | restart |  | boxing |
