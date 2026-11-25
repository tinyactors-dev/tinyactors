# Graph

## Charts

### Chart 1: warehouse-order

- **Name**: warehouse-order
- **Width**: 1215
- **Height**: 447

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1215 | 447 |
| received | atomic | /scxml | atomic | 40 | 32 | 85 | 84 |
| held | atomic | /scxml | atomic | 253 | 29 | 64 | 34 |
| fulfilling | parallel | /scxml | parallel | 253 | 81 | 348 | 300 |
| picking | compound | fulfilling | region | 267 | 121 | 300 | 88 |
| locating | atomic | picking | atomic | 307 | 161 | 85 | 34 |
| picked | atomic | picking | atomic | 476 | 161 | 70 | 34 |
| invoicing | compound | fulfilling | region | 267 | 227 | 300 | 140 |
| drafting | atomic | invoicing | atomic | 307 | 267 | 85 | 34 |
| issued | atomic | invoicing | atomic | 483 | 267 | 70 | 34 |
| backorder | atomic | invoicing | atomic | 307 | 319 | 92 | 34 |
| packing | compound | /scxml | compound | 692 | 149 | 509 | 140 |
| weighing | atomic | packing | atomic | 732 | 227 | 85 | 34 |
| boxing | atomic | packing | atomic | 925 | 213 | 70 | 34 |
| labelled | atomic | packing | atomic | 1102 | 218 | 85 | 34 |
| shipped | final | /scxml | final | 253 | 399 | 77 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| received | fulfilling | accept |  | forward | 125,96 223,96 223,117 253,117 |
| received | shipped | express |  | forward | 125,106 213,106 213,416 253,416 |
| received | held | hold |  | forward | 125,42 253,42 |
| held | received | release |  | back | 253,53 125,53 |
| fulfilling | boxing | packed |  | forward | 601,244 682,244 682,225 925,225 |
| locating | picked | found |  | forward | 392,170 466,170 466,178 476,178 |
| locating | backorder | short |  | back | 392,187 403,187 403,205 577,205 577,331 399,331 |
| picked | picking | recount |  | toAncestor | 545,178 567,178 |
| drafting | issued | issue |  | forward | 392,284 483,284 |
| backorder | locating | restock |  | back | 399,342 587,342 587,200 408,200 408,178 392,178 |
| weighing | boxing | weighed |  | forward | 817,237 925,237 |
| boxing | labelled | boxed |  | forward | 994,230 1102,230 |
| labelled | weighing | relabel |  | back | 1102,242 1092,242 1092,270 915,270 915,250 817,250 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| packing | restart |  | boxing |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | picked → picking on recount | backorder → locating on restock |
