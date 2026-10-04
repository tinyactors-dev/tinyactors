type T = Foo<Bar<typeof bar>>
type T = Foo<Bar<typeof bar.baz>>
type T = Foo<Bar<typeof bar["baz"]>>
