# Graph

## Charts

### Chart 1: document

- **Name**: document
- **Width**: 671
- **Height**: 916

#### Boxes

| State | Kind | Parent | Drawn as | Hides | Marks | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 |  | 0 | 0 | 671 | 916 |
| draft | atomic | /scxml | atomic | 0 | path | 297 | 40 | 150 | 34 |
| inFlight | compound | /scxml | compound | 0 | path | 14 | 136 | 463 | 630 |
| active | compound | inFlight | compound | 0 |  | 28 | 202 | 413 | 324 |
| review | parallel | active | folded | 8 | path | 42 | 312 | 385 | 74 |
| revising | atomic | active | atomic | 0 |  | 172 | 478 | 85 | 34 |
| signoff | atomic | active | atomic | 0 | path | 340 | 478 | 77 | 34 |
| resumePoint | deep history | active | history | 0 |  | 222 | 242 | 26 | 26 |
| escalated | atomic | inFlight | atomic | 0 | highlight | 37 | 598 | 248 | 58 |
| archiving | atomic | inFlight | atomic | 0 | path | 303 | 598 | 160 | 34 |
| archiveRetry | atomic | inFlight | atomic | 0 |  | 325 | 718 | 115 | 34 |
| approved | final | /scxml | final | 0 |  | 99 | 868 | 85 | 34 |
| rejected | final | /scxml | final | 0 |  | 202 | 868 | 85 | 34 |
| withdrawn | final | /scxml | final | 0 |  | 305 | 868 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Steps | Route |
| --- | --- | --- | --- | --- | --- | --- |
| draft | inFlight | submit |  | forward | 1 | 333,74 333,136 |
| draft | withdrawn | withdraw |  | forward |  | 405,74 405,126 500,126 500,858 380,858 380,868 |
| inFlight | withdrawn | withdraw |  | forward |  | 322,766 322,868 |
| active | escalated | sla.expired |  | forward |  | 192,526 192,598 |
| review | signoff | done.state.review |  | forward | 2 | 379,386 379,478 |
| review | revising | legal.changes |  | forward |  | 90,386 90,468 182,468 182,478 |
| review | rejected | legal.reject |  | forward |  | 283,386 283,532 470,532 470,818 232,818 232,868 |
| review | revising | finance.changes |  | forward |  | 138,386 138,458 192,458 192,478 |
| review | rejected | finance.reject |  | forward |  | 331,386 331,536 470,536 470,828 244,828 244,868 |
| revising | review | resubmit | _event.data && _event.data.material | back |  | 202,478 202,448 186,448 186,386 |
| revising | review | resubmit |  | back |  | 235,478 235,386 |
| signoff | archiving | signoff.approve | approvals.legal && approvals.finance | forward | 3 | 392,512 392,588 383,588 383,598 |
| signoff | rejected | signoff.reject |  | forward |  | 366,512 366,540 470,540 470,848 257,848 257,868 |
| resumePoint | review |  |  | forward |  | 240,267 240,290 235,290 235,312 |
| escalated | resumePoint | resume |  | back |  | 202,598 202,578 35,578 35,274 230,274 230,267 |
| archiving | approved | archived |  | forward |  | 343,632 343,638 470,638 470,838 142,838 142,868 |
| archiving | archiveRetry | error.communication |  | forward |  | 383,632 383,718 |
| archiveRetry | archiving | archive.retry |  | back |  | 423,718 423,632 |

#### Inside

| State | Events | Condition | Targets | Steps |
| --- | --- | --- | --- | --- |
| review | done.state.legal done.state.finance |  |  |  |
| escalated | legal finance signoff resubmit |  |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | review → signoff on done.state.review | review → rejected on finance.reject |
| labelOnEdge | revising → review on resubmit | review → revising on legal.changes |
| labelOnEdge | revising → review on resubmit | review → rejected on legal.reject |
| labelOnEdge | revising → review on resubmit | review → revising on finance.changes |
| labelOnEdge | revising → review on resubmit | revising → review on resubmit |
| labelOnEdge | signoff → archiving on signoff.approve | draft → withdrawn on withdraw |
| labelOnEdge | signoff → archiving on signoff.approve | review → rejected on legal.reject |
| labelOnEdge | signoff → archiving on signoff.approve | review → rejected on finance.reject |
| labelOnEdge | signoff → archiving on signoff.approve | signoff → rejected on signoff.reject |
