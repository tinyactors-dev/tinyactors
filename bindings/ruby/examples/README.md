# Ruby examples

Build the extension first from this directory:

```sh
make
```

Then run the examples with the development-tree loader:

```sh
ruby examples/basic.rb
ruby examples/entities.rb 10000
ruby examples/ring.rb 64 1000000
```

`ring.rb` is the Ruby binding counterpart of the hosted ring workload described
in [`benchmarks/README.md`](../../../benchmarks/README.md). All actors share one
definition, and Ruby advances the ring by posting each hop to the next actor;
this keeps the benchmark focused on the binding's thin runtime wrapper without
creating one native definition per actor.
