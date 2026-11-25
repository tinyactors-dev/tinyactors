# Graph

## Charts

### Chart 1: warehouse-order

- **Name**: warehouse-order
- **Width**: 1195
- **Height**: 463

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1195 | 463 |
| received | atomic | /scxml | atomic | 40 | 143 | 85 | 84 |
| held | atomic | /scxml | atomic | 253 | 15 | 64 | 34 |
| fulfilling | parallel | /scxml | parallel | 253 | 67 | 328 | 330 |
| picking | compound | fulfilling | region | 267 | 107 | 300 | 88 |
| locating | atomic | picking | atomic | 307 | 147 | 85 | 34 |
| picked | atomic | picking | atomic | 476 | 147 | 70 | 34 |
| invoicing | compound | fulfilling | region | 267 | 213 | 300 | 140 |
| drafting | atomic | invoicing | atomic | 307 | 253 | 85 | 34 |
| issued | atomic | invoicing | atomic | 483 | 253 | 70 | 34 |
| backorder | atomic | invoicing | atomic | 307 | 305 | 92 | 34 |
| packing | compound | /scxml | compound | 672 | 150 | 509 | 140 |
| weighing | atomic | packing | atomic | 712 | 234 | 85 | 34 |
| boxing | atomic | packing | atomic | 905 | 214 | 70 | 34 |
| labelled | atomic | packing | atomic | 1082 | 214 | 85 | 34 |
| shipped | final | /scxml | final | 253 | 415 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| received | fulfilling | accept |  | forward | 125,193 253,193 |
| received | shipped | express |  | forward | 125,214 213,214 213,423 255,423 |
| received | held | hold |  | forward | 125,156 233,156 233,27 253,27 |
| held | received | release |  | back | 253,39 243,39 243,173 125,173 |
| fulfilling | boxing | packed |  | forward | 581,245 662,245 662,225 905,225 |
| locating | picked | found |  | forward | 392,164 476,164 |
| locating | backorder | short |  | back | 335,181 335,370 338,370 338,339 |
| picked | picking | recount |  | toAncestor | 545,164 567,164 |
| drafting | issued | issue |  | forward | 392,270 483,270 |
| backorder | locating | restock |  | back | 368,339 368,380 363,380 363,181 |
| weighing | boxing | weighed |  | forward | 797,244 885,244 885,238 905,238 |
| boxing | labelled | boxed |  | forward | 974,227 1082,227 |
| labelled | weighing | relabel |  | back | 1082,238 1072,238 1072,271 895,271 895,256 797,256 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| packing | restart |  | boxing |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| notCentred | received → shipped on express | shipped |
| throughBox | locating → backorder on short | drafting |
| throughBox | locating → backorder on short | backorder |
| throughBox | backorder → locating on restock | drafting |
| throughBox | backorder → locating on restock | backorder |
| notCentred | boxing → labelled on boxed | boxing |
