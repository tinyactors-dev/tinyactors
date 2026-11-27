// What makes a stand-in or a client an I/O processor of the host: its
// function `(event, data) => answer` is called with each send, and the
// answer, given when its promise settles, is the send's answer; null fails
// the send (error.communication for the sender), 'silent' answers nothing
// (the chart's timeout decides).

export function processor(handle) {
  return {
    async receive(send) {
      const { event, data } = send;
      const result = await handle(event, data);
      if (result === 'silent') return;
      if (result === null || result === undefined) send.fail();
      else send.answer(result.event, result.data);
    },
  };
}
