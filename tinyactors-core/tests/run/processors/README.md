# processors

More I/O processors than the config has top-level keys: four in
`config.json`'s `ioProcessors` (two keys) and one more from
`--io-processor tax=...`. A checkout, placed, sends once to each type, so
the `effect.send` notifications show every type, the config's in its
order and then the command line's. The server once sized its list of
processors by the config's top-level keys and wrote past it here.
