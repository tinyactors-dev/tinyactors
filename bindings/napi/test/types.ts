import { defineMachine, createSystem, assign, send, type ActorEvent } from "..";
import * as n from "../lowlevel";
type State = { x: number };
type Input = { initial: number };
type Move = ActorEvent<{ dx: number }>;
const machine = defineMachine<State, Input, Move>({
  id: "typed",
  state: ({ input }) => ({ x: input.initial }),
  initial: "idle",
  states: {
    idle: {
      on: {
        MOVE: {
          actions: ({ state, event }) => {
            state.x += event?.data?.dx || 0;
          },
        },
      },
      entry: [
        assign<State>({ x: 1 }),
        send<State>("READY", { data: ({ state }) => ({ x: state.x }) }),
      ],
    },
  },
});
const system = createSystem({ limits: { actors: 100 } });
const actor = system.spawn(machine, { input: { initial: 2 } });
actor.send({ type: "MOVE", data: { dx: 3 } });
system.run({ nowNs: 1n });
const position: number = actor.snapshot().state.x;
system.inspect((record) => {
  if (record.kind === "actor") console.log(record.info.state, position);
});
// @ts-expect-error incorrect input
system.spawn(machine, { input: { wrong: true } });
// @ts-expect-error wrong event data
actor.send({ type: "MOVE", data: { dx: "bad" } });
// @ts-expect-error opaque handles are not interchangeable
n.ta_system_stats(system.prepare(machine));
system.close();
