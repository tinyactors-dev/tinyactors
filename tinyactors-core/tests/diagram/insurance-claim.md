# Graph

## Charts

### Chart 1: insurance-claim

- **Name**: insurance-claim
- **Width**: 1044
- **Height**: 218

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 1044 | 218 |
| filed | atomic | /scxml | atomic | 40 | 105 | 64 | 34 |
| assessing | compound | /scxml | compound | 195 | 14 | 631 | 166 |
| documents | compound | assessing | compound | 458 | 78 | 354 | 88 |
| collecting | atomic | documents | atomic | 498 | 118 | 100 | 34 |
| checking | atomic | documents | atomic | 713 | 118 | 85 | 34 |
| investigation | compound | assessing | compound | 209 | 78 | 151 | 88 |
| interview | atomic | investigation | atomic | 249 | 118 | 92 | 34 |
| appeal | atomic | /scxml | atomic | 961 | 66 | 70 | 34 |
| paid | final | /scxml | final | 961 | 118 | 64 | 34 |
| closed | final | /scxml | final | 961 | 170 | 70 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| filed | assessing | assess |  | forward | 104,122 195,122 |
| assessing | closed | withdraw |  | forward | 826,170 921,170 921,187 961,187 |
| collecting | checking | complete |  | forward | 598,129 713,129 |
| checking | collecting | missing |  | back | 713,141 598,141 |
| checking | paid | settle |  | forward | 798,141 951,141 951,135 961,135 |
| checking | appeal | appeal |  | forward | 798,129 941,129 941,89 961,89 |
| interview | documents | cleared |  | forward | 341,135 458,135 |
| appeal | assessing | upheld |  | back | 961,79 826,79 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| assessing | fraud |  | interview |

#### Problems

_none_
