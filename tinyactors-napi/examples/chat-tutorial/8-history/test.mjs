import assert from 'node:assert/strict';
import { test } from 'node:test';
import { History, run } from './main.mjs';

// Actors run in no fixed order relative to each other: compare each phase,
// the lines between two "--" lines, as a sorted list.
const phases = (lines) => lines.join('\n').split(/^(?=-- )/m).map((phase) => phase.split('\n').filter(Boolean).sort());

for (const datamodel of ['native', 'quickjs']) {
  test(`8-history, ${datamodel}: a member who joins late reads the channel's history from the store, and a second join adds nobody`, async () => {
    assert.deepEqual(phases(await run({ datamodel })).slice(0, 2), phases([
      '-- ada and bob join and talk',
      'ada joins general and reads nothing yet',
      'bob joins general and reads nothing yet',
      'ada sees post 1 from ada: hi bob',
      'bob sees post 1 from ada: hi bob',
      'ada sees post 2 from bob: hi ada',
      'bob sees post 2 from bob: hi ada',
      '-- cat joins late, bob joins again',
      'cat joins general and reads ada: hi bob | bob: hi ada',
      'bob joins general and reads ada: hi bob | bob: hi ada',
      'ada sees post 3 from ada: welcome, cat',
      'bob sees post 3 from ada: welcome, cat',
      'cat sees post 3 from ada: welcome, cat',
    ]));
  });
}

for (const datamodel of ['native', 'quickjs']) {
  test(`8-history, ${datamodel}: a user who signs out reads the posts made meanwhile from the store when their connection opens`, async () => {
    assert.deepEqual(phases(await run({ datamodel })).at(-1), phases([
      '-- cat signs out, and back in',
      'ada sees post 4 from bob: lunch at noon',
      'bob sees post 4 from bob: lunch at noon',
      'cat connects and reads in general: bob: lunch at noon',
    ])[0]);
  });
}

// A store that refuses bob's post: the room gets error.communication for
// it, so the post is neither stored nor delivered, and the next one takes
// the number it would have had.
class Refusing extends History {
  receive(send) {
    if (send.data.text === 'hi ada') send.fail();
    else super.receive(send);
  }
}

for (const datamodel of ['native', 'quickjs']) {
  test(`8-history, ${datamodel}: a post the store refuses is neither stored nor delivered`, async () => {
    assert.deepEqual(phases(await run({ datamodel, history: new Refusing() })).slice(0, 2), phases([
      '-- ada and bob join and talk',
      'ada joins general and reads nothing yet',
      'bob joins general and reads nothing yet',
      'ada sees post 1 from ada: hi bob',
      'bob sees post 1 from ada: hi bob',
      '-- cat joins late, bob joins again',
      'cat joins general and reads ada: hi bob',
      'bob joins general and reads ada: hi bob',
      'ada sees post 2 from ada: welcome, cat',
      'bob sees post 2 from ada: welcome, cat',
      'cat sees post 2 from ada: welcome, cat',
    ]));
  });
}
