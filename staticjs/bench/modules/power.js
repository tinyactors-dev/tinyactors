// Copied from tests/spec/modules.spec, the case "water-filling, written in
// staticjs": a chart's module, which bench.c compiles and evaluates.
export const fill = (left, wants) =>
  wants.reduce((acc, w, i) => {
    const kw = Math.min(w.want_kw, acc.left / (wants.length - i));
    return { left: acc.left - kw, out: { ...acc.out, [w.port]: kw } };
  }, { left, out: {} });

export const waterFill = (capacity, demand) => {
  const ports = Object.entries(demand);
  const priorities = [...new Set(ports.map(([, d]) => d.priority))].toSorted((a, b) => b - a);
  return priorities.reduce((acc, prio) => {
    const wants = ports
      .filter(([, d]) => d.priority === prio)
      .map(([port, d]) => ({ port, want_kw: d.want_kw }))
      .toSorted((a, b) => a.want_kw - b.want_kw);
    const r = fill(acc.left, wants);
    return { left: r.left, out: { ...acc.out, ...r.out } };
  }, { left: capacity, out: {} }).out;
};

export const example = waterFill(60, {
  a: { want_kw: 22, priority: 1 },
  b: { want_kw: 7, priority: 1 },
  c: { want_kw: 50, priority: 2 }
});
