# `atom.h`

Atoms intern ordinary strings such as definition/state/type names. `ta_atom` is a compact process-local handle.

Atoms and structured event names are distinct: an event name consists of multiple atom segments linked through the event-name table.

Core session images may use interpreter-owned runtime structures in-process. A portable durability encoding must record logical text (or another stable dictionary representation), not blindly persist a process-local `ta_atom` index.
