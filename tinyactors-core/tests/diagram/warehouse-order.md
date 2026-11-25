# Graph

## Charts

### Chart 1: warehouse-order

- **Name**: warehouse-order
- **Width**: 587
- **Height**: 826

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 587 | 826 |
| received | atomic | /scxml | atomic | 25 | 40 | 195 | 34 |
| held | atomic | /scxml | atomic | 14 | 156 | 64 | 34 |
| fulfilling | parallel | /scxml | parallel | 96 | 156 | 382 | 274 |
| picking | compound | fulfilling | region | 110 | 196 | 113 | 200 |
| locating | atomic | picking | atomic | 124 | 262 | 85 | 34 |
| picked | atomic | picking | atomic | 125 | 348 | 70 | 34 |
| invoicing | compound | fulfilling | region | 241 | 196 | 223 | 200 |
| drafting | atomic | invoicing | atomic | 255 | 262 | 85 | 34 |
| issued | atomic | invoicing | atomic | 262 | 348 | 70 | 34 |
| backorder | atomic | invoicing | atomic | 358 | 262 | 92 | 34 |
| packing | compound | /scxml | compound | 199 | 482 | 177 | 330 |
| weighing | atomic | packing | atomic | 242 | 572 | 85 | 34 |
| boxing | atomic | packing | atomic | 218 | 668 | 70 | 34 |
| labelled | atomic | packing | atomic | 242 | 764 | 85 | 34 |
| shipped | final | /scxml | final | 496 | 156 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| received | fulfilling | accept |  | forward | 142,74 142,156 |
| received | shipped | express |  | forward | 181,74 181,116 535,116 535,156 |
| received | held | hold |  | forward | 35,74 35,156 |
| held | received | release |  | back | 57,156 57,146 61,146 61,74 |
| fulfilling | boxing | packed |  | forward | 287,430 287,472 206,472 206,662 265,662 265,668 |
| locating | picked | found |  | forward | 160,296 160,348 |
| locating | backorder | short |  | back | 200,296 200,406 388,406 388,296 |
| picked | picking | recount |  | toAncestor | 160,382 160,396 |
| drafting | issued | issue |  | forward | 297,296 297,348 |
| backorder | locating | restock |  | back | 419,296 419,416 213,416 213,313 190,313 190,296 |
| weighing | boxing | weighed |  | forward | 255,606 255,668 |
| boxing | labelled | boxed |  | forward | 253,702 253,764 |
| labelled | weighing | relabel |  | back | 313,764 313,658 320,658 320,606 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| packing | restart |  | boxing |

#### Problems

_none_
