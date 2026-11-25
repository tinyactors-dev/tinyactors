# Graph

## Charts

### Chart 1: document

- **Name**: document
- **Width**: 712
- **Height**: 1304

#### Boxes

| State | Kind | Parent | Drawn as | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 | 0 | 712 | 1304 |
| draft | atomic | /scxml | atomic | 443 | 40 | 150 | 34 |
| inFlight | compound | /scxml | compound | 14 | 136 | 656 | 1038 |
| active | compound | inFlight | compound | 28 | 202 | 628 | 732 |
| review | parallel | active | parallel | 42 | 378 | 600 | 456 |
| legal | compound | review | region | 56 | 442 | 224 | 378 |
| legalQueued | atomic | legal | atomic | 114 | 552 | 108 | 34 |
| legalReviewing | atomic | legal | atomic | 70 | 638 | 196 | 58 |
| legalApproved | final | legal | final | 107 | 748 | 123 | 34 |
| legalWasAt | shallow history | legal | history | 155 | 482 | 26 | 26 |
| finance | compound | review | region | 298 | 442 | 330 | 378 |
| financeQueued | atomic | finance | atomic | 402 | 552 | 123 | 34 |
| financeReviewing | atomic | finance | atomic | 312 | 638 | 302 | 82 |
| financeApproved | final | finance | final | 394 | 772 | 138 | 34 |
| financeWasAt | shallow history | finance | history | 450 | 482 | 26 | 26 |
| revising | atomic | active | atomic | 136 | 242 | 413 | 34 |
| signoff | atomic | active | atomic | 304 | 886 | 77 | 34 |
| resumePoint | deep history | active | history | 567 | 242 | 26 | 26 |
| escalated | atomic | inFlight | atomic | 144 | 1006 | 248 | 58 |
| archiving | atomic | inFlight | atomic | 410 | 1006 | 160 | 34 |
| archiveRetry | atomic | inFlight | atomic | 433 | 1126 | 115 | 34 |
| approved | final | /scxml | final | 196 | 1256 | 85 | 34 |
| rejected | final | /scxml | final | 299 | 1256 | 85 | 34 |
| withdrawn | final | /scxml | final | 401 | 1256 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Route |
| --- | --- | --- | --- | --- | --- |
| draft | inFlight | submit |  | forward | 478,74 478,136 |
| draft | withdrawn | withdraw |  | forward | 550,74 550,126 693,126 693,1246 477,1246 477,1256 |
| inFlight | withdrawn | withdraw |  | forward | 418,1174 418,1256 |
| active | escalated | sla.expired |  | forward | 241,934 241,1006 |
| review | signoff | done.state.review |  | forward | 342,834 342,886 |
| legalQueued | legalReviewing | legal.claim |  | forward | 168,586 168,628 117,628 117,638 |
| legalReviewing | legalApproved | legal.approve |  | forward | 168,696 168,748 |
| legalReviewing | revising | legal.changes |  | back | 173,638 173,632 273,632 273,348 205,348 205,276 |
| legalWasAt | legalQueued |  |  | forward | 168,508 168,552 |
| financeQueued | financeReviewing | finance.claim |  | forward | 463,586 463,628 521,628 521,638 |
| financeReviewing | financeApproved | finance.approve | !legalFirst(amount, legalFirstAt) \|\| In('legalApproved') | forward | 463,720 463,772 |
| financeReviewing | revising | finance.changes |  | back | 458,638 458,632 305,632 305,328 411,328 411,276 |
| financeWasAt | financeQueued |  |  | forward | 463,508 463,552 |
| revising | review | resubmit | _event.data && _event.data.material | forward | 342,276 342,378 |
| revising | legalWasAt | resubmit |  | forward | 273,276 273,358 168,358 168,482 |
| revising | financeWasAt | resubmit |  | forward | 480,276 480,368 463,368 463,482 |
| signoff | archiving | signoff.approve | approvals.legal && approvals.finance | forward | 355,920 355,976 490,976 490,1006 |
| signoff | rejected | signoff.reject |  | forward | 329,920 329,940 21,940 21,1226 341,1226 341,1256 |
| resumePoint | review |  |  | forward | 585,267 585,378 |
| escalated | resumePoint | resume |  | back | 273,1006 273,996 649,996 649,274 575,274 575,267 |
| archiving | approved | archived |  | forward | 450,1040 450,1050 663,1050 663,1236 238,1236 238,1256 |
| archiving | archiveRetry | error.communication |  | forward | 490,1040 490,1116 453,1116 453,1126 |
| archiveRetry | archiving | archive.retry |  | back | 530,1126 530,1040 |

#### Inside

| State | Events | Condition | Targets |
| --- | --- | --- | --- |
| review | done.state.legal done.state.finance |  |  |
| legalReviewing | legal.reject |  | rejected |
| financeReviewing | finance.approve |  |  |
| financeReviewing | finance.reject |  | rejected |
| escalated | legal finance signoff resubmit |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | revising → review on resubmit | revising → financeWasAt on resubmit |
| labelOnEdge | revising → review on resubmit | resumePoint → review |
