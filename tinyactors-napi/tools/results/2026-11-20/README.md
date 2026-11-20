# Runs of 2026-11-20

The runs the guides quote, each alone on one machine (Intel Xeon @
2.10GHz, 4 CPUs, 16 GiB, Linux; node 22.23.5, bun 1.4.1).

| File | Made by | Quoted in |
|---|---|---|
| `stress.log` | `mise run stress` | docs/guide/hibernation.md |
| `migration.log` | `mise run migration` | docs/guide/migration.md |
| `snapshot.log` | `mise run snapshot` | docs/guide/snapshots.md |
| `snapshot-resident.log` | `mise run snapshot --actors 100000 --resident 100000` | docs/guide/snapshots.md |
| `services.log` | `mise run services` | docs/guide/services.md |
| `scale.log`, `scale-*.json` | `mise run scale` | docs/guide/capacity.md |

The JSON files hold every operation's samples at both populations, for a
chart. The root `AGENTS.md` ("Published numbers") says when to run them
again.
