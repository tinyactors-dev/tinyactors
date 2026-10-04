enum Test {
    A,
    'B',
    'C' = Math.floor(Math.random() * 1000),
    D = 10,
    E
}

enum Style {
    None = 0,
    Bold = 1,
    Italic = 2,
    Underline = 4,
    Emphasis = Bold | Italic,
    Hyperlink = Bold | Underline
}
