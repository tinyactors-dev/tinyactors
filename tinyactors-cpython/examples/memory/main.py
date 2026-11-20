"""What an actor, a message and a delayed send cost on this interpreter.

    python main.py [--count N] [--datamodel python|ecmascript] [--no-tracing]
                   [--chart file.scxml] [--data JSON]

Each measurement runs in a process of its own. It sizes the host's pools
for the count up front, takes the memory, makes N of the thing, and takes
the memory again. It prints the difference per thing, in whole bytes:

- memory: what the process holds more (its resident set), the host
  included: plan with this number;
- Python heap: of it, Python's objects (tracemalloc, in a second process,
  since tracing inflates the resident set). On the "python" datamodel an
  actor's variables and a message's data are here. quickjs keeps its own
  heap, which only resident shows;
- core reserves: the core's records for each thing, which the host
  reserves when it sizes its pools; they count in memory once used.

--chart measures an actor of your chart; its datamodel attribute picks the
datamodel. --data is the data each message and delayed send carries.
"""

import argparse
import array
import gc
import json
import os
import re
import resource
import subprocess
import sys
import tracemalloc

import tinyactors

CHARTS = {
    "python": """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="python" name="counter" initial="idle">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="idle">
    <transition event="message"><assign location="count" expr="count + 1"/></transition>
    <transition event="arm"><send event="later" delay="3600s"><content expr="_event.data"/></send></transition>
  </state>
</scxml>""",
    "ecmascript": """<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0"
       datamodel="ecmascript" name="counter" initial="idle">
  <datamodel><data id="count" expr="0"/></datamodel>
  <state id="idle">
    <transition event="message"><assign location="count" expr="count + 1"/></transition>
    <transition event="arm"><send event="later" delay="3600s"><content expr="_event.data"/></send></transition>
  </state>
</scxml>""",
}

ROOM = 4096


def resident():
    """The resident set in bytes: /proc on Linux, else the peak (macOS's ru_maxrss is in bytes)."""
    try:
        with open("/proc/self/statm") as statm:
            return int(statm.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
    except OSError:
        return resource.getrusage(resource.RUSAGE_SELF).ru_maxrss


def sizes(what, count, n, tracing):
    system = {"actor": {"maxActors": n + 16, "maxEvents": count + ROOM},
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


def measure(what, count, datamodel, tracing, chart, data, traced):
    text = chart or CHARTS[datamodel]
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
    if traced:
        tracemalloc.start()
    def sample():
        gc.collect()
        return tracemalloc.get_traced_memory()[0] if traced else resident()

    def batches(send):
        """Inputs 1000 at a time, so the queue never holds many at once and the peak stays low."""
        for first in range(0, count, 1000):
            for i in range(first, min(count, first + 1000)):
                send(i)
            host.run_until_idle()

    def per(before, after):
        return {"bytes": round((after - before) / count), "region": round(region)}

    result = {}
    sessions = array.array("Q", bytes(8 * count))  # the program's own list of actors, made before the baseline
    if not traced:
        before = sample()                         # before the host: its region counts as it is used
    host = tinyactors.Host(image, system="memory", clock=tinyactors.SimulatedClock(),
                           config={"system": sizes(what, count, count, tracing)})
    if traced:                                    # after it: tracemalloc also sees the region (PyMem_RawMalloc)
        tracemalloc.start()
        before = sample()
    with host:
        if what == "actor":
            for i in range(count):
                sessions[i] = host.spawn(name).id
            host.run_until_idle()
            result["started"] = per(before, sample())
            batches(lambda i: host.send(sessions[i], "message", data))
            result["handled"] = per(before, sample())
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
    """Every row, each measured in processes of its own: resident, then the Python heap."""
    data = {"text": "hello"} if data is None else data
    results = {}
    for what in ["actor", "message", "delayed send"]:
        rows = {}
        for traced in [False, True]:
            job = json.dumps({"what": what, "count": count, "datamodel": datamodel, "tracing": tracing,
                              "chart": chart if what == "actor" else None, "data": data, "traced": traced})
            child = subprocess.run([sys.executable, __file__, "--job", job], capture_output=True, text=True,
                                   env=os.environ)
            if child.returncode != 0:
                raise RuntimeError(f"measuring {what} failed:\n{child.stderr}")
            for row, value in json.loads(child.stdout).items():
                rows.setdefault(row, {"region": value["region"]})["heap" if traced else "resident"] = value["bytes"]
        results[what] = rows
    return results


def report(results, count, datamodel, tracing):
    lines = [f"Python {sys.version.split()[0]}, datamodel {datamodel}, tracing {'on' if tracing else 'off'}, "
             f"{count} of each", "", f"{'':28}{'memory':>10}{'Python heap':>13}{'core reserves':>15}"]
    rows = [("per actor, started", results["actor"]["started"]),
            ("per actor, after an event", results["actor"]["handled"]),
            ("per message waiting", results["message"]["waiting"]),
            ("per delayed send pending", results["delayed send"]["pending"])]
    for label, row in rows:
        lines.append(f"{label:28}{row['resident']:>8} B{row['heap']:>11} B{row['region']:>13} B")
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--job")
    parser.add_argument("--count", type=int, default=100_000)
    parser.add_argument("--datamodel", choices=["python", "ecmascript"], default="python")
    parser.add_argument("--no-tracing", action="store_true")
    parser.add_argument("--chart")
    parser.add_argument("--data")
    arguments = parser.parse_args()
    if arguments.job:
        print(json.dumps(measure(**json.loads(arguments.job))))
        return
    chart = open(arguments.chart).read() if arguments.chart else None
    datamodel = re.search(r'datamodel="([^"]+)"', chart).group(1) if chart else arguments.datamodel
    data = json.loads(arguments.data) if arguments.data else None
    tracing = not arguments.no_tracing
    for line in report(run(arguments.count, datamodel, tracing, chart, data), arguments.count, datamodel, tracing):
        print(line)


if __name__ == "__main__":
    main()
