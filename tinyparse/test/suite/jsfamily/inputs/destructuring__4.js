let {a: b = c} = object;
for await (var {a: {b} = object} of asyncIter) {}
function a({b = true}, [c, d = false]) {}
function b({c} = {}) {}
