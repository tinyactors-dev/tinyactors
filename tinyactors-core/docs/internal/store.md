# Host persistence

Storage is host-owned. Tinyactors supplies snapshot reader/writer callback
types for its session checkpoint codec but does not define a store vtable,
batch transaction, or persistence lifecycle.

A host persists a checkpoint before evicting resident state and materializes a
checkpoint before routing work to an evicted actor. It coordinates checkpoint
durability with its actor-directory implementation; this permits database
transactions, object-store manifests, distributed routers, and embedded stores
without imposing one storage model on the core.

Delayed sends are part of the actor checkpoint. A host with evicted actors
maintains the durable deadline index needed to route due work back to the
actor.
