/* tinyactors/frame.h — link frames on byte streams (docs/protocol.md, "Framing").
 *
 *     frame = length (4 bytes, big-endian) + that many bytes of UTF-8 JSON
 *
 * For hosts that carry the link protocol over TCP, stdio or a serial line
 * next to the router (ta_host). Freestanding (ta_base): no allocation; the
 * caller owns the buffer a frame is read into. A frame above max_frame is
 * refused (answer it as `malformed`): its bytes are skipped as they arrive,
 * never buffered, so the stream stays in step.
 *
 *     ta_frame_reader reader;
 *     ta_frame_reader_init(&reader, sizeof buffer);
 *     while (n) {                                // bytes arrived: in[0..n)
 *         uint32_t event, k = ta_frame_read(&reader, buffer, in, n, &event);
 *         in += k, n -= k;
 *         if (event == TA_FRAME_READY) handle(buffer, reader.length);   // one JSON message
 *     }
 */
#ifndef TINYACTORS_FRAME_H
#define TINYACTORS_FRAME_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TA_FRAME_HEADER 4u          /* the length prefix */
#define TA_FRAME_MAX    (1u << 20u) /* the protocol's default max_frame (1 MiB) */

enum {
    TA_FRAME_MORE = 0,   /* every byte consumed, no frame complete yet */
    TA_FRAME_READY = 1,  /* buffer[0 .. reader->length) is a frame */
    TA_FRAME_REFUSED = 2 /* a frame of reader->length bytes is above max_frame: being skipped */
};

typedef struct ta_frame_reader {
    uint32_t size;      /* sizeof(ta_frame_reader) */
    uint32_t max_frame; /* frames above it are refused; the buffer holds at least this */
    uint32_t length;    /* the current frame's length, once its prefix is read */
    uint32_t have;      /* bytes of the current frame read (or skipped) */
    uint32_t head;      /* bytes of the prefix read (0-4) */
    uint32_t skip;      /* 1: the current frame is refused, its bytes are dropped */
} ta_frame_reader;

/* Write the prefix of a frame of `length` bytes; send it, then the bytes. */
TA_API void ta_frame_header(uint32_t length, uint8_t out[TA_FRAME_HEADER]);
TA_API void ta_frame_reader_init(ta_frame_reader *r, uint32_t max_frame);
/* Read from in[0..n) (any split of the stream) until a frame completes, a
 * frame is refused or the input ends. Returns the bytes consumed and sets
 * *event (TA_FRAME_*); call again with the rest. `buffer` must hold max_frame
 * bytes and is only written for frames that are not refused. */
TA_API uint32_t ta_frame_read(ta_frame_reader *r, uint8_t *buffer, const uint8_t *in, uint32_t n,
                              uint32_t *event);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_FRAME_H */
