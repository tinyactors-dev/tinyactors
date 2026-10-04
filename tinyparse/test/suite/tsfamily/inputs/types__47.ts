// These should generate the same AST aside from the parenthesized_type node
type X1 = typeof Y[keyof typeof Z];
type X2 = (typeof Y)[keyof typeof Z];
