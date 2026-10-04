let a // comment at end of declaration

// comment outside of declaration
let b /* comment between declarators */, c

/** comment with *stars* **/ /* comment with /slashes/ */
/* third comment in a row */

let d

let e
/* back to back *//* comments */

class C {
  method/*comment*/() {}
}

b
/* interleaved non-semi-insertion */
  .c
