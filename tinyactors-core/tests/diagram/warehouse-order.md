# Graph

## Charts

### Chart 1: warehouse-order

- **Name**: warehouse-order
- **Width**: 1195
- **Height**: 477

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1195 | 477 |
| received | atomic | /scxml | atomic | 40 | 32 | 85 | 84 |
| held | atomic | /scxml | atomic | 253 | 29 | 64 | 34 |
| fulfilling | parallel | /scxml | parallel | 253 | 81 | 328 | 330 |
| picking | compound | fulfilling | region | 267 | 121 | 300 | 88 |
| locating | atomic | picking | atomic | 307 | 161 | 85 | 34 |
| picked | atomic | picking | atomic | 476 | 161 | 70 | 34 |
| invoicing | compound | fulfilling | region | 267 | 227 | 300 | 140 |
| drafting | atomic | invoicing | atomic | 307 | 267 | 85 | 34 |
| issued | atomic | invoicing | atomic | 483 | 267 | 70 | 34 |
| backorder | atomic | invoicing | atomic | 307 | 319 | 92 | 34 |
| packing | compound | /scxml | compound | 672 | 164 | 509 | 140 |
| weighing | atomic | packing | atomic | 712 | 242 | 85 | 34 |
| boxing | atomic | packing | atomic | 905 | 228 | 70 | 34 |
| labelled | atomic | packing | atomic | 1082 | 233 | 85 | 34 |
| shipped | final | /scxml | final | 253 | 429 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| received | fulfilling | accept |  | forward | 125,96 223,96 223,117 253,117 |
| received | shipped | express |  | forward | 125,106 213,106 213,446 253,446 |
| received | held | hold |  | forward | 125,42 253,42 |
| held | received | release |  | back | 253,53 125,53 |
| fulfilling | boxing | packed |  | forward | 581,259 662,259 662,240 905,240 |
| locating | picked | found |  | forward | 392,178 476,178 |
| locating | backorder | short |  | back | 335,195 335,384 338,384 338,353 |
| picked | picking | recount |  | toAncestor | 545,178 567,178 |
| drafting | issued | issue |  | forward | 392,284 483,284 |
| backorder | locating | restock |  | back | 368,353 368,394 363,394 363,195 |
| weighing | boxing | weighed |  | forward | 797,252 905,252 |
| boxing | labelled | boxed |  | forward | 974,245 1082,245 |
| labelled | weighing | relabel |  | back | 1082,257 1072,257 1072,285 895,285 895,265 797,265 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| packing | restart |  | boxing |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| throughBox | locating → backorder on short | drafting |
| throughBox | locating → backorder on short | backorder |
| throughBox | backorder → locating on restock | drafting |
| throughBox | backorder → locating on restock | backorder |
