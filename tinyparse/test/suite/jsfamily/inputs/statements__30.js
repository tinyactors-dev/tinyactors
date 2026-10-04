theLoop:
for (;;) {
  if (a) {
    break theLoop;
  } else {
    continue theLoop;
  }
}

label
: {
  break label;
}

async:
while (true) {
  continue async;
}
