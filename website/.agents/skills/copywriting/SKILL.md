---
name: copywriting
description: Write and review the copy of www.tinyactors.dev (website/content/). Use before writing or changing any headline, lede, heading, paragraph, claim, list item, caption or button label of the site, and to review copy someone else wrote. It holds the one test every sentence must pass (does it say something the reader did not know?), the failures to look for (labels instead of benefits, ledes that list what is coming, sentences that only point at the next block), how to fix each, and a command that prints a page's copy as plain text for review.
---

# Copywriting for the website

A reader gives the site a few seconds. Every sentence must tell them
something: a fact about tinyactors, what it does for them, or what to do
next. A sentence that only describes the page ("what X costs, how to Y")
spends those seconds and tells them nothing.

The voice rules are in `website/AGENTS.md` ("The voice: short and
literal"). This skill is about **substance**: what a sentence says, not
only how it sounds.

## The test

For each sentence, ask: **if I deleted it, would the reader know less
about tinyactors?** If not, delete it or replace it with the fact it was
standing in for.

## Failures, and their fixes

### 1. A label where a benefit should be

A headline names the page's topics instead of saying what the reader gets.

- Before: "Small actors, flat costs." (two labels, shaped like a slogan)
- After: "Small enough to use everywhere." (what small actors let you do)

A section heading names its finding, not its topic, when it has one:

- Before: "What an actor costs." / After: "An actor takes about 530 bytes."
- Before: "Costs stay flat." / After: "Twice the actors, the same cost."

A heading that is an instruction ("Size a host.", "1. Create a room.") is
fine: it says what the reader does there.

### 2. A lede that is a table of contents

The lede lists what the sections below will cover. The reader learns the
page's outline, not one thing about the product.

- Before: "What an actor costs in memory, how to size a host, and how each
  operation's cost holds as the population doubles. Every number comes
  from a program in the repository that you can run."
- After: "A started actor takes about 530 bytes and a hibernated one about
  120. Four million fit in one process, and each operation costs about the
  same at four million actors as at two."

Fix: take the most important facts the sections establish and state them.
The lede is the page's conclusion, not its index.

### 3. A sentence that only points at the next block

- Before: "Three rules that must hold whatever the service does." (then
  the code)
- After: "Whatever the service does, the payment charges at most three
  times, gives up only after the third charge, and is paid only when the
  service answered."
- Before: "Read what a host uses at any time:"
- After: "`hostStats` reports each pool's capacity, what it uses and its
  high water mark:"

Fix: say what the block shows, so a reader who skips the code still knows.
A colon before a code block is fine when the sentence carries a fact.

### 4. A sentence with no content

Vague claims about growth, power or flexibility: "It grows with your
system.", "a powerful way to", "seamlessly", "out of the box".

- Before: "This is a pattern, not a library. It grows with your system."
- After: "There is no library to install: the world, the properties, the
  generator, the search and the shrinker are 76 lines of your own code."

Fix: replace the adjective with the number or the mechanism behind it.
If there is none, delete the sentence.

### 5. Three of everything

A list of three qualities sounds complete, so it gets written by reflex:
"efficient, in process, scalable", "it leaves, finds food, comes home".
The reader weighs none of the three, and the rhythm gives the reflex away.

- Before: "Two fights kill it, and so do old age, after 6 to 14 trips, and
  your tap."
- After: "An egg becomes a larva, then a worker that forages for 6 to 14
  trips. Two fights or a tap end it sooner."

Fix: keep the one item that carries the point, or two when both matter.
Give an example ("such as arriving at food") instead of the catalogue.

### 6. What it is not

A sentence that defines something by what it is not ("no server", "costs
memory and no work", "drawing costs tinyactors nothing", "not a
framework") makes the reader think of the thing denied first.

- Before: "A resting ant costs memory and no work."
- After: "An ant at rest takes memory and waits for its next event."

Fix: say what it is or does. Keep a negative only when the reader holds
the belief it corrects and the sentence names that belief.

## Facts are checked

A number in copy comes from the repository: a transcluded table, a
measurement in an example's README, a guide. Find it before writing it,
and prefer showing the table on the page (`<table src>`) to quoting it.
When a measurement is redone, change the copy that quotes it too.

## Reviewing a page

Print every headline, lede, heading, paragraph, claim, list item and
caption of a page as plain text, and read it top to bottom without the
code and tables. Each line should hold up on its own:

```sh
cd website
sxml xpath --context-item=content/pages/performance.xml \
  'for $e in //*:headline | //*:lede | //*:heading | //*:p | //*:statement | //*:item/*:text | //*:caption
   return local-name($e) || ": " || normalize-space($e)'
```

(`sxml` is pinned in `website/mise.toml`; outside a mise shell, prefix the
command with `mise x --`.)

Then, for each line:

1. Apply the test. Mark the lines that would leave the reader knowing no
   less if deleted.
2. For a headline or heading: is it a label (two nouns, a topic) where it
   could state a benefit or a finding?
3. For a lede: does it list topics? Replace it with the page's two or three
   strongest facts.
4. For a sentence before code or a table: does it say what the block
   shows, or only that a block follows?
5. Look for lists of three and for sentences that say what something is
   not (failures 5 and 6).
6. Check every number against its source.

Fix what you marked, run `mise run test` in `website/`, and look at the
page in a browser, phone width included.
