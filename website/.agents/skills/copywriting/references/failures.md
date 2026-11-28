# Failures found on this site

Each failure was found on www.tinyactors.dev, most of them by the
maintainer, and each has before-and-after pairs in `patterns.md`. Go
down the list in step 6 of the process (`../SKILL.md`). Failures 6 and
15 were revised on 2026-11-28 for the developer voice: a negative may
name the reader's current tool, and a question may quote the reader.

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
6. **A denial nobody needed.** "Costs no work", "not a framework" make
   the reader picture the thing denied. A negative earns its place only
   when it names the reader's current tool or belief ("The engine never
   opens a socket", against engines that own your I/O): that is how the
   enemy is named. Otherwise say what it is or does.
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
    explain. A question on the page only quotes the reader (an
    objection, a heading of frequently asked questions).
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

21. **Naming the audience.** "For the operator", "as a tech lead", "if you
    run agents in production": the roles in `site.xml` are the writer's
    target, never words on the page (the maintainer, 2026-11-25). Choose
    what to explain for that role and say it plainly.
22. **Explaining how the site is made.** "Each page below is made from
    the command's own help", "from the help of version X, which wrote
    everything on this page": the reader does not care how a page is
    built. Say what the thing does for them (the maintainer, 2026-11-25:
    "why would the reader care about how this is generated?").
