"""Reading a call's params (driver.md): each method names the params it
takes; a param of the wrong type is -32602."""

from .frames import INVALID_PARAMS, Refused


def check(params, names):
    for key in params:
        if key not in names:
            raise Refused(INVALID_PARAMS, "unknown", f"params.{key}", "a param the method does not take")


def get(params, name, kind, default=None, required=False):
    if name not in params:
        if required:
            raise Refused(INVALID_PARAMS, "missing", f"params.{name}", f"params.{name}: missing")
        return default
    value = params[name]
    if kind is int and (isinstance(value, bool) or not isinstance(value, int)) or \
            kind is not int and not isinstance(value, kind):
        raise Refused(INVALID_PARAMS, "type", f"params.{name}", f"params.{name}: of the wrong type")
    return value
