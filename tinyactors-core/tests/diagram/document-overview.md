# Graph

## Charts

### Chart 1: document

- **Name**: document
- **Width**: 802
- **Height**: 916

#### Boxes

| State | Kind | Parent | Drawn as | Hides | Marks | X | Y | Width | Height |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| /scxml | compound |  | root | 0 |  | 0 | 0 | 802 | 916 |
| draft | atomic | /scxml | atomic | 0 | path | 413 | 40 | 150 | 34 |
| inFlight | compound | /scxml | compound | 0 | path | 130 | 136 | 463 | 630 |
| active | compound | inFlight | compound | 0 |  | 144 | 202 | 413 | 324 |
| review | parallel | active | folded | 8 | path | 158 | 312 | 385 | 74 |
| revising | atomic | active | atomic | 0 |  | 196 | 478 | 85 | 34 |
| signoff | atomic | active | atomic | 0 | path | 456 | 478 | 77 | 34 |
| resumePoint | deep history | active | history | 0 |  | 337 | 242 | 26 | 26 |
| escalated | atomic | inFlight | atomic | 0 | highlight | 152 | 598 | 248 | 58 |
| archiving | atomic | inFlight | atomic | 0 | path | 418 | 598 | 160 | 34 |
| archiveRetry | atomic | inFlight | atomic | 0 |  | 441 | 718 | 115 | 34 |
| approved | final | /scxml | final | 0 |  | 215 | 868 | 85 | 34 |
| rejected | final | /scxml | final | 0 |  | 318 | 868 | 85 | 34 |
| withdrawn | final | /scxml | final | 0 |  | 421 | 868 | 92 | 34 |

#### Edges

| Source | Target | Events | Condition | Drawn as | Steps | Route |
| --- | --- | --- | --- | --- | --- | --- |
| draft | inFlight | submit |  | forward | 1 | 449,74 449,136 |
| draft | withdrawn | withdraw |  | forward |  | 521,74 521,126 615,126 615,858 496,858 496,868 |
| inFlight | withdrawn | withdraw |  | forward |  | 438,766 438,868 |
| active | escalated | sla.expired |  | forward |  | 249,526 249,598 |
| review | signoff | done.state.review |  | forward | 2 | 495,386 495,478 |
| review | revising | legal.changes |  | forward |  | 206,386 206,478 |
| review | rejected | legal.reject |  | forward |  | 399,386 399,532 590,532 590,818 348,818 348,868 |
| review | revising | finance.changes |  | forward |  | 254,386 254,448 230,448 230,478 |
| review | rejected | finance.reject |  | forward |  | 447,386 447,536 580,536 580,828 360,828 360,868 |
| revising | review | resubmit | _event.data && _event.data.material | back |  | 247,478 247,458 302,458 302,386 |
| revising | review | resubmit |  | back |  | 264,478 264,468 350,468 350,386 |
| signoff | archiving | signoff.approve | approvals.legal && approvals.finance | forward | 3 | 508,512 508,588 498,588 498,598 |
| signoff | rejected | signoff.reject |  | forward |  | 482,512 482,540 595,540 595,848 373,848 373,868 |
| resumePoint | review |  |  | forward |  | 355,267 355,290 350,290 350,312 |
| escalated | resumePoint | resume |  | back |  | 281,598 281,578 151,578 151,274 345,274 345,267 |
| archiving | approved | archived |  | forward |  | 458,632 458,638 585,638 585,838 258,838 258,868 |
| archiving | archiveRetry | error.communication |  | forward |  | 498,632 498,708 461,708 461,718 |
| archiveRetry | archiving | archive.retry |  | back |  | 538,718 538,632 |

#### Inside

| State | Events | Condition | Targets | Steps |
| --- | --- | --- | --- | --- |
| review | done.state.legal done.state.finance |  |  |  |
| escalated | legal finance signoff resubmit |  |  |  |

#### Problems

| Kind | Subject | Touches |
| --- | --- | --- |
| labelOnEdge | review → signoff on done.state.review | review → rejected on finance.reject |
| labelOnEdge | review → revising on legal.changes | escalated → resumePoint on resume |
| labelOnEdge | review → revising on finance.changes | review → revising on legal.changes |
| labelOnEdge | review → revising on finance.changes | revising → review on resubmit |
| labelOnEdge | revising → review on resubmit | review → revising on legal.changes |
| labelOnEdge | revising → review on resubmit | review → revising on finance.changes |
| labelOnEdge | revising → review on resubmit | escalated → resumePoint on resume |
| labelOnEdge | signoff → archiving on signoff.approve | draft → withdrawn on withdraw |
| labelOnEdge | signoff → archiving on signoff.approve | review → rejected on legal.reject |
| labelOnEdge | signoff → archiving on signoff.approve | review → rejected on finance.reject |
| labelOnEdge | signoff → rejected on signoff.reject | draft → withdrawn on withdraw |
| labelOnEdge | archiveRetry → archiving on archive.retry | archiving → archiveRetry on error.communication |
