// This is a special construct, different from both "typeof (f<T>)" and
// "(typeof f)<T>" (which are both invalid)
type T = typeof f<T>;
