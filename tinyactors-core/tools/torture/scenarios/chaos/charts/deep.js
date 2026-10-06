// Two branches of `depth` nested states; "swap" jumps between the deepest
// leaves (worst case for exit/entry sets and LCCA computation).
const depth = params.depth || 48;
function branch(name, d) {
  if (d === 0) return `<state id="${name}leaf"><transition event="swap" target="${name === 'l' ? 'r' : 'l'}leaf"/></state>`;
  return `<state id="${name}${d}"><onentry><raise event="entered"/></onentry>${branch(name, d - 1)}</state>`;
}
return `<scxml xmlns="http://www.w3.org/2005/07/scxml" version="1.0" datamodel="null" name="deep" initial="lleaf">
  <state id="root">${branch('l', depth)}${branch('r', depth)}<transition event="entered"/></state>
</scxml>`;
