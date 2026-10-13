# Python values: bootstrap

The new C extension exposes `encode(value)` and `decode(data)`, using
core's streaming CBOR writer and reader. This first slice supports exact
built-in `int` (from -2^64 to 2^64-1), `float`, `str`, `bytes`, `list`,
and `dict` with `str` keys, plus `True`, `False`, and `None`. Lists and
maps preserve order. Nesting is limited to 64 by core. Shared subtrees
are allowed; cycles are refused by the nesting limit.

Encoding refuses unsupported Python types with `TypeError`, integers
outside the wire range with `OverflowError`, and lone surrogates with
`UnicodeError`. Decoding accepts exact built-in `bytes` containing exactly
one CBOR item; other input types raise `TypeError` in this bootstrap.
Malformed input and unsupported tags or simple values
raise `ValueError`. `Address` and `UNDEFINED` remain to be implemented;
they are not silently converted to ordinary strings or `None`.

No host or actor execution exists yet. The `conform` adapter uses this
codec for driver frames and reports every method as unimplemented.
