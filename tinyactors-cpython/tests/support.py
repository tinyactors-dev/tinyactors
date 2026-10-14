"""What the tests share: charts and a host made from them."""

import tinyactors

APP = "urn:test:app"


def chart(body, name="c", datamodel="python", initial="s", data=""):
    """An SCXML document with one <datamodel> and the states in `body`."""
    return ("<scxml xmlns='http://www.w3.org/2005/07/scxml' version='1.0' datamodel='%s' name='%s' initial='%s'>"
            "<datamodel>%s</datamodel>%s</scxml>" % (datamodel, name, initial, data, body))


def host_with(documents, processors=None, sync=False, **options):
    """A host on a simulated clock with one I/O processor of type APP, the
    documents compiled into one image and added."""
    config = {"systemID": "t", "system": {"ticksPerSecond": 1000},
              "ioProcessors": [{"type": APP, "sync": sync}]}
    options.setdefault("clock", tinyactors.SimulatedClock())
    host = tinyactors.Host(config, processors if processors is not None else {}, **options)
    with tinyactors.compile_charts(documents) as compiled:
        host.add_image(compiled.image)
    return host


class Logs:
    """A log handler that keeps (label, text) of every line."""

    def __init__(self):
        self.lines = []

    def __call__(self, host, entry):
        self.lines.append((entry.label, entry.text))

    def labelled(self, label):
        return [text for each, text in self.lines if each == label]
