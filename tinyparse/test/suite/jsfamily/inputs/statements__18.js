for (using item of items) {
  process(item);
}

for await (await using resource of resources) {
  consume(resource);
}
