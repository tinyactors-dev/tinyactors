// Push-to-talk: one clip from the microphone, kept in memory only, given
// to the model as inline audio. Takes `navigator.mediaDevices` and
// `MediaRecorder` as dependencies, so tests inject fakes.

/** A recorder of one clip at a time. */
export function createRecorder({
  mediaDevices = globalThis.navigator?.mediaDevices,
  MediaRecorder = globalThis.MediaRecorder,
} = {}) {
  let current = null;
  return {
    get recording() { return current !== null; },

    /** Starts recording; rejects when there is no microphone or no permission. */
    async start() {
      if (current) throw new Error('Already recording.');
      if (!mediaDevices?.getUserMedia || !MediaRecorder) throw new Error('This browser cannot record audio here.');
      const stream = await mediaDevices.getUserMedia({ audio: true });
      const recorder = new MediaRecorder(stream);
      const chunks = [];
      recorder.ondataavailable = (event) => { if (event.data && event.data.size > 0) chunks.push(event.data); };
      const stopped = new Promise((resolve, reject) => {
        recorder.onstop = () => resolve();
        recorder.onerror = (event) => reject(event.error || new Error('The recording failed.'));
      });
      recorder.start();
      current = { stream, recorder, chunks, stopped };
    },

    /** Stops and returns the clip, `{mimeType, bytes}`; the tracks end. */
    async stop() {
      if (!current) throw new Error('Not recording.');
      const { stream, recorder, chunks, stopped } = current;
      current = null;
      try {
        recorder.stop();
        await stopped;
      } finally {
        for (const track of stream.getTracks()) track.stop();
      }
      const mimeType = (recorder.mimeType || chunks[0]?.type || 'audio/webm').split(';')[0];
      const blob = new Blob(chunks, { type: mimeType });
      return { mimeType, bytes: new Uint8Array(await blob.arrayBuffer()) };
    },

    /** Stops without a clip (the panel closed). */
    cancel() {
      if (!current) return;
      const { stream, recorder } = current;
      current = null;
      try { recorder.stop(); } catch { /* already stopped */ }
      for (const track of stream.getTracks()) track.stop();
    },
  };
}
