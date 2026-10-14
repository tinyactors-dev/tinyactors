// Code highlighting in the Tinyactors palette: pencil and ink on vellum,
// pine for the language's own words, ochre for text, slate for numbers.
const theme = (name, type, c) => ({
  name,
  type,
  colors: { 'editor.background': c.bg, 'editor.foreground': c.fg },
  tokenColors: [
    { settings: { foreground: c.fg } },
    { scope: ['comment', 'punctuation.definition.comment'], settings: { foreground: c.comment, fontStyle: 'italic' } },
    { scope: ['keyword', 'storage', 'storage.type', 'keyword.control', 'keyword.operator.new', 'keyword.operator.expression', 'variable.language'], settings: { foreground: c.keyword } },
    { scope: ['string', 'string.template', 'punctuation.definition.string'], settings: { foreground: c.string } },
    { scope: ['string.regexp', 'constant.other.character-class.regexp', 'keyword.operator.quantifier.regexp'], settings: { foreground: c.regexp } },
    { scope: ['constant.numeric', 'constant.language', 'support.constant', 'constant.language.boolean'], settings: { foreground: c.number } },
    { scope: ['entity.name.function', 'support.function', 'meta.function-call entity.name.function'], settings: { foreground: c.fn } },
    { scope: ['variable.other.property', 'meta.object-literal.key', 'support.type.property-name'], settings: { foreground: c.property } },
    { scope: ['entity.name.type', 'support.class', 'entity.name.class', 'support.type'], settings: { foreground: c.type } },
    { scope: ['punctuation', 'meta.brace', 'keyword.operator'], settings: { foreground: c.punctuation } },
    { scope: ['variable.parameter'], settings: { foreground: c.fg, fontStyle: 'italic' } },
    { scope: ['entity.name.tag', 'meta.preprocessor', 'keyword.control.directive'], settings: { foreground: c.keyword } },
  ],
});

export const vellum = theme('tinyactors-vellum', 'light', {
  bg: '#EEE7D9', fg: '#2A2622', comment: '#6b6154', keyword: '#1F5E4A', string: '#8a5d0c', regexp: '#4F6A40',
  number: '#3E5F70', fn: '#3A342E', property: '#5C544B', type: '#194D3D', punctuation: '#655b4f',
});

export const umber = theme('tinyactors-umber', 'dark', {
  bg: '#120D09', fg: '#F1E6D2', comment: '#8f7a64', keyword: '#86C4AB', string: '#D8A64E', regexp: '#9DB282',
  number: '#9DB6C3', fn: '#F1E6D2', property: '#CBBBA2', type: '#5FAE8F', punctuation: '#A38F76',
});
