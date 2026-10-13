"""Bridge the shared suite's driver protocol to the public Python module.

Run via ``mise run conform`` so the built extension is on the import path.
Frames are a four-byte unsigned big-endian length followed by one CBOR
message (tinyactors-test/docs/driver.md). The public binding codec handles
the message; this adapter handles only transport framing and, later, the
translation of driver methods to public binding calls.

This bootstrap implements framing only. Every method returns -32601 so
the suite diagnoses missing functionality. It never claims a chart passed
or exposes private core structs. Standard output contains frames only;
transport errors raise and go to standard error.
"""

import sys

import tinyactors


def read_exactly(stream, size):
    """Read size bytes from a blocking binary stream, joining short reads.

    Return independently owned bytes (including b"" when size is zero).
    EOF after a frame has started raises EOFError; the caller handles clean
    EOF between frames before using this helper. No stream is closed here.
    """
    chunks = bytearray()
    while len(chunks) < size:
        chunk = stream.read(size - len(chunks))
        if not chunk:
            raise EOFError("incomplete driver frame")
        chunks.extend(chunk)
    return bytes(chunks)


def main():
    """Serve sequential driver calls on stdin/stdout until clean EOF.

    Called by ``python -m conform``, not by an application driving actors.
    Inputs are the runner's well-formed call maps with integer ids. Match
    each id in an unimplemented-method reply and flush before reading the
    next call, so the synchronous runner cannot wait on buffered output.
    Truncated frames or invalid CBOR abort; there is no resynchronization.
    """
    source = sys.stdin.buffer
    target = sys.stdout.buffer
    while True:
        first = source.read(1)
        if not first:
            return
        header = first + read_exactly(source, 3)
        request = tinyactors.decode(read_exactly(source, int.from_bytes(header, "big")))
        reply = {
            "id": request["id"],
            "error": {
                "code": -32601,
                "message": "method is not implemented yet",
                "data": {"reason": "unknown", "path": "method"},
            },
        }
        encoded = tinyactors.encode(reply)
        target.write(len(encoded).to_bytes(4, "big") + encoded)
        target.flush()


if __name__ == "__main__":
    main()
