# Graph

## Charts

### Chart 1: insurance-claim

- **Name**: insurance-claim
- **Width**: 267
- **Height**: 710

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 267 | 710 |
| filed | atomic | /scxml | atomic | 77 | 40 | 71 | 34 |
| assessing | compound | /scxml | compound | 21 | 126 | 183 | 454 |
| documents | compound | assessing | compound | 46 | 356 | 128 | 210 |
| collecting | atomic | documents | atomic | 60 | 422 | 100 | 34 |
| checking | atomic | documents | atomic | 70 | 518 | 85 | 34 |
| investigation | compound | assessing | compound | 35 | 190 | 151 | 114 |
| interview | atomic | investigation | atomic | 49 | 256 | 92 | 34 |
| appeal | atomic | /scxml | atomic | 14 | 662 | 70 | 34 |
| paid | final | /scxml | final | 102 | 662 | 64 | 34 |
| closed | final | /scxml | final | 184 | 662 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| filed | assessing | assess |  | forward | 112,74 112,126 |
| assessing | closed | withdraw |  | forward | 194,580 194,622 218,622 218,662 |
| collecting | checking | complete |  | forward | 105,456 105,518 |
| checking | collecting | missing |  | back | 145,518 145,456 |
| checking | paid | settle |  | forward | 126,552 126,632 134,632 134,662 |
| checking | appeal | appeal |  | forward | 98,552 98,652 37,652 37,662 |
| interview | documents | cleared |  | forward | 95,290 95,346 110,346 110,356 |
| appeal | assessing | upheld |  | back | 73,662 73,580 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| assessing | fraud |  | interview |

#### Problems

_none_
