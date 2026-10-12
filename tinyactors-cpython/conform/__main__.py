"""python -m conform: reads calls from standard input and writes one reply
for each to standard output, until its input ends (driver.md)."""

import sys

from . import frames, scenario
from .frames import UNKNOWN_METHOD, Refused
from .vectors import Vectors


def main():
    vectors = Vectors()
    methods = dict(Vectors.METHODS)
    methods["scenario.run"] = scenario.run
    stdin, stdout = sys.stdin.buffer, sys.stdout.buffer
    while (data := frames.read(stdin)) is not None:
        call_id = None
        try:
            call_id, method, params = frames.call_of(data)
            function = methods.get(method)
            if not function:
                raise Refused(UNKNOWN_METHOD, "unknown", "method", "no such method")
            reply = {"id": call_id, "result": function(vectors, params)}
        except Refused as refused:
            reply = frames.error_reply(call_id, refused)
        frames.write(stdout, reply)
    vectors.close()


main()
