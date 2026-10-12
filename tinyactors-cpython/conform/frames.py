"""Frames and calls (driver.md, "Frames and messages"): a 4-byte big-endian
length, then one CBOR item; replies in JSON-RPC 2.0's shape without its
"jsonrpc" member."""

import struct
import sys

import tinyactors

PARSE_ERROR = -32700
INVALID_REQUEST = -32600
UNKNOWN_METHOD = -32601
INVALID_PARAMS = -32602
STATE = -32002


class Refused(Exception):
    """A call the adapter refuses with one of the protocol's errors."""

    def __init__(self, code, reason, path, message):
        super().__init__(message)
        self.code, self.reason, self.path = code, reason, path


def read(stream):
    """The next frame's bytes, or None at the end of the input."""
    head = stream.read(4)
    if not head:
        return None
    if len(head) != 4:
        raise EOFError("the input ended inside a frame's length")
    (length,) = struct.unpack(">I", head)
    data = stream.read(length)
    if len(data) != length:
        raise EOFError(f"a frame of {length} bytes could not be read")
    return data


def write(stream, reply):
    data = tinyactors.encode(reply)
    stream.write(struct.pack(">I", len(data)) + data)
    stream.flush()


def call_of(data):
    """(id, method, params) of a frame, or Refused with the id it has."""
    try:
        call = tinyactors.decode(data)
    except tinyactors.TinyactorsError:
        raise Refused(PARSE_ERROR, "malformed", "", "not one CBOR item") from None
    if not isinstance(call, dict):
        raise Refused(INVALID_REQUEST, "type", "", "a call is a map")
    for key in call:
        if key not in ("id", "method", "params"):
            raise Refused(INVALID_REQUEST, "unknown", "", "not a member of a call")
    if not isinstance(call.get("method"), str):
        raise Refused(INVALID_REQUEST, "missing" if "method" not in call else "type", "method", "a call names its method")
    if not isinstance(call.get("id"), int) or isinstance(call.get("id"), bool):
        raise Refused(INVALID_REQUEST, "missing", "id", "every call has an integer id")
    params = call.get("params", {})
    if not isinstance(params, dict):
        raise Refused(INVALID_REQUEST, "type", "params", "params is a map")
    return call["id"], call["method"], params


def error_reply(call_id, refused):
    return {"id": call_id, "error": {"code": refused.code, "message": str(refused),
                                     "data": {"reason": refused.reason, "path": refused.path}}}


def log(*parts):
    print("conform:", *parts, file=sys.stderr, flush=True)
