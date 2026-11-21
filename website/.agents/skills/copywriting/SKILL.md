---
name: copywriting
description: Write and review the copy of www.tinyactors.dev (website/content/, and the words inside the site's components and live examples). Use before writing or changing any headline, lede, heading, paragraph, claim, list item, caption, button label, chart title or status line, before reordering a page's sections, and to review copy someone else wrote. It holds the test every sentence must pass, the failures found on this site with their fixes, how a page is ordered, and commands that print a page's copy for review; references/patterns.md holds the before-and-after pairs.
---

# Copywriting for the website

A reader gives the site a few seconds. Every sentence must tell them
something: a fact about tinyactors, what it does for them, or what to do
next. The voice rules are in `website/AGENTS.md` ("The voice: short and
literal"). This skill is about substance: what a sentence says, whether
it is true, and where it stands on the page.

**[references/patterns.md](references/patterns.md)** holds the patterns:
real sentences from this site before and after review, by kind of
element (headline, lede, heading, paragraph, demo page, component copy).
Read the section for what you are writing before you write it.

## Headings carry no period

A headline, a section's heading and a claim's statement end without a
period ("Build a team chat", "An actor takes about 530 bytes"); a
question keeps its question mark. The schema refuses a final period.
The before-and-after pairs in `references/patterns.md` were collected
when headings ended with one: read their wording, not their punctuation.

## The test

For each sentence, ask: **if I deleted it, would the reader know less
about tinyactors?** If not, delete it or replace it with the fact it was
standing in for.

Then ask: **is it true where the reader is?** On their device, in the code
the sentence introduces, of the thing the sentence names as its subject.

## Failures

Each has examples in `references/patterns.md`.

1. **A label where a benefit or a finding should be.** A headline names
   topics ("Small actors, flat costs."), a heading names a subject
   ("Watch them.", "What an actor costs."). State what the reader gets,
   or what the section found. An instruction is a good heading too
   ("Drop food and squash ants.").
2. **A lede that is a table of contents.** It lists what the sections
   cover. Give the page's strongest facts instead, or on a demo page the
   invitation to try it.
3. **A sentence that only points at the next block** ("Read what a host
   uses at any time:", "The charts under the field measure this tab.").
   Say what the block shows, or delete the sentence.
4. **A sentence with no content.** Adjectives about growth, power or ease
   ("It grows with your system."). Replace them with the number or the
   mechanism, or delete the sentence.
5. **Three of everything.** A list of three by reflex ("it leaves, finds
   food, comes home"). Keep the item that carries the point, or two when
   both matter; give one example instead of a catalogue.
6. **What it is not.** "No server", "costs no work", "not a framework"
   make the reader picture the thing denied. Say what it is or does. Keep
   a negative only when it corrects a belief the reader holds, and name
   the belief.
7. **A vague sentence where a fix removed something.** Taking out a
   triad or a negation leaves a gap; filling it with something vaguer
   ("An ant at rest takes memory and waits for its next event.") fails
   failure 4. Put the fact the removed words stood for ("An ant uses the
   CPU only while it handles a message.").
8. **The wrong subject.** The grammatical subject must be what does the
   thing: rests do not send messages, ants do. A loose subject is a
   loose claim.
9. **A claim the block below does not show.** A sentence before code
   describes that code. "A worker forages for 6 to 14 trips" before a
   chart that counts no trips sends the reader looking for something
   that is not there.
10. **A claim that depends on the reader's machine.** "Four million
    actors in a browser tab", "Each colony gets a CPU core" hold on one
    laptop. Claim what the page shows by default, and let the live
    numbers show the rest.
11. **Our pronouns.** "Your JavaScript" for code the reader did not
    write. Say whose: "a JavaScript object on this page".
12. **A term before it is explained.** "Host", "I/O processor" in a
    heading or early sentence, before the page says what they are. Use
    the reader's words first; introduce the term where its meaning is
    shown.
13. **A central claim with no proof on the page.** "Messages decide how
    many ants keep up" needs something the reader can see hold: a live
    chart of time per message, a table, an output. When nothing backs it,
    build the proof or drop the claim.
14. **Our plumbing instead of their use.** An excerpt of how the example
    is wired (a 50-line frame step, how two Workers split a field)
    teaches our simulation. Show what the reader would write or run.
15. **Saying what the reader should think.** "How would you build it?
    Picture where a million of those live." spells out the reaction the
    page should cause. Order the page so the reader asks it: show, then
    explain.
16. **Emphasis by pattern.** Italics go on the one word that carries the
    contrast. Copying the italic from the last headline ("in a *browser
    tab*") stresses nothing.
17. **Repeated shapes.** Several sentences in a row that open the same
    way ("Every ant…", "Each ant…", "Each colony…"), a heading repeated
    by the sentence under it, a lede that repeats the headline ("in this
    tab… in your browser"). Vary the shape or cut the repeat.
18. **Units the reader cannot read.** "1.00×", "Pace, % of real time"
    leave the reader guessing what is measured against what. Name the
    quantity in the reader's words ("Keeping up with real time", 99%)
    and draw the reference (a line at 100%).
19. **A number that misleads.** "2,071 bytes of WebAssembly memory an
    actor" divided the module's fixed size among few actors. A number on
    the page measures what its label says, at every setting the reader
    can choose.

20. **A solution without its problem.** A sentence names the mechanism
    ("A user at rest is saved to a store and woken by its next event")
    or a topic ("Who is in which room") and goes straight to how
    tinyactors handles it. The reader cannot weigh an answer to a
    problem they were never shown. State the problem first, in the
    reader's terms, then the answer: "Most users sit idle, but their
    state must survive until they return. A user at rest is saved to a
    store…". The maintainer has flagged this more than once: check every
    list item and card that describes a feature for its problem.

## How a page is ordered

- **A demo page** leads with the experience: the hero invites the reader
  to watch or try ("Watch a million ants forage and fight."), the demo
  follows at once, then how it works, piece by piece, then what it costs
  (after the mechanics the cost depends on), then how to run it
  yourself.
- **A guide** is steps, each heading an instruction ("1. Create a
  room."), each step a sentence and the code that does it.
- **A measurement page** leads with its numbers: the lede states them,
  the sections show the programs that made them.
- On every page, a section is one or two sentences of prose and then the
  proof: code, output, a table, a live chart.

## Facts are checked

A number in copy comes from the repository: a transcluded table, a
measurement in an example's README, a guide, or a live chart on the page
itself. Find it before writing it. Where the page measures live (the
anthill), the copy names what the reader will see and states no recorded
numbers. When a measurement is redone, change the copy that quotes it.
Keep the pitch's words (`website/AGENTS.md`, "The pitch"): properties
and generated runs, never formal verification.

## Reviewing a page

Print every headline, lede, heading, paragraph, claim, list item and
caption of a page as plain text, and read it top to bottom without the
code and tables:

```sh
cd website
sxml xpath --context-item=content/pages/anthill.xml \
  'for $e in //*:headline | //*:lede | //*:heading | //*:p | //*:statement | //*:item/*:text | //*:caption | //*:description
   return local-name($e) || ": " || normalize-space($e)'
```

(`sxml` is pinned in `website/mise.toml`; outside a mise shell, prefix the
command with `mise x --`.)

Copy also lives in the components and live examples: chart titles,
labels, buttons, status lines, progress messages. Print it too:

```sh
cd website
rg -n "title: '|textContent = |'aria-label'|, '[A-Z][a-z]+ '|\`[A-Z][a-z].*\\$\\{" components examples/*/page.mjs
```

Before changing any part of a page, read the whole page as it stands
now, top to bottom: what the reader knows when they reach the part, and
what they will read next. A fix made to one line against a page you have
not reread contradicts the rest of it. Before a link or a button points
somewhere, open the target: it must exist, be finished (not a draft or a
"to do"), and answer what the link promises at that point of the page.
A hero's "learn more", for instance, may best lead down the page itself,
to the section that explains the lede's terms.

Then, for each line:

1. Apply both tests (says something, true where the reader is).
2. Go down the failures above, and compare with the patterns for its kind
   of element in `references/patterns.md`.
3. Check every number against its source, and every sentence before a
   block against the block.
4. Read the page's order against "How a page is ordered".

Fix what you marked, run `mise run test` in `website/`, and look at the
page in a browser, phone width included.
