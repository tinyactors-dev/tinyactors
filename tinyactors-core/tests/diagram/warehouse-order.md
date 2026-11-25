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
| received | atomic | /scxml | atomic | 37 | 40 | 195 | 34 |
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
| boxing | atomic | packing | atomic | 220 | 668 | 70 | 34 |
| labelled | atomic | packing | atomic | 213 | 764 | 85 | 34 |
| shipped | final | /scxml | final | 496 | 156 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| received | fulfilling | accept |  | forward | 154,74 154,156 |
| received | shipped | express |  | forward | 193,74 193,116 535,116 535,156 |
| received | held | hold |  | forward | 47,74 47,156 |
| held | received | release |  | back | 68,156 68,146 73,146 73,74 |
| fulfilling | boxing | packed |  | forward | 287,430 287,472 206,472 206,662 267,662 267,668 |
| locating | picked | found |  | forward | 160,296 160,348 |
| locating | backorder | short |  | back | 200,296 200,406 388,406 388,296 |
| picked | picking | recount |  | toAncestor | 160,382 160,396 |
| drafting | issued | issue |  | forward | 297,296 297,348 |
| backorder | locating | restock |  | back | 419,296 419,416 213,416 213,313 190,313 190,296 |
| weighing | boxing | weighed |  | forward | 288,606 288,658 257,658 257,668 |
| boxing | labelled | boxed |  | forward | 255,702 255,764 |
| labelled | weighing | relabel |  | back | 287,764 287,754 313,754 313,648 248,648 248,606 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| packing | restart |  | boxing |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | picked → picking on recount | locating → backorder on short |
| labelOnEdge | picked → picking on recount | backorder → locating on restock |
| labelAstray | backorder → locating on restock |  |
