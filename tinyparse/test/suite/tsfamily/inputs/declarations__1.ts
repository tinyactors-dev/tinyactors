declare class Error {
  constructor: Function
}

declare var foo: number;

declare const bar = "baz";

declare function greet(greeting: string): void;

declare namespace myLib {
    function makeGreeting(s: string): string;
    let numberOfGreetings: number;

    interface LogOptions {
      verbose?: boolean;
    }
    interface AlertOptions {
      modal: boolean;
      title?: string;
      color?: string;
    }
}

declare class Greeter {
  constructor(greeting: string);

  greeting: string;
  showGreeting(): void;
}

declare module Foo.Bar { export var foo; };

declare module Foo {
  break;
  continue;
  debugger;
  do { } while (true);
  for (x in null) { }
  for (;;) { }
  if (true) { } else { }
  1;
  return;
  switch (x) {
      case 1:
          break;
      default:
          break;
  }
  throw "hello";
  try { }
  catch (e) { }
  finally { }
}
