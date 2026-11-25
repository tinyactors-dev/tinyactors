"""What an actor, a message and a delayed send cost on this interpreter, part by part.

    python main.py [--counts N,N,...] [--datamodel python|ecmascript] [--no-tracing]
                   [--chart file.scxml] [--data JSON] [--csv file.csv]

Each measurement runs in a process of its own. It sizes every pool it fills
for the count up front, so nothing grows while it measures, makes N of the
thing, and prints per thing, in whole bytes:

- core reserved: the core's region for one more of it, the difference
  between a host sized for N and one sized for none, over N: what a machine
  holds for a host sized up front (plan with this);
- core used: what the things use of it, from the core's own counters
  (host_stats' pools): each pool's records in use times its record's size,
  and the slab of instances and strings;
- host other: what the host allocates beside its regions (host_stats'
  allocatedBytes less regionBytes), over N;
- quickjs: on the ecmascript datamodel, quickjs's heap and the C library's
  header of each of its blocks (quickjs_heap_bytes, quickjs_heap_blocks),
  over N;
- accounted: core reserved, host other and quickjs together;
- process: what the process holds more (its own pages, in memory or
  swapped out), the host included. Never exact: the gap to accounted is
  mostly Python's objects holding the chart's data on the "python"
  datamodel, then malloc's overhead per block and the fixed costs of a host
  and an interpreter spread over N;
- blob: a hibernated actor's bytes in your store.

--counts measures each count in turn (default 100000), and --csv writes
every row to a file, one line each, for a chart. --chart measures an actor
of your chart; its datamodel attribute picks the datamodel. --data is the
data each message and delayed send carries.
"""

import argparse
import array
import datetime
import gc
import json
import os
import platform
import re
import resource
import subprocess
import sys

import tinyactors

CHART = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="{datamodel}" name="counter" initial="idle">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="idle">
    <transition event="message"><assign location="count" expr="count + 1"/></transition>
    <transition event="arm"><send event="later" delay="3600s"><content expr="_event.data"/></send></transition>
  </state>
