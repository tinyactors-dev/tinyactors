# Reference sources

Primary semantic source:

- W3C Recommendation, **State Chart XML (SCXML): State Machine Notation for Control Abstraction, Version 1.0**, 1 September 2015: <https://www.w3.org/TR/scxml/>

Particularly relevant sections:

- 3.1.3 Parallel States;
- 3.10 History;
- 3.11 Legal State Configurations and Specifications;
- 3.12 SCXML Events, especially Event Descriptors and Errors;
- 3.13 Selecting and Executing Transitions;
- 4 Executable Content;
- 5 Data Model and Data Manipulation;
- 6.2 Send;
- 6.3 Cancel;
- 6.4 Invoke;
- Appendix D, Algorithm for SCXML Interpretation (informative algorithm, not a required implementation data structure).

Design note: Tinyactors aims to preserve these execution semantics while using a non-XML definition representation and an embedder-controlled runtime environment.
