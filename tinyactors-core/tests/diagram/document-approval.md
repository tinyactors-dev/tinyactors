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
| draft | atomic | /scxml | atomic | 40 | 342 | 64 | 58 |
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
| revising | atomic | active | atomic | 273 | 249 | 85 | 84 |
| signoff | atomic | active | atomic | 1979 | 274 | 77 | 34 |
| resumePoint | deep history | active | history | 273 | 351 | 26 | 26 |
| escalated | atomic | inFlight | atomic | 2412 | 216 | 248 | 58 |
| archiving | atomic | inFlight | atomic | 2412 | 292 | 92 | 34 |
| archiveRetry | atomic | inFlight | atomic | 2850 | 296 | 115 | 34 |
| approved | final | /scxml | final | 3155 | 179 | 85 | 34 |
| rejected | final | /scxml | final | 3155 | 231 | 85 | 34 |
| withdrawn | final | /scxml | final | 3155 | 297 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| draft | inFlight | submit |  | forward | 104,361 219,361 |
| draft | withdrawn | withdraw |  | forward | 104,381 209,381 209,489 3145,489 3145,323 3157,323 |
| inFlight | withdrawn | withdraw |  | forward | 2979,306 3157,306 |
| active | escalated | sla.expired |  | forward | 2070,244 2412,244 |
| review | signoff | done.state.review |  | forward | 1813,291 1979,291 |
| legalQueued | legalReviewing | legal.claim |  | forward | 888,227 1013,227 |
| legalReviewing | legalApproved | legal.approve |  | forward | 1210,227 1349,227 |
| legalReviewing | revising | legal.changes |  | back | 1013,237 1007,237 1007,263 358,263 |
| legalWasAt | legalQueued |  |  | forward | 737,227 781,227 |
| financeQueued | financeReviewing | finance.claim |  | forward | 904,369 1043,369 |
| financeReviewing | financeApproved | finance.approve | !legalFirst(amount, legalFirstAt) \|\| In('legalApproved') | forward | 1325,369 1647,369 |
| financeReviewing | revising | finance.changes |  | back | 1043,355 1037,355 1037,321 643,321 643,305 358,305 |
| financeWasAt | financeQueued |  |  | forward | 737,369 781,369 |
| revising | review | resubmit | _event.data && _event.data.material | forward | 358,291 683,291 |
| revising | legalWasAt | resubmit |  | forward | 358,277 673,277 673,227 711,227 |
| revising | financeWasAt | resubmit |  | forward | 358,319 633,319 633,369 711,369 |
| signoff | archiving | signoff.approve | approvals.legal && approvals.finance | forward | 2056,297 2382,297 2382,309 2412,309 |
| signoff | rejected | signoff.reject |  | forward | 2056,285 3125,285 3125,248 3155,248 |
| resumePoint | review |  |  | forward | 298,368 683,368 |
| escalated | resumePoint | resume |  | back | 2412,254 2402,254 2402,445 305,445 305,360 298,360 |
| archiving | approved | archived |  | forward | 2504,300 2510,300 2510,459 3135,459 3135,196 3155,196 |
| archiving | archiveRetry | error.communication |  | forward | 2504,309 2850,309 |
| archiveRetry | archiving | archive.retry |  | back | 2850,319 2830,319 2830,317 2504,317 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| review | done.state.legal done.state.finance |  |  |
| legalReviewing | legal.reject |  | rejected |
| financeReviewing | finance.approve |  |  |
| financeReviewing | finance.reject |  | rejected |
| escalated | legal finance signoff resubmit |  |  |

#### Problems

_none_
