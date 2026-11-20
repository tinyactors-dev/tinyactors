"""What an actor, a message and a delayed send cost on this interpreter.

    python main.py [--counts N,N,...] [--datamodel python|ecmascript] [--no-tracing]
                   [--chart file.scxml] [--data JSON] [--csv file.csv]

Each measurement runs in a process of its own. It sizes the host's pools
for the count up front, takes the memory, makes N of the thing, and takes
the memory again. It prints the difference per thing, in whole bytes:

- memory: what the process holds more (its resident set), the host
  included: plan with this number;
- Python heap: of it, Python's objects (tracemalloc, in a second process,
  since tracing inflates the resident set). On the "python" datamodel an
  actor's variables and a message's data are here. quickjs keeps its own
  heap, which only memory shows;
- core reserves: the core's records for each thing, which the host
  reserves when it sizes its pools; they count in memory once used;
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
import tracemalloc

import tinyactors

CHART = """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="{datamodel}" name="counter" initial="idle">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="idle">
    <transition event="message"><assign location="count" expr="count + 1"/></transition>
    <transition event="arm"><send event="later" delay="3600s"><content expr="_event.data"/></send></transition>
  </state>
</scxml>"""

ROOM = 4096
RESIDENT = 1000  # the actors a hibernated population keeps in memory

ROWS = [("per actor, started", "actor", "started"),
        ("per actor, after an event", "actor", "handled"),
        ("per actor, hibernated", "hibernated", "hibernated"),
        ("per message waiting", "message", "waiting"),
        ("per delayed send pending", "delayed send", "pending")]


def resident():
    """The resident set in bytes: /proc on Linux, else the peak (macOS's ru_maxrss is in bytes)."""
    try:
        with open("/proc/self/statm") as statm:
            return int(statm.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except OSError:
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def sizes(what, count, n, tracing):
    system = {"actor": {"maxActors": n + 16, "maxEvents": count + ROOM},
              # the slabs for the resident actors only: a hibernated actor has no instance
              "hibernated": {"maxActors": n + RESIDENT + 16, "maxEvents": ROOM,
                             "instanceBytes": 1 << 20, "stringBytes": 1 << 20},
              "message": {"maxEvents": n + ROOM},
              "delayed send": {"maxTimers": n + 64, "maxEvents": n + ROOM}}[what]
    return {**system, "tracing": tracing}


def region_of(image, system):
    """The core region of a host sized by `system`, with the image added."""
    found = []
    with tinyactors.Host(image, system="memory", config={"system": system}) as host:
        host.host_stats(lambda stats: found.append(stats["host"]["regionBytes"]))
        host.run_until_idle()
    return found[0]


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


def measure(what, count, datamodel, tracing, chart, data, traced):
    text = chart or CHART.format(datamodel=datamodel)
    name = re.search(r'<scxml[^>]*\sname="([^"]+)"', text).group(1)
    with tinyactors.compile_charts([("chart.scxml", text)]) as compiled:
        image = compiled.image
    region = (region_of(image, sizes(what, count, count, tracing))
              - region_of(image, sizes(what, count, 0, tracing))) / count

    # a small host first, so the code each step runs is loaded before the baseline
    with tinyactors.Host(image, system="memory", clock=tinyactors.SimulatedClock()) as warm:
        warm.spawn(name).send("message", data)
        if name == "counter":
            warm.spawn(name).send("arm", data)
        warm.run_until_idle()

    def sample():
        gc.collect()
        return tracemalloc.get_traced_memory()[0] if traced else resident()

    def per(before, after):
        return {"bytes": round((after - before) / count), "region": round(region)}

    result = {}
    store = CountingStore()
    sessions = array.array("Q", bytes(8 * count if what == "actor" else 0))  # the program's own list of actors
    if not traced:
        before = sample()                         # before the host: its region counts as it is used
    hibernating = {"store": store, "resident": RESIDENT} if what == "hibernated" else {}
    host = tinyactors.Host(image, system="memory", clock=tinyactors.SimulatedClock(),
                           config={"system": sizes(what, count, count, tracing)}, **hibernating)
    if traced:                                    # after it: tracemalloc also sees the region (PyMem_RawMalloc)
        tracemalloc.start()
        before = sample()

    def batches(send):
        """Inputs 1000 at a time, so the queue never holds many at once and the peak stays low."""
        for first in range(0, count, 1000):
            for i in range(first, min(count, first + 1000)):
                send(i)
            host.run_until_idle()

    with host:
        if what == "actor":
            for i in range(count):
                sessions[i] = host.spawn(name).id
            host.run_until_idle()
            result["started"] = per(before, sample())
            batches(lambda i: host.send(sessions[i], "message", data))
            result["handled"] = per(before, sample())
        elif what == "hibernated":                # after each turn the host hibernates all but RESIDENT
            batches(lambda i: host.spawn(name))
            result["hibernated"] = {**per(before, sample()), "blob": round(store.bytes / store.saved)}
        else:
            actor = host.spawn("counter")
            host.run_until_idle()
            if what == "message":                 # the messages wait in the mailbox: nothing runs
                for _ in range(count):
                    actor.send("message", data)
                result["waiting"] = per(before, sample())
            else:                                 # each arm handled: its delayed send waits an hour
                batches(lambda i: actor.send("arm", data))
                result["pending"] = per(before, sample())
    return result


def run(count=100_000, datamodel="python", tracing=True, chart=None, data=None):
    """Every row at `count`, each measured in processes of its own: memory, then the Python heap."""
    data = {"text": "hello"} if data is None else data
    results = {}
    for what in ["actor", "hibernated", "message", "delayed send"]:
        rows = {}
        for traced in [False, True]:
            job = json.dumps({"what": what, "count": count, "datamodel": datamodel, "tracing": tracing,
                              "chart": chart if what in ("actor", "hibernated") else None, "data": data,
                              "traced": traced})
            child = subprocess.run([sys.executable, __file__, "--job", job], capture_output=True, text=True,
                                   env=os.environ)
            if child.returncode != 0:
                raise RuntimeError(f"measuring {what} at {count} failed:\n{child.stderr}")
            for row, value in json.loads(child.stdout).items():
                entry = rows.setdefault(row, {"region": value["region"], "blob": value.get("blob")})
                entry["heap" if traced else "resident"] = value["bytes"]
        results[what] = rows
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
             f"{'':28}{'memory':>10}{'Python heap':>13}{'core reserves':>15}{'blob':>8}"]
    for label, what, key in ROWS:
        row = results[what][key]
        blob = "" if row["blob"] is None else f"{row['blob']} B"
        lines.append(f"{label:28}{row['resident']:>8} B{row['heap']:>11} B{row['region']:>13} B{blob:>8}")
    return lines


CSV_HEADER = "runtime,datamodel,tracing,count,row,memory,heap,core_reserves,blob,machine,date\n"


def csv(results, count, datamodel, tracing):
    date = datetime.date.today().isoformat()
    lines = []
    for label, what, key in ROWS:
        row = results[what][key]
        blob = "" if row["blob"] is None else row["blob"]
        lines.append(f'{runtime()},{datamodel},{"on" if tracing else "off"},{count},"{label}",'
                     f'{row["resident"]},{row["heap"]},{row["region"]},{blob},"{machine()}",{date}')
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
