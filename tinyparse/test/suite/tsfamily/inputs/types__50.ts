type Foo = import('node:fs').ReadStream & {
  bar();
};
