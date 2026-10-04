[
  function *() {},
  function *generateStuff(arg1, arg2) {
    yield;
    yield arg2;
    yield * foo();
  }
]