</scxml>"""

ROOM = 4096          # room in the events pool beyond what a row fills
RESIDENT = 1000      # the actors a hibernated population keeps in memory
BLOCK_HEADER = 16    # the C library's header beside each quickjs block: two words on a 64-bit machine
# The pool sizes of core's own check of its records (tinyactors-core's tools/layout/layout.c): a
# record's size is the region's growth from 2^16 to 2^17 of its pool, the slabs fixed, over 2^16.
SMALL = 1 << 16
FIXED_SLABS = {"instanceBytes": 4096, "stringBytes": 16384}

ROWS = [("per actor, started", "actor", "started"),
        ("per actor, after an event", "actor", "handled"),
        ("per actor, hibernated", "hibernated", "hibernated"),
        ("per message waiting", "message", "waiting"),
        ("per delayed send pending", "delayed send", "pending")]

COLUMNS = [("core reserved", "reserved"), ("core used", "used"), ("host other", "host_other"),
           ("quickjs", "quickjs"), ("accounted", "accounted"), ("process", "process"), ("blob", "blob")]


def resident():
    """The process's own memory in bytes: on Linux its anonymous pages, in memory or swapped out
    (RssAnon + VmSwap); else the peak resident set (macOS's ru_maxrss is in bytes).

    Not the whole resident set: that also counts the pages of mapped files (the interpreter's
    and the extension's code), which the kernel drops under memory pressure and reads back when
    they run again. Dropped between the two samples, they made a row negative."""
    try:
        with open("/proc/self/status") as status:
            fields = dict(line.split(":", 1) for line in status)
        return sum(int(fields[name].split()[0]) for name in ("RssAnon", "VmSwap")) * 1024  # in kB
    except OSError:
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def compiled(text):
    """The image of `text`, and its chart's name."""
    name = re.search(r'<scxml[^>]*\sname="([^"]+)"', text).group(1)
    with tinyactors.compile_charts([("chart.scxml", text)]) as result:
        return result.image, name


def sizes(what, count, n, tracing):
    """The host config's "system" map of a row's host, sized for `n` of its thing (0: none of
    it): every pool the row fills holds what it fills, so no pool grows during a measurement."""
    system = {"actor": {"maxActors": n + 16, "maxEvents": count + ROOM},
              # the slabs for the resident actors only: a hibernated actor has no instance
              "hibernated": {"maxActors": n + RESIDENT + 16, "maxEvents": ROOM,
                             "instanceBytes": 1 << 20, "stringBytes": 1 << 20},
              "message": {"maxEvents": n + ROOM},
              "delayed send": {"maxTimers": n + 64, "maxEvents": n + ROOM}}[what]
    return {**system, "tracing": tracing}


def stats(host):
    """host_stats of a host: its counters and its pools, summed over its segments. The reply is
    the host's state when it was asked; the turn that delivers it runs nothing first."""
    found = []
    host.host_stats(found.append)
    host.run_until_idle()
    counters, pools = found[0]["host"], found[0]["pools"]

    def used(name):
        return sum(pool[name]["used"] for pool in pools)

    return {"region": counters["regionBytes"], "allocated": counters["allocatedBytes"],
            "used": {"actors": used("actors"), "events": used("events"), "timers": used("timers"),
                     "slab": used("slabBytes")},
            "capacities": {name: [pool[name]["capacity"] for pool in pools]
                           for name in ("actors", "events", "timers", "slabBytes")}}


def region_of(image, system):
    """The core region of a host sized by `system`, with the image added."""
    with tinyactors.Host(image, system="memory", clock=tinyactors.SimulatedClock(),
                         config={"system": system}) as host:
        return stats(host)["region"]


def units(tracing=True):
    """The size of a record of each pool in the core's region, as the core itself checks it
    (tools/layout/layout.c): the region's growth from 2^16 to 2^17 of the pool, the slabs fixed,
    over 2^16. `event` follows `tracing` (a traced event holds a trace context)."""
    image, _ = compiled(CHART.format(datamodel="python"))

    def unit(pool):
        def at(n):
            return region_of(image, {**FIXED_SLABS, pool: n, "tracing": tracing})
        return (at(2 * SMALL) - at(SMALL)) // SMALL

    return {"actor": unit("maxActors"), "event": unit("maxEvents"), "timer": unit("maxTimers")}


class CountingStore:
    """A store that keeps only what it was given: the blobs' count and bytes."""

    def __init__(self):
        self.saved = 0
        self.bytes = 0

    def save(self, session, blob):
        self.saved += 1
        self.bytes += len(blob)

    def load(self, session):
        raise RuntimeError("nothing wakes during the measurement")

    def delete(self, session):
        pass


def measure(what, count, datamodel, tracing, chart, data, unit):
    image, name = compiled(chart or CHART.format(datamodel=datamodel))
    reserved = region_of(image, sizes(what, count, count, tracing)) - region_of(image, sizes(what, count, 0, tracing))

    # a small host first, so the code each step runs is loaded before the baseline
    with tinyactors.Host(image, system="memory", clock=tinyactors.SimulatedClock()) as warm:
        warm.spawn(name).send("message", data)
        if name == "counter":
            warm.spawn(name).send("arm", data)
        warm.run_until_idle()

    store = CountingStore()
    sessions = array.array("Q", bytes(8 * count if what == "actor" else 0))  # the program's own list of actors
    gc.collect()
    own = resident()                              # before the host: its region counts
    hibernating = {"store": store, "resident": RESIDENT} if what == "hibernated" else {}
    host = tinyactors.Host(image, system="memory", clock=tinyactors.SimulatedClock(),
                           config={"system": sizes(what, count, count, tracing)}, **hibernating)
    quickjs = datamodel == "ecmascript"

    def sample():
        gc.collect()
        process = resident()                      # before host_stats' turn
        engine = host.quickjs_heap_bytes + BLOCK_HEADER * host.quickjs_heap_blocks
        return {"process": process, "quickjs": engine, **stats(host)}

    def per(value):
        return round(value / count)

    def batches(send):
        """Inputs 1000 at a time, so the queue never holds many at once and the peak stays low."""
        for first in range(0, count, 1000):
            for i in range(first, min(count, first + 1000)):
                send(i)
            host.run_until_idle()

    with host:
        actor = None
        if what in ("message", "delayed send"):
            actor = host.spawn("counter")
            host.run_until_idle()
        before = {**sample(), "process": own}

        def row(after):
            used, was = after["used"], before["used"]
            core = ((used["actors"] - was["actors"]) * unit["actor"] + (used["events"] - was["events"]) * unit["event"]
                    + (used["timers"] - was["timers"]) * unit["timer"] + (used["slab"] - was["slab"]))
            host_other = (after["allocated"] - after["region"]) - (before["allocated"] - before["region"])
            engine = after["quickjs"] - before["quickjs"] if quickjs else 0
            return {"reserved": per(reserved), "used": per(core), "host_other": per(host_other),
                    "quickjs": per(engine) if quickjs else None,
                    "accounted": per(reserved + host_other + engine), "process": per(after["process"] - before["process"]),
                    "pools": {name: used[name] - was[name] for name in ("actors", "events", "timers")},
                    "grown": [name for name in before["capacities"]
                              if before["capacities"][name] != after["capacities"][name]]}

        result = {}
        if what == "actor":
            for i in range(count):
                sessions[i] = host.spawn(name).id
            host.run_until_idle()
            result["started"] = row(sample())
            try:
                batches(lambda i: host.send(sessions[i], "message", data))
                result["handled"] = row(sample())
            except tinyactors.TinyactorsError as error:  # quickjs's heap full, say: the started row stands
                result["handled"] = {"failed": f"{type(error).__name__}: {error}"}
        elif what == "hibernated":                # after each turn the host hibernates all but RESIDENT
            batches(lambda i: host.spawn(name))
            # then the last RESIDENT too, so the row holds hibernated actors only: a resident
            # actor's instance holds whole slab pages. A sweep passes over an actor that ran since
            # the last one, so it takes up to three rounds.
            for _ in range(3):
                for session in host.sweep(count, count):
                    store.save(session, host.hibernate(session))
            result["hibernated"] = {**row(sample()), "blob": round(store.bytes / store.saved)}
        elif what == "message":                   # the messages wait in the mailbox: nothing runs
            for _ in range(count):
                actor.send("message", data)
            result["waiting"] = row(sample())
        else:                                     # each arm handled: its delayed send waits an hour
            batches(lambda i: actor.send("arm", data))
            result["pending"] = row(sample())
    return result


def run(count=100_000, datamodel="python", tracing=True, chart=None, data=None):
    """Every row at `count`, each measured in a process of its own, and the record sizes core used
    is counted with."""
    data = {"text": "hello"} if data is None else data
    unit = units(tracing)
    results = {"units": unit}
    for what in ["actor", "hibernated", "message", "delayed send"]:
        job = json.dumps({"what": what, "count": count, "datamodel": datamodel, "tracing": tracing,
                          "chart": chart if what in ("actor", "hibernated") else None, "data": data, "unit": unit})
        child = subprocess.run([sys.executable, __file__, "--job", job], capture_output=True, text=True,
                               env=os.environ)
        if child.returncode == 0:
            results[what] = json.loads(child.stdout)
            continue
        # a measurement that failed is a row too: what stopped it (quickjs's memory, the machine's)
        errors = re.findall(r"^[\w.]*Error: .*$", child.stderr, re.M)
        reason = errors[-1] if errors else f"exit {child.returncode}"
        keys = {"actor": ["started", "handled"], "hibernated": ["hibernated"],
                "message": ["waiting"], "delayed send": ["pending"]}[what]
        results[what] = {key: {"failed": reason} for key in keys}
    return results


def runtime():
    return f"python {platform.python_version()}"


def machine():
    """The machine, for the CSV and the report: results are this machine's."""
    model = platform.processor() or platform.machine()
    try:
        with open("/proc/cpuinfo") as cpuinfo:
            model = re.search(r"model name\s*:\s*(.+)", cpuinfo.read()).group(1).strip()
    except (OSError, AttributeError):
        pass
    memory = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    return f"{model}, {os.cpu_count()} CPUs, {round(memory / 2 ** 30)} GiB, {sys.platform} {platform.machine()}"


def report(results, count, datamodel, tracing):
    lines = [f"{runtime()}, datamodel {datamodel}, tracing {'on' if tracing else 'off'}, {count} of each", "",
             " " * 28 + "".join(f"{title:>{len(title) + 2}}" for title, _ in COLUMNS)]
    for label, what, key in ROWS:
        row = results[what][key]
        if "failed" in row:
            lines.append(f"{label:28}  failed: {row['failed']}")
            continue
        cells = ("" if row.get(name) is None else f"{row[name]} B" for _, name in COLUMNS)
        lines.append(f"{label:28}" + "".join(f"{cell:>{len(title) + 2}}" for (title, _), cell in zip(COLUMNS, cells)))
    unit = results["units"]
    lines += ["", f"core's records: actor {unit['actor']} B, event {unit['event']} B, timer {unit['timer']} B"]
    return lines


CSV_HEADER = ("runtime,datamodel,tracing,count,row,core_reserved,core_used,host_other,quickjs,accounted,process,"
              "blob,failed,machine,date\n")


def csv(results, count, datamodel, tracing):
    date = datetime.date.today().isoformat()
    lines = []
    for label, what, key in ROWS:
        row = results[what][key]
        failed = '"' + row["failed"].replace('"', "'") + '"' if "failed" in row else ""
        values = ["" if row.get(name) is None else str(row[name]) for _, name in COLUMNS]
        lines.append(f'{runtime()},{datamodel},{"on" if tracing else "off"},{count},"{label}",'
                     f'{",".join(values)},{failed},"{machine()}",{date}')
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--job")
    parser.add_argument("--counts", default="100000")
    parser.add_argument("--datamodel", choices=["python", "ecmascript"], default="python")
    parser.add_argument("--no-tracing", action="store_true")
    parser.add_argument("--chart")
    parser.add_argument("--data")
    parser.add_argument("--csv")
    arguments = parser.parse_args()
    if arguments.job:
        print(json.dumps(measure(**json.loads(arguments.job))))
        return
    chart = open(arguments.chart).read() if arguments.chart else None
    datamodel = re.search(r'datamodel="([^"]+)"', chart).group(1) if chart else arguments.datamodel
    data = json.loads(arguments.data) if arguments.data else None
    tracing = not arguments.no_tracing
    if arguments.csv and not os.path.exists(arguments.csv):
        with open(arguments.csv, "w") as out:
            out.write(CSV_HEADER)
    print(machine())
    for count in (int(c) for c in arguments.counts.split(",")):
        results = run(count, datamodel, tracing, chart, data)
        print()
        for line in report(results, count, datamodel, tracing):
            print(line)
        if arguments.csv:
            with open(arguments.csv, "a") as out:
                out.write(csv(results, count, datamodel, tracing))


if __name__ == "__main__":
    main()
