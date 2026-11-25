# Graph

## Charts

### Chart 1: document

- **Name**: document
- **Width**: 3261
- **Height**: 508

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 3261 | 508 |
| draft | atomic | /scxml | atomic | 40 | 336 | 64 | 58 |
| inFlight | compound | /scxml | compound | 219 | 14 | 2760 | 452 |
| active | compound | inFlight | compound | 259 | 54 | 1811 | 398 |
| review | parallel | active | parallel | 683 | 94 | 1130 | 344 |
| legal | compound | review | region | 697 | 158 | 1102 | 112 |
| legalQueued | atomic | legal | atomic | 781 | 210 | 108 | 34 |
| legalReviewing | atomic | legal | atomic | 1013 | 198 | 196 | 58 |
| legalApproved | final | legal | final | 1349 | 210 | 123 | 34 |
| legalWasAt | shallow history | legal | history | 711 | 214 | 26 | 26 |
| finance | compound | review | region | 697 | 288 | 1102 | 136 |
| financeQueued | atomic | finance | atomic | 781 | 352 | 123 | 34 |
| financeReviewing | atomic | finance | atomic | 1043 | 328 | 282 | 82 |
| financeApproved | final | finance | final | 1647 | 352 | 138 | 34 |
| financeWasAt | shallow history | finance | history | 711 | 356 | 26 | 26 |
| revising | atomic | active | atomic | 273 | 224 | 85 | 84 |
| signoff | atomic | active | atomic | 1979 | 249 | 77 | 34 |
| resumePoint | deep history | active | history | 273 | 326 | 26 | 26 |
| escalated | atomic | inFlight | atomic | 2412 | 203 | 248 | 58 |
| archiving | atomic | inFlight | atomic | 2412 | 279 | 92 | 34 |
| archiveRetry | atomic | inFlight | atomic | 2850 | 279 | 115 | 34 |
| approved | final | /scxml | final | 3155 | 164 | 85 | 34 |
| rejected | final | /scxml | final | 3155 | 216 | 85 | 34 |
| withdrawn | final | /scxml | final | 3155 | 289 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| draft | inFlight | submit |  | forward | 104,355 219,355 |
| draft | withdrawn | withdraw |  | forward | 104,374 209,374 209,489 3145,489 3145,314 3155,314 |
| inFlight | withdrawn | withdraw |  | forward | 2979,297 3155,297 |
| active | escalated | sla.expired |  | forward | 2070,231 2412,231 |
| review | signoff | done.state.review |  | forward | 1813,273 1979,273 |
| legalQueued | legalReviewing | legal.claim |  | forward | 888,227 1013,227 |
| legalReviewing | legalApproved | legal.approve |  | forward | 1210,227 1349,227 |
| legalReviewing | revising | legal.changes |  | back | 1013,237 1007,237 1007,263 663,263 663,238 358,238 |
| legalWasAt | legalQueued |  |  | forward | 737,227 781,227 |
| financeQueued | financeReviewing | finance.claim |  | forward | 904,369 1043,369 |
| financeReviewing | financeApproved | finance.approve | !legalFirst(amount, legalFirstAt) \|\| In('legalApproved') | forward | 1325,369 1647,369 |
| financeReviewing | revising | finance.changes |  | back | 1043,355 1037,355 1037,321 643,321 643,280 358,280 |
| financeWasAt | financeQueued |  |  | forward | 737,369 781,369 |
| revising | review | resubmit | _event.data && _event.data.material | forward | 358,266 683,266 |
| revising | legalWasAt | resubmit |  | forward | 358,252 673,252 673,227 711,227 |
| revising | financeWasAt | resubmit |  | forward | 358,294 633,294 633,369 711,369 |
| signoff | archiving | signoff.approve | approvals.legal && approvals.finance | forward | 2056,272 2382,272 2382,296 2412,296 |
| signoff | rejected | signoff.reject |  | forward | 2056,260 2076,260 2076,459 3125,459 3125,233 3155,233 |
| resumePoint | review |  |  | forward | 299,343 683,343 |
| escalated | resumePoint | resume |  | back | 2412,241 2402,241 2402,445 305,445 305,335 299,335 |
| archiving | approved | archived |  | forward | 2504,287 2514,287 2514,459 3135,459 3135,181 3155,181 |
| archiving | archiveRetry | error.communication |  | forward | 2504,296 2830,296 2830,291 2850,291 |
| archiveRetry | archiving | archive.retry |  | back | 2850,303 2840,303 2840,304 2504,304 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| review | done.state.legal done.state.finance |  |  |
| legalReviewing | legal.reject |  | rejected |
| financeReviewing | finance.approve |  |  |
| financeReviewing | finance.reject |  | rejected |
| escalated | legal finance signoff resubmit |  |  |
