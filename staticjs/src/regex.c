/*
 * Regular expression patterns (docs/regex.md, sections 4 to 8): a literal's
 * flags and body read, every construct sorted into exact, unsupported or a
 * syntax error, and an exact one compiled to a program for the automaton of
 * regex_match.c.
 *
 * The pattern is read twice, as JavaScript reads it. A first pass counts the
 * capture groups and finds their names, which a reference may name before
 * the group it refers to. The second builds a tree of nodes, iteratively:
 * groups nest on an explicit stack of open groups, bounded by max_nesting,
 * and a quantifier wraps the atom before it in place. Reading goes on after
 * an unsupported construct, so that a syntax error later in the pattern is
 * still found: JavaScript refuses the whole program for it. Compiling walks
 * the tree with an explicit stack too, once for its sizes and once to emit
 * its instructions; a quantifier's body is emitted once per copy.
 *
 * Without the u or v flag the pattern is read as UTF-16 code units, so that
 * a character outside the BMP is two characters, as in JavaScript; with u or
 * v, as code points.
 */
#include "regex.h"
#include "regex_program.h"

/* ------------------------------------------------------------------------
 * Flags
 */

static const char FLAG_LETTERS[] = "dgimsuvy";

bool sjs_regex_flags(sjs_str text, uint32_t *flags) {
    uint32_t set = 0;
    for (uint32_t i = 0; i < text.length; i++) {
        uint32_t bit = 0;
        for (uint32_t k = 0; k < 8u; k++) {
            if (text.ptr[i] == FLAG_LETTERS[k]) bit = 1u << k;
        }
        if (bit == 0 || (set & bit) != 0) return false;
        set |= bit;
    }
    if ((set & SJS_REGEX_UNICODE) != 0 && (set & SJS_REGEX_UNICODE_SETS) != 0) return false;
    *flags = set;
    return true;
}

uint32_t sjs_regex_flag_text(uint32_t flags, char *out) {
    uint32_t n = 0;
    for (uint32_t k = 0; k < 8u; k++) {
        if ((flags & (1u << k)) != 0) out[n++] = FLAG_LETTERS[k];
    }
    return n;
}

/* ------------------------------------------------------------------------
 * Unicode property names, for \p{...} and \P{...} with u or v: the names
 * Node 22 accepts, which unicode-property-aliases-ecmascript and
 * unicode-property-value-aliases-ecmascript 3.0.0 list, each kept when Node
 * 22.23.2 compiles it. A known name is JavaScript staticjs leaves out
 * (unsupported); any other is a syntax error. Sorted by bytes.
 */

static const char *const PROPERTY_NAMES[] = {
    "AHex", "ASCII", "ASCII_Hex_Digit", "Alpha", "Alphabetic", "Any", "Assigned", "Bidi_C",
    "Bidi_Control", "Bidi_M", "Bidi_Mirrored", "C", "CI", "CWCF", "CWCM", "CWKCF", "CWL", "CWT",
    "CWU", "Case_Ignorable", "Cased", "Cased_Letter", "Cc", "Cf", "Changes_When_Casefolded",
    "Changes_When_Casemapped", "Changes_When_Lowercased", "Changes_When_NFKC_Casefolded",
    "Changes_When_Titlecased", "Changes_When_Uppercased", "Close_Punctuation", "Cn", "Co",
    "Combining_Mark", "Connector_Punctuation", "Control", "Cs", "Currency_Symbol", "DI", "Dash",
    "Dash_Punctuation", "Decimal_Number", "Default_Ignorable_Code_Point", "Dep", "Deprecated",
    "Dia", "Diacritic", "EBase", "EComp", "EMod", "EPres", "Emoji", "Emoji_Component",
    "Emoji_Modifier", "Emoji_Modifier_Base", "Emoji_Presentation", "Enclosing_Mark", "Ext",
    "ExtPict", "Extended_Pictographic", "Extender", "Final_Punctuation", "Format", "Gr_Base",
    "Gr_Ext", "Grapheme_Base", "Grapheme_Extend", "Hex", "Hex_Digit", "IDC", "IDS", "IDSB", "IDST",
    "IDS_Binary_Operator", "IDS_Trinary_Operator", "ID_Continue", "ID_Start", "Ideo", "Ideographic",
    "Initial_Punctuation", "Join_C", "Join_Control", "L", "LC", "LOE", "Letter", "Letter_Number",
    "Line_Separator", "Ll", "Lm", "Lo", "Logical_Order_Exception", "Lower", "Lowercase",
    "Lowercase_Letter", "Lt", "Lu", "M", "Mark", "Math", "Math_Symbol", "Mc", "Me", "Mn",
    "Modifier_Letter", "Modifier_Symbol", "N", "NChar", "Nd", "Nl", "No", "Noncharacter_Code_Point",
    "Nonspacing_Mark", "Number", "Open_Punctuation", "Other", "Other_Letter", "Other_Number",
    "Other_Punctuation", "Other_Symbol", "P", "Paragraph_Separator", "Pat_Syn", "Pat_WS",
    "Pattern_Syntax", "Pattern_White_Space", "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Private_Use",
    "Ps", "Punctuation", "QMark", "Quotation_Mark", "RI", "Radical", "Regional_Indicator", "S",
    "SD", "STerm", "Sc", "Sentence_Terminal", "Separator", "Sk", "Sm", "So", "Soft_Dotted",
    "Space_Separator", "Spacing_Mark", "Surrogate", "Symbol", "Term", "Terminal_Punctuation",
    "Titlecase_Letter", "UIdeo", "Unassigned", "Unified_Ideograph", "Upper", "Uppercase",
    "Uppercase_Letter", "VS", "Variation_Selector", "WSpace", "White_Space", "XIDC", "XIDS",
    "XID_Continue", "XID_Start", "Z", "Zl", "Zp", "Zs", "cntrl", "digit", "punct", "space",
};

/* Values of General_Category= and gc=. */
static const char *const CATEGORY_VALUES[] = {
    "C", "Cased_Letter", "Cc", "Cf", "Close_Punctuation", "Cn", "Co", "Combining_Mark",
    "Connector_Punctuation", "Control", "Cs", "Currency_Symbol", "Dash_Punctuation",
    "Decimal_Number", "Enclosing_Mark", "Final_Punctuation", "Format", "Initial_Punctuation", "L",
    "LC", "Letter", "Letter_Number", "Line_Separator", "Ll", "Lm", "Lo", "Lowercase_Letter", "Lt",
    "Lu", "M", "Mark", "Math_Symbol", "Mc", "Me", "Mn", "Modifier_Letter", "Modifier_Symbol", "N",
    "Nd", "Nl", "No", "Nonspacing_Mark", "Number", "Open_Punctuation", "Other", "Other_Letter",
    "Other_Number", "Other_Punctuation", "Other_Symbol", "P", "Paragraph_Separator", "Pc", "Pd",
    "Pe", "Pf", "Pi", "Po", "Private_Use", "Ps", "Punctuation", "S", "Sc", "Separator", "Sk", "Sm",
    "So", "Space_Separator", "Spacing_Mark", "Surrogate", "Symbol", "Titlecase_Letter",
    "Unassigned", "Uppercase_Letter", "Z", "Zl", "Zp", "Zs", "cntrl", "digit", "punct",
};

/* Values of Script=, sc=, Script_Extensions= and scx=. */
static const char *const SCRIPT_VALUES[] = {
    "Adlam", "Adlm", "Aghb", "Ahom", "Anatolian_Hieroglyphs", "Arab", "Arabic", "Armenian", "Armi",
    "Armn", "Avestan", "Avst", "Bali", "Balinese", "Bamu", "Bamum", "Bass", "Bassa_Vah", "Batak",
    "Batk", "Beng", "Bengali", "Berf", "Beria_Erfe", "Bhaiksuki", "Bhks", "Bopo", "Bopomofo",
    "Brah", "Brahmi", "Brai", "Braille", "Bugi", "Buginese", "Buhd", "Buhid", "Cakm",
    "Canadian_Aboriginal", "Cans", "Cari", "Carian", "Caucasian_Albanian", "Chakma", "Cham", "Cher",
    "Cherokee", "Chorasmian", "Chrs", "Common", "Copt", "Coptic", "Cpmn", "Cprt", "Cuneiform",
    "Cypriot", "Cypro_Minoan", "Cyrillic", "Cyrl", "Deseret", "Deva", "Devanagari", "Diak",
    "Dives_Akuru", "Dogr", "Dogra", "Dsrt", "Dupl", "Duployan", "Egyp", "Egyptian_Hieroglyphs",
    "Elba", "Elbasan", "Elym", "Elymaic", "Ethi", "Ethiopic", "Gara", "Garay", "Geor", "Georgian",
    "Glag", "Glagolitic", "Gong", "Gonm", "Goth", "Gothic", "Gran", "Grantha", "Greek", "Grek",
    "Gujarati", "Gujr", "Gukh", "Gunjala_Gondi", "Gurmukhi", "Guru", "Gurung_Khema", "Han", "Hang",
    "Hangul", "Hani", "Hanifi_Rohingya", "Hano", "Hanunoo", "Hatr", "Hatran", "Hebr", "Hebrew",
    "Hira", "Hiragana", "Hluw", "Hmng", "Hmnp", "Hung", "Imperial_Aramaic", "Inherited",
    "Inscriptional_Pahlavi", "Inscriptional_Parthian", "Ital", "Java", "Javanese", "Kaithi", "Kali",
    "Kana", "Kannada", "Katakana", "Kawi", "Kayah_Li", "Khar", "Kharoshthi", "Khitan_Small_Script",
    "Khmer", "Khmr", "Khoj", "Khojki", "Khudawadi", "Kirat_Rai", "Kits", "Knda", "Krai", "Kthi",
    "Lana", "Lao", "Laoo", "Latin", "Latn", "Lepc", "Lepcha", "Limb", "Limbu", "Lina", "Linb",
    "Linear_A", "Linear_B", "Lisu", "Lyci", "Lycian", "Lydi", "Lydian", "Mahajani", "Mahj", "Maka",
    "Makasar", "Malayalam", "Mand", "Mandaic", "Mani", "Manichaean", "Marc", "Marchen",
    "Masaram_Gondi", "Medefaidrin", "Medf", "Meetei_Mayek", "Mend", "Mende_Kikakui", "Merc", "Mero",
    "Meroitic_Cursive", "Meroitic_Hieroglyphs", "Miao", "Mlym", "Modi", "Mong", "Mongolian", "Mro",
    "Mroo", "Mtei", "Mult", "Multani", "Myanmar", "Mymr", "Nabataean", "Nag_Mundari", "Nagm",
    "Nand", "Nandinagari", "Narb", "Nbat", "New_Tai_Lue", "Newa", "Nko", "Nkoo", "Nshu", "Nushu",
    "Nyiakeng_Puachue_Hmong", "Ogam", "Ogham", "Ol_Chiki", "Ol_Onal", "Olck", "Old_Hungarian",
    "Old_Italic", "Old_North_Arabian", "Old_Permic", "Old_Persian", "Old_Sogdian",
    "Old_South_Arabian", "Old_Turkic", "Old_Uyghur", "Onao", "Oriya", "Orkh", "Orya", "Osage",
    "Osge", "Osma", "Osmanya", "Ougr", "Pahawh_Hmong", "Palm", "Palmyrene", "Pau_Cin_Hau", "Pauc",
    "Perm", "Phag", "Phags_Pa", "Phli", "Phlp", "Phnx", "Phoenician", "Plrd", "Prti",
    "Psalter_Pahlavi", "Qaac", "Qaai", "Rejang", "Rjng", "Rohg", "Runic", "Runr", "Samaritan",
    "Samr", "Sarb", "Saur", "Saurashtra", "Sgnw", "Sharada", "Shavian", "Shaw", "Shrd", "Sidd",
    "Siddham", "Sidetic", "Sidt", "SignWriting", "Sind", "Sinh", "Sinhala", "Sogd", "Sogdian",
    "Sogo", "Sora", "Sora_Sompeng", "Soyo", "Soyombo", "Sund", "Sundanese", "Sunu", "Sunuwar",
    "Sylo", "Syloti_Nagri", "Syrc", "Syriac", "Tagalog", "Tagb", "Tagbanwa", "Tai_Le", "Tai_Tham",
    "Tai_Viet", "Tai_Yo", "Takr", "Takri", "Tale", "Talu", "Tamil", "Taml", "Tang", "Tangsa",
    "Tangut", "Tavt", "Tayo", "Telu", "Telugu", "Tfng", "Tglg", "Thaa", "Thaana", "Thai", "Tibetan",
    "Tibt", "Tifinagh", "Tirh", "Tirhuta", "Tnsa", "Todhri", "Todr", "Tolong_Siki", "Tols", "Toto",
    "Tulu_Tigalari", "Tutg", "Ugar", "Ugaritic", "Unknown", "Vai", "Vaii", "Vith", "Vithkuqi",
    "Wancho", "Wara", "Warang_Citi", "Wcho", "Xpeo", "Xsux", "Yezi", "Yezidi", "Yi", "Yiii",
    "Zanabazar_Square", "Zanb", "Zinh", "Zyyy", "Zzzz",
};

/* Properties of strings: with v only, and never in \P or a negated class. */
static const char *const STRING_PROPERTIES[] = {
    "Basic_Emoji", "Emoji_Keycap_Sequence", "RGI_Emoji", "RGI_Emoji_Flag_Sequence",
    "RGI_Emoji_Modifier_Sequence", "RGI_Emoji_Tag_Sequence", "RGI_Emoji_ZWJ_Sequence",
};

typedef struct {
    const char *const *names;
    uint32_t count;
} rx_list;

#define LIST(list) ((rx_list){ (list), (uint32_t)(sizeof(list) / sizeof((list)[0])) })

/* Is `name` (pattern characters) one of the names of `list`, which is
 * sorted by bytes? */
static bool rx_name_listed(const uint32_t *name, uint32_t length, rx_list list) {
    uint32_t low = 0;
    uint32_t high = list.count;
    while (low < high) {
        uint32_t mid = low + (high - low) / 2u;
        const char *s = list.names[mid];
        int32_t order = 0;
        uint32_t i = 0;
        for (; order == 0; i++) {
            uint32_t a = i < length ? name[i] : 0;
            uint32_t b = (uint8_t)s[i];
            if (i >= length && b == 0) break;
            order = a < b ? -1 : (a > b ? 1 : 0);
        }
        if (order == 0) return true;
        if (order < 0) high = mid;
        else low = mid + 1u;
    }
    return false;
}

static bool rx_chars_are(const uint32_t *chars, uint32_t length, const char *text) {
    uint32_t i = 0;
    for (; i < length; i++) {
        if (text[i] == '\0' || chars[i] != (uint8_t)text[i]) return false;
    }
    return text[i] == '\0';
}

/* What a \p{...} names: P_NONE when JavaScript refuses it. */
enum { P_NONE = 0, P_CHARACTERS, P_STRINGS };

static uint8_t rx_property_kind(const uint32_t *name, uint32_t length, bool sets) {
    uint32_t eq = 0;
    while (eq < length && name[eq] != '=') eq++;
    if (eq == length) {
        if (rx_name_listed(name, length, LIST(PROPERTY_NAMES))) return P_CHARACTERS;
        if (sets && rx_name_listed(name, length, LIST(STRING_PROPERTIES))) return P_STRINGS;
        return P_NONE;
    }
    const uint32_t *value = name + eq + 1u;
    uint32_t value_length = length - eq - 1u;
    if (rx_chars_are(name, eq, "General_Category") || rx_chars_are(name, eq, "gc")) {
        return rx_name_listed(value, value_length, LIST(CATEGORY_VALUES)) ? P_CHARACTERS : P_NONE;
    }
    if (rx_chars_are(name, eq, "Script") || rx_chars_are(name, eq, "sc") || rx_chars_are(name, eq, "Script_Extensions") ||
        rx_chars_are(name, eq, "scx")) {
        return rx_name_listed(value, value_length, LIST(SCRIPT_VALUES)) ? P_CHARACTERS : P_NONE;
    }
    return P_NONE;
}

/* ------------------------------------------------------------------------
 * The parser's state
 */

#define NONE UINT32_MAX
/* The largest count a quantifier can give: V8 reads a larger one as this. */
#define COUNT_MAX 2147483647u

/* Tree nodes. */
enum { N_EMPTY = 0, N_CHAR, N_ANY, N_CLASS, N_ASSERT, N_GROUP, N_CONCAT, N_ALT, N_REPEAT };

typedef struct {
    uint8_t type;
    uint8_t greedy;     /* N_REPEAT */
    uint8_t capture;    /* N_GROUP: 1 for a capture group */
    uint32_t value;     /* N_CHAR: the character; N_CLASS: its class; N_ASSERT: RX_ASSERT_*; N_GROUP: its number */
    uint32_t min;       /* N_REPEAT */
    uint32_t max;       /* N_REPEAT: RX_INFINITE for none */
    uint32_t first;     /* the first child, or NONE */
    uint32_t last;      /* the last child, or NONE */
    uint32_t next;      /* the next sibling, or NONE */
} rx_node;

/* A group being read: its alternatives (an N_ALT), the one being read (an
 * N_CONCAT), and the group's node (NONE for the whole pattern). */
enum { G_ROOT = 0, G_PLAIN, G_CAPTURE, G_LOOKAHEAD, G_LOOKBEHIND };

typedef struct {
    uint32_t alt;
    uint32_t concat;
    uint32_t group;
    uint8_t kind;
} rx_open;

/* A name of a group, from the first pass: where it is in the pattern. */
typedef struct {
    uint32_t at;
    uint32_t length;
} rx_name;

typedef struct {
    const sjs_allocator *allocator;
    sjs_limits limits;
    sjs_regex_literal literal;
    uint32_t flags;
    bool unicode;          /* u or v: code points, and no Annex B */
    bool sets;             /* v */
    bool fold;             /* i */
    sjs_buf chars;         /* the pattern's characters, uint32_t each */
    const uint32_t *pat;
    uint32_t n;
    uint32_t i;
    /* The first pass. */
    uint32_t groups;
    bool named;
    bool exotic_names;     /* a name with an escape or a character that is not ASCII */
    sjs_buf names;         /* rx_name, one per capture group, by number - 1; length 0 for none */
    /* The second. */
    sjs_buf nodes;         /* rx_node */
    sjs_buf opens;         /* rx_open */
    uint32_t captured;     /* groups opened so far */
    uint32_t atom;         /* the node a quantifier would repeat, or NONE */
    /* A class being read, and the classes read: ranges of characters, two
     * uint32_t each, low and high; each class an rx_class into them. */
    sjs_buf work;
    sjs_buf ranges;
    sjs_buf classes;
    /* The verdict so far: a syntax error ends the reading. */
    uint8_t outcome;
    const char *message;
    bool nomem;
} rx_parser;

static bool rx_stopped(const rx_parser *p) {
    return p->outcome == SJS_REGEX_SYNTAX || p->outcome == SJS_REGEX_LIMIT || p->nomem;
}

static void rx_syntax(rx_parser *p, const char *message) {
    if (rx_stopped(p)) return;
    p->outcome = SJS_REGEX_SYNTAX;
    p->message = message;
}

static void rx_unsupported(rx_parser *p, const char *message) {
    if (p->outcome != SJS_REGEX_EXACT) return;
    p->outcome = SJS_REGEX_UNSUPPORTED;
    p->message = message;
}

/* Append to a buffer of the parser's; false, once out of memory. */
static bool rx_add_bytes(rx_parser *p, sjs_buf *buf, sjs_bytes bytes) {
    if (p->nomem) return false;
    if (sjs_buf_append(p->allocator, buf, bytes) != SJS_OK) {
        p->nomem = true;
        return false;
    }
    return true;
}

#define rx_add(p, buf, item, size) rx_add_bytes((p), (buf), (sjs_bytes){ (const uint8_t *)(const void *)(item), (size) })

static uint32_t rx_at(const rx_parser *p, uint32_t i) {
    return i < p->n ? p->pat[i] : NONE;
}

static bool rx_is_digit(uint32_t c) {
    return c >= '0' && c <= '9';
}

static int32_t rx_hex(uint32_t c) {
    if (c >= '0' && c <= '9') return (int32_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (int32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (int32_t)(c - 'A' + 10);
    return -1;
}

static bool rx_is_ascii_letter(uint32_t c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/* SyntaxCharacter, which an escape may always name, and `/`. */
static bool rx_is_syntax_char(uint32_t c) {
    switch (c) {
    case '^': case '$': case '\\': case '.': case '*': case '+': case '?':
    case '(': case ')': case '[': case ']': case '{': case '}': case '|': case '/':
        return true;
    default:
        return false;
    }
}

static rx_node *rx_node_at(rx_parser *p, uint32_t i) {
    return &SJS_BUF_AT(p->nodes, rx_node, i);
}

static uint32_t rx_new_node(rx_parser *p, uint8_t type, uint32_t value) {
    rx_node n = { 0 };
    n.type = type;
    n.value = value;
    n.first = NONE;
    n.last = NONE;
    n.next = NONE;
    uint32_t index = SJS_BUF_COUNT(p->nodes, rx_node);
    if (!rx_add(p, &p->nodes, &n, sizeof n)) return NONE;
    return index;
}

static void rx_append_child(rx_parser *p, uint32_t parent, uint32_t child) {
    rx_node *x = rx_node_at(p, parent);
    if (x->last == NONE) x->first = child;
    else rx_node_at(p, x->last)->next = child;
    x->last = child;
}

static rx_open *rx_top_open(rx_parser *p) {
    return &SJS_BUF_AT(p->opens, rx_open, SJS_BUF_COUNT(p->opens, rx_open) - 1u);
}

/* An atom, or an assertion (`quantifiable` false), at the end of the
 * alternative being read. */
static void rx_add_term(rx_parser *p, uint32_t node, bool quantifiable) {
    if (node == NONE) return;
    rx_append_child(p, rx_top_open(p)->concat, node);
    p->atom = quantifiable ? node : NONE;
}

/* With i, staticjs has case tables for ASCII letters only. */
static void rx_folded_char(rx_parser *p, uint32_t c) {
    if (p->fold && c >= 0x80u) rx_unsupported(p, "staticjs matches a regular expression with the i flag only where its letters are ASCII");
}

static void rx_add_char(rx_parser *p, uint32_t c) {
    rx_folded_char(p, c);
    rx_add_term(p, rx_new_node(p, N_CHAR, c), true);
}

/* ------------------------------------------------------------------------
 * The first pass: capture groups and their names
 */

static void rx_prescan(rx_parser *p) {
    uint32_t depth = 0; /* classes open: with v they nest */
    for (uint32_t i = 0; i < p->n; i++) {
        uint32_t c = p->pat[i];
        if (c == '\\') {
            i++;
            continue;
        }
        if (depth > 0) {
            if (c == ']') depth--;
            else if (c == '[' && p->sets) depth++;
            continue;
        }
        if (c == '[') {
            depth = 1;
            continue;
        }
        if (c != '(') continue;
        rx_name name = { 0, 0 };
        if (rx_at(p, i + 1u) == '?') {
            if (rx_at(p, i + 2u) != '<' || rx_at(p, i + 3u) == '=' || rx_at(p, i + 3u) == '!') continue;
            uint32_t j = i + 3u;
            while (j < p->n && p->pat[j] != '>') {
                if (p->pat[j] == '\\' || p->pat[j] >= 0x80u) p->exotic_names = true;
                j++;
            }
            name.at = i + 3u;
            name.length = j - name.at;
            p->named = true;
        }
        p->groups++;
        if (!rx_add(p, &p->names, &name, sizeof name)) return;
    }
}

/* The group whose name is the pattern's characters [from, from + length),
 * or 0 for none. */
static uint32_t rx_group_named(const rx_parser *p, uint32_t from, uint32_t length) {
    for (uint32_t g = 0; g < p->groups; g++) {
        const rx_name *name = &SJS_BUF_AT(p->names, rx_name, g);
        if (name->length != length || length == 0) continue;
        uint32_t k = 0;
        while (k < length && p->pat[name->at + k] == p->pat[from + k]) k++;
        if (k == length) return g + 1u;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Escapes
 */

/* \u after the backslash, at `u`: RegExpUnicodeEscapeSequence. With u or v
 * (`unicode`) a {code point}, or a pair of \u escapes that make one; else
 * four hex digits. False, with nothing read, when none follows. */
static bool rx_unicode_escape(rx_parser *p, bool unicode, uint32_t *value) {
    uint32_t i = p->i + 1u;
    if (unicode && rx_at(p, i) == '{') {
        uint64_t v = 0;
        uint32_t digits = 0;
        i++;
        while (rx_hex(rx_at(p, i)) >= 0) {
            v = v * 16u + (uint64_t)rx_hex(rx_at(p, i));
            if (v > 0x10FFFFu) return false;
            digits++;
            i++;
        }
        if (digits == 0 || rx_at(p, i) != '}') return false;
        p->i = i + 1u;
        *value = (uint32_t)v;
        return true;
    }
    uint32_t v = 0;
    for (uint32_t k = 0; k < 4u; k++) {
        int32_t d = rx_hex(rx_at(p, i + k));
        if (d < 0) return false;
        v = v * 16u + (uint32_t)d;
    }
    i += 4u;
    if (unicode && v >= 0xD800u && v <= 0xDBFFu && rx_at(p, i) == '\\' && rx_at(p, i + 1u) == 'u') {
        uint32_t w = 0;
        bool ok = true;
        for (uint32_t k = 0; k < 4u; k++) {
            int32_t d = rx_hex(rx_at(p, i + 2u + k));
            if (d < 0) ok = false;
            else w = w * 16u + (uint32_t)d;
        }
        if (ok && w >= 0xDC00u && w <= 0xDFFFu) {
            v = 0x10000u + ((v - 0xD800u) << 10) + (w - 0xDC00u);
            i += 6u;
        }
    }
    p->i = i;
    *value = v;
    return true;
}

/* A CharacterEscape after the backslash, at p->i, that is not a decimal,
 * \k, \b or a class escape: a character, read. An identity escape staticjs
 * leaves out (Annex B) is read as the character it names. False after a
 * syntax error. */
static bool rx_character_escape(rx_parser *p, uint32_t *value) {
    uint32_t c = rx_at(p, p->i);
    switch (c) {
    case 'f': *value = '\f'; p->i++; return true;
    case 'n': *value = '\n'; p->i++; return true;
    case 'r': *value = '\r'; p->i++; return true;
    case 't': *value = '\t'; p->i++; return true;
    case 'v': *value = '\v'; p->i++; return true;
    case 'x': {
        int32_t a = rx_hex(rx_at(p, p->i + 1u));
        int32_t b = rx_hex(rx_at(p, p->i + 2u));
        if (a >= 0 && b >= 0) {
            *value = (uint32_t)(a * 16 + b);
            p->i += 3u;
            return true;
        }
        if (p->unicode) {
            rx_syntax(p, "Invalid escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out \\x without two hexadecimal digits (Annex B)");
        *value = 'x';
        p->i++;
        return true;
    }
    case 'u':
        if (rx_unicode_escape(p, p->unicode, value)) return true;
        if (p->unicode) {
            rx_syntax(p, "Invalid Unicode escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out \\u without four hexadecimal digits (Annex B)");
        *value = 'u';
        p->i++;
        return true;
    default:
        if (rx_is_syntax_char(c)) {
            *value = c;
            p->i++;
            return true;
        }
        if (p->unicode) {
            rx_syntax(p, "Invalid escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out identity escapes of characters that are not syntax (Annex B)");
        *value = c;
        p->i++;
        return true;
    }
}

/* \p{...} or \P{...} with u or v, at the p: a property staticjs has no
 * tables for, or a syntax error. P_STRINGS for a property of strings. */
static uint8_t rx_property_escape(rx_parser *p) {
    bool negated = rx_at(p, p->i) == 'P';
    p->i++;
    if (rx_at(p, p->i) != '{') {
        rx_syntax(p, "Invalid property name");
        return P_NONE;
    }
    uint32_t start = p->i + 1u;
    uint32_t j = start;
    while (j < p->n && p->pat[j] != '}') j++;
    if (j >= p->n) {
        rx_syntax(p, "Invalid property name");
        return P_NONE;
    }
    uint8_t kind = rx_property_kind(p->pat + start, j - start, p->sets);
    p->i = j + 1u;
    if (kind == P_NONE || (kind == P_STRINGS && negated)) {
        rx_syntax(p, "Invalid property name");
        return P_NONE;
    }
    rx_unsupported(p, "staticjs has no Unicode property tables for \\p{...}");
    return kind;
}

/* \k after the backslash, at p->i. */
static void rx_named_reference(rx_parser *p) {
    if (!p->unicode && !p->named) {
        rx_unsupported(p, "staticjs leaves out \\k as an identity escape (Annex B)");
        p->i++;
        return;
    }
    if (rx_at(p, p->i + 1u) != '<') {
        rx_syntax(p, "Invalid named reference");
        return;
    }
    uint32_t start = p->i + 2u;
    uint32_t j = start;
    bool exotic = false;
    while (j < p->n && p->pat[j] != '>') {
        if (p->pat[j] == '\\' || p->pat[j] >= 0x80u) exotic = true;
        j++;
    }
    if (j >= p->n || j == start) {
        rx_syntax(p, "Invalid named reference");
        return;
    }
    p->i = j + 1u;
    if (!exotic && !p->exotic_names && rx_group_named(p, start, j - start) == 0) {
        rx_syntax(p, "Invalid named capture referenced");
        return;
    }
    rx_unsupported(p, "staticjs has no backreferences: a linear-time automaton cannot match them");
}

/* The number of the decimal digits at p->i, read; COUNT_MAX past it. */
static uint32_t rx_decimal(rx_parser *p) {
    uint64_t v = 0;
    while (rx_is_digit(rx_at(p, p->i))) {
        v = v * 10u + (rx_at(p, p->i) - '0');
        if (v > COUNT_MAX) v = COUNT_MAX;
        p->i++;
    }
    return (uint32_t)v;
}

/* ------------------------------------------------------------------------
 * Classes
 */

static bool rx_add_range(rx_parser *p, uint32_t low, uint32_t high) {
    uint32_t pair[2] = { low, high };
    return rx_add(p, &p->work, pair, sizeof pair);
}

static uint32_t rx_char_limit(const rx_parser *p) {
    return p->unicode ? 0x10FFFFu : 0xFFFFu;
}

/* The ranges of \d, \s or \w (or, upper case, their complements) into the
 * class being read. With i and u, \w also holds U+017F and U+212A, whose
 * case folds are s and k (ECMA-262, WordCharacters). */
static void rx_class_escape(rx_parser *p, uint32_t c) {
    static const uint32_t digits[] = { '0', '9' };
    static const uint32_t spaces[] = { 0x09, 0x0D, 0x20, 0x20, 0xA0, 0xA0, 0x1680, 0x1680, 0x2000, 0x200A,
                                       0x2028, 0x2029, 0x202F, 0x202F, 0x205F, 0x205F, 0x3000, 0x3000, 0xFEFF, 0xFEFF };
    static const uint32_t words[] = { '0', '9', 'A', 'Z', '_', '_', 'a', 'z' };
    static const uint32_t words_folded[] = { '0', '9', 'A', 'Z', '_', '_', 'a', 'z', 0x17F, 0x17F, 0x212A, 0x212A };
    const uint32_t *set;
    uint32_t pairs;
    uint32_t lower = c | 0x20u;
    if (lower == 'd') {
        set = digits;
        pairs = 1;
    } else if (lower == 's') {
        set = spaces;
        pairs = 10;
    } else if (p->fold && p->unicode) {
        set = words_folded;
        pairs = 6;
    } else {
        set = words;
        pairs = 4;
    }
    if (c == lower) {
        for (uint32_t k = 0; k < pairs; k++) rx_add_range(p, set[2u * k], set[2u * k + 1u]);
        return;
    }
    uint32_t from = 0;
    for (uint32_t k = 0; k < pairs; k++) {
        if (set[2u * k] > from) rx_add_range(p, from, set[2u * k] - 1u);
        from = set[2u * k + 1u] + 1u;
    }
    if (from <= rx_char_limit(p)) rx_add_range(p, from, rx_char_limit(p));
}

/* Sort the class's ranges by their low ends: a heap sort, in place. */
static void rx_sort_ranges(uint32_t *r, uint32_t count) {
    for (uint32_t n = count; n > 1u;) {
        /* Heapify from the last parent down, then move the root out. */
        if (n == count) {
            for (uint32_t start = n / 2u; start-- > 0;) {
                uint32_t root = start;
                for (;;) {
                    uint32_t child = 2u * root + 1u;
                    if (child >= n) break;
                    if (child + 1u < n && r[2u * (child + 1u)] > r[2u * child]) child++;
                    if (r[2u * child] <= r[2u * root]) break;
                    uint32_t a = r[2u * root], b = r[2u * root + 1u];
                    r[2u * root] = r[2u * child];
                    r[2u * root + 1u] = r[2u * child + 1u];
                    r[2u * child] = a;
                    r[2u * child + 1u] = b;
                    root = child;
                }
            }
        }
        n--;
        uint32_t a = r[0], b = r[1];
        r[0] = r[2u * n];
        r[1] = r[2u * n + 1u];
        r[2u * n] = a;
        r[2u * n + 1u] = b;
        uint32_t root = 0;
        for (;;) {
            uint32_t child = 2u * root + 1u;
            if (child >= n) break;
            if (child + 1u < n && r[2u * (child + 1u)] > r[2u * child]) child++;
            if (r[2u * child] <= r[2u * root]) break;
            uint32_t x = r[2u * root], y = r[2u * root + 1u];
            r[2u * root] = r[2u * child];
            r[2u * root + 1u] = r[2u * child + 1u];
            r[2u * child] = x;
            r[2u * child + 1u] = y;
            root = child;
        }
    }
}

/* With i, the other case of each ASCII letter the class holds, and with u
 * also U+017F for s and U+212A for k (case folding maps them there). The
 * class's characters are ASCII, but for its class escapes, whose sets
 * already hold every character that folds as one of theirs. */
static bool rx_in_range(uint32_t c, uint32_t low, uint32_t high) {
    return low <= c && c <= high;
}

static void rx_fold_class(rx_parser *p) {
    uint32_t count = p->work.length / 8u;
    for (uint32_t k = 0; k < count && !p->nomem; k++) {
        uint32_t low = SJS_BUF_AT(p->work, uint32_t, 2u * k);
        uint32_t high = SJS_BUF_AT(p->work, uint32_t, 2u * k + 1u);
        static const uint32_t letters[2][2] = { { 'a', 'z' }, { 'A', 'Z' } };
        for (uint32_t r = 0; r < 2u; r++) {
            uint32_t from = low > letters[r][0] ? low : letters[r][0];
            uint32_t to = high < letters[r][1] ? high : letters[r][1];
            if (from <= to) rx_add_range(p, from ^ 0x20u, to ^ 0x20u);
        }
        if (!p->unicode) continue;
        if (rx_in_range('s', low, high) || rx_in_range('S', low, high)) rx_add_range(p, 0x17Fu, 0x17Fu);
        if (rx_in_range('k', low, high) || rx_in_range('K', low, high)) rx_add_range(p, 0x212Au, 0x212Au);
    }
}

/* The class read into p->work, sorted, merged, complemented when negated,
 * kept: its index. */
static uint32_t rx_keep_class(rx_parser *p, bool negated) {
    if (p->fold) rx_fold_class(p);
    if (p->nomem) return NONE;
    uint32_t *r = (uint32_t *)(void *)p->work.ptr;
    uint32_t count = p->work.length / 8u;
    rx_sort_ranges(r, count);
    uint32_t merged = 0;
    for (uint32_t k = 0; k < count; k++) {
        if (merged > 0 && r[2u * k] <= r[2u * (merged - 1u) + 1u] + 1u) {
            if (r[2u * k + 1u] > r[2u * (merged - 1u) + 1u]) r[2u * (merged - 1u) + 1u] = r[2u * k + 1u];
            continue;
        }
        r[2u * merged] = r[2u * k];
        r[2u * merged + 1u] = r[2u * k + 1u];
        merged++;
    }
    rx_class record = { p->ranges.length / 8u, 0 };
    if (negated) {
        uint32_t from = 0;
        for (uint32_t k = 0; k < merged; k++) {
            if (r[2u * k] > from) {
                uint32_t pair[2] = { from, r[2u * k] - 1u };
                if (!rx_add(p, &p->ranges, pair, sizeof pair)) return NONE;
                record.count++;
            }
            from = r[2u * k + 1u] + 1u;
        }
        if (from <= rx_char_limit(p)) {
            uint32_t pair[2] = { from, rx_char_limit(p) };
            if (!rx_add(p, &p->ranges, pair, sizeof pair)) return NONE;
            record.count++;
        }
    } else {
        if (!rx_add(p, &p->ranges, r, merged * 8u)) return NONE;
        record.count = merged;
    }
    uint32_t index = SJS_BUF_COUNT(p->classes, rx_class);
    if (!rx_add(p, &p->classes, &record, sizeof record)) return NONE;
    return index;
}

/* One ClassAtom, without v: a character (`*c`), or a class escape whose
 * ranges are added (false). */
static bool rx_class_atom(rx_parser *p, uint32_t *c) {
    uint32_t x = rx_at(p, p->i);
    if (x != '\\') {
        p->i++;
        *c = x;
        return true;
    }
    p->i++;
    uint32_t e = rx_at(p, p->i);
    switch (e) {
    case NONE:
        rx_syntax(p, "\\ at end of pattern");
        return false;
    case 'b':
        p->i++;
        *c = 0x08u;
        return true;
    case '-':
        p->i++;
        *c = '-';
        if (!p->unicode) rx_unsupported(p, "staticjs leaves out identity escapes of characters that are not syntax (Annex B)");
        return true;
    case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
        p->i++;
        rx_class_escape(p, e);
        return false;
    case 'p': case 'P':
        if (p->unicode) {
            rx_property_escape(p);
            return false;
        }
        rx_unsupported(p, "staticjs leaves out identity escapes of characters that are not syntax (Annex B)");
        p->i++;
        *c = e;
        return true;
    case 'c':
        if (rx_is_ascii_letter(rx_at(p, p->i + 1u))) {
            *c = rx_at(p, p->i + 1u) % 32u;
            p->i += 2u;
            return true;
        }
        if (p->unicode) {
            rx_syntax(p, "Invalid class escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out \\c without a letter (Annex B)");
        if (rx_is_digit(rx_at(p, p->i + 1u)) || rx_at(p, p->i + 1u) == '_') {
            *c = rx_at(p, p->i + 1u) % 32u;
            p->i += 2u;
        } else {
            *c = '\\';
        }
        return true;
    case 'B':
        if (p->unicode) {
            rx_syntax(p, "Invalid escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out identity escapes of characters that are not syntax (Annex B)");
        p->i++;
        *c = e;
        return true;
    case 'k':
        if (p->unicode || p->named) {
            rx_syntax(p, "Invalid escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out \\k as an identity escape (Annex B)");
        p->i++;
        *c = e;
        return true;
    default:
        break;
    }
    if (rx_is_digit(e)) {
        if (e == '0' && !rx_is_digit(rx_at(p, p->i + 1u))) {
            p->i++;
            *c = 0;
            return true;
        }
        if (p->unicode) {
            rx_syntax(p, "Invalid class escape");
            return false;
        }
        rx_unsupported(p, "staticjs leaves out legacy octal escapes (Annex B)");
        rx_decimal(p);
        *c = 0;
        return true;
    }
    return rx_character_escape(p, c);
}

/* A class without v, from `[`. */
static void rx_parse_class(rx_parser *p) {
    p->i++;
    bool negated = rx_at(p, p->i) == '^';
    if (negated) p->i++;
    p->work.length = 0;
    for (;;) {
        if (rx_stopped(p)) return;
        uint32_t x = rx_at(p, p->i);
        if (x == NONE) {
            rx_syntax(p, "Unterminated character class");
            return;
        }
        if (x == ']') {
            p->i++;
            break;
        }
        uint32_t a = 0;
        bool single = rx_class_atom(p, &a);
        if (rx_stopped(p)) return;
        if (rx_at(p, p->i) == '-' && rx_at(p, p->i + 1u) != ']' && rx_at(p, p->i + 1u) != NONE) {
            p->i++;
            uint32_t b = 0;
            bool single_b = rx_class_atom(p, &b);
            if (rx_stopped(p)) return;
            if (!single || !single_b) {
                if (p->unicode) {
                    rx_syntax(p, "Invalid character class");
                    return;
                }
                rx_unsupported(p, "staticjs leaves out a range with a class escape at an end (Annex B)");
                if (single) rx_add_range(p, a, a);
                if (single_b) rx_add_range(p, b, b);
                rx_add_range(p, '-', '-');
                continue;
            }
            if (a > b) {
                rx_syntax(p, "Range out of order in character class");
                return;
            }
            rx_folded_char(p, b);
            rx_add_range(p, a, b);
            continue;
        }
        if (single) {
            rx_folded_char(p, a);
            rx_add_range(p, a, a);
        }
    }
    uint32_t index = rx_keep_class(p, negated);
    if (index != NONE) rx_add_term(p, rx_new_node(p, N_CLASS, index), true);
}

/* ------------------------------------------------------------------------
 * Classes with v: read for their syntax alone, as v is left out. Nested
 * classes are frames of an explicit stack.
 */

/* A frame: its operator so far, and whether it may contain strings. */
enum { V_NONE = 0, V_UNION, V_INTERSECTION, V_SUBTRACTION };

typedef struct {
    uint8_t negated;
    uint8_t op;
    uint8_t operands;     /* 0, 1, or 2 for more */
    uint8_t need_operand; /* after && or -- */
    uint8_t strings;      /* MayContainStrings so far */
    uint8_t first_strings;
} v_frame;

/* Before an operand of the frame's class: an intersection or a subtraction
 * needs its operator first; with none, a second operand makes a union. */
static bool rx_v_before_operand(rx_parser *p, v_frame *f) {
    if ((f->op == V_INTERSECTION || f->op == V_SUBTRACTION) && !f->need_operand) {
        rx_syntax(p, "Invalid set operation in character class");
        return false;
    }
    if (f->op == V_NONE && f->operands > 0) f->op = V_UNION;
    return true;
}

static void rx_v_operand(v_frame *f, bool strings) {
    if (f->operands == 0) {
        f->first_strings = strings;
        f->strings = strings;
    } else if (f->op == V_INTERSECTION) {
        f->strings = f->strings && strings;
    } else {
        f->strings = f->strings || strings;
    }
    if (f->operands < 2u) f->operands++;
    f->need_operand = 0;
}

static bool rx_reserved_double(const rx_parser *p, uint32_t i) {
    static const char doubles[] = "&!#$%*+,.:;<=>?@^`~";
    uint32_t c = rx_at(p, i);
    if (c == NONE || rx_at(p, i + 1u) != c) return false;
    for (const char *d = doubles; *d != '\0'; d++) {
        if ((uint32_t)(uint8_t)*d == c) return true;
    }
    return false;
}

static bool rx_is_set_syntax(uint32_t c) {
    switch (c) {
    case '(': case ')': case '[': case ']': case '{': case '}': case '/': case '-': case '\\': case '|':
        return true;
    default:
        return false;
    }
}

static bool rx_is_set_punctuator(uint32_t c) {
    switch (c) {
    case '&': case '-': case '!': case '#': case '%': case ',': case ':': case ';': case '<': case '=':
    case '>': case '@': case '`': case '~':
        return true;
    default:
        return false;
    }
}

/* A ClassSetCharacter at p->i: its value, read; false, with nothing read,
 * when none is there; a syntax error stops. */
static bool rx_set_character(rx_parser *p, uint32_t *value) {
    uint32_t c = rx_at(p, p->i);
    if (c == NONE) return false;
    if (c != '\\') {
        if (rx_is_set_syntax(c) || rx_reserved_double(p, p->i)) return false;
        p->i++;
        *value = c;
        return true;
    }
    uint32_t e = rx_at(p, p->i + 1u);
    if (e == 'b') {
        p->i += 2u;
        *value = 0x08u;
        return true;
    }
    if (rx_is_set_punctuator(e)) {
        p->i += 2u;
        *value = e;
        return true;
    }
    if (e == 'c') {
        if (!rx_is_ascii_letter(rx_at(p, p->i + 2u))) {
            rx_syntax(p, "Invalid class escape");
            return false;
        }
        *value = rx_at(p, p->i + 2u) % 32u;
        p->i += 3u;
        return true;
    }
    if (e == '0' && !rx_is_digit(rx_at(p, p->i + 2u))) {
        p->i += 2u;
        *value = 0;
        return true;
    }
    if (rx_is_digit(e) || e == 'k' || e == 'B' || e == 'd' || e == 'D' || e == 's' || e == 'S' || e == 'w' || e == 'W' ||
        e == 'p' || e == 'P' || e == 'q') {
        return false;
    }
    p->i++;
    return rx_character_escape(p, value);
}

/* \q{...}: may it hold strings (a string of another length than 1)? */
static bool rx_string_disjunction(rx_parser *p) {
    p->i += 3u; /* \q{ */
    bool strings = false;
    uint32_t length = 0;
    for (;;) {
        if (rx_stopped(p)) return false;
        uint32_t c = rx_at(p, p->i);
        if (c == NONE) {
            rx_syntax(p, "Unterminated character class");
            return false;
        }
        if (c == '}' || c == '|') {
            if (length != 1u) strings = true;
            length = 0;
            p->i++;
            if (c == '}') return strings;
            continue;
        }
        uint32_t v;
        if (!rx_set_character(p, &v)) {
            rx_syntax(p, "Invalid class string disjunction");
            return false;
        }
        /* A pair of halves is one character. */
        length++;
    }
}

static void rx_parse_class_v(rx_parser *p) {
    sjs_buf frames = { 0 };
    p->i++;
    v_frame f = { 0 };
    f.negated = rx_at(p, p->i) == '^';
    if (f.negated) p->i++;
    if (!rx_add(p, &frames, &f, sizeof f)) return;
    while (!rx_stopped(p)) {
        v_frame *top = &SJS_BUF_AT(frames, v_frame, SJS_BUF_COUNT(frames, v_frame) - 1u);
        uint32_t c = rx_at(p, p->i);
        if (c == NONE) {
            rx_syntax(p, "Unterminated character class");
            break;
        }
        uint32_t d = rx_at(p, p->i + 1u);
        if (c == ']') {
            p->i++;
            if (top->need_operand) {
                rx_syntax(p, "Invalid set operation in character class");
                break;
            }
            bool strings = top->op == V_SUBTRACTION ? top->first_strings : top->strings;
            if (top->negated && strings) {
                rx_syntax(p, "Negated character class may contain strings");
                break;
            }
            frames.length -= (uint32_t)sizeof(v_frame);
            if (frames.length == 0) break;
            /* The nested class is an operand of the class around it. */
            rx_v_operand(&SJS_BUF_AT(frames, v_frame, SJS_BUF_COUNT(frames, v_frame) - 1u), strings);
            continue;
        }
        if ((c == '&' && d == '&') || (c == '-' && d == '-')) {
            uint8_t op = c == '&' ? V_INTERSECTION : V_SUBTRACTION;
            if (top->operands == 0 || top->need_operand || (top->op != V_NONE && top->op != op)) {
                rx_syntax(p, "Invalid set operation in character class");
                break;
            }
            top->op = op;
            top->need_operand = 1;
            p->i += 2u;
            if (op == V_INTERSECTION && rx_at(p, p->i) == '&') {
                rx_syntax(p, "Invalid character in character class");
                break;
            }
            continue;
        }
        if (!rx_v_before_operand(p, top)) break;
        if (c == '[') {
            if (SJS_BUF_COUNT(frames, v_frame) >= p->limits.max_nesting) {
                p->outcome = SJS_REGEX_LIMIT;
                p->message = "the program nests deeper than max_nesting";
                break;
            }
            p->i++;
            v_frame nested = { 0 };
            nested.negated = rx_at(p, p->i) == '^';
            if (nested.negated) p->i++;
            if (!rx_add(p, &frames, &nested, sizeof nested)) break;
            continue;
        }
        if (c == '\\' && (d == 'd' || d == 'D' || d == 's' || d == 'S' || d == 'w' || d == 'W')) {
            p->i += 2u;
            rx_v_operand(top, false);
            continue;
        }
        if (c == '\\' && (d == 'p' || d == 'P')) {
            p->i++;
            uint8_t kind = rx_property_escape(p);
            rx_v_operand(top, kind == P_STRINGS);
            continue;
        }
        if (c == '\\' && d == 'q' && rx_at(p, p->i + 2u) == '{') {
            bool strings = rx_string_disjunction(p);
            rx_v_operand(top, strings);
            continue;
        }
        uint32_t a;
        if (!rx_set_character(p, &a)) {
            rx_syntax(p, "Invalid character in character class");
            break;
        }
        if (rx_at(p, p->i) == '-' && rx_at(p, p->i + 1u) != '-') {
            /* A range, which only a union holds. */
            if (top->op == V_INTERSECTION || top->op == V_SUBTRACTION) {
                rx_syntax(p, "Invalid set operation in character class");
                break;
            }
            p->i++;
            uint32_t b;
            if (!rx_set_character(p, &b)) {
                rx_syntax(p, "Invalid character class");
                break;
            }
            if (a > b) {
                rx_syntax(p, "Range out of order in character class");
                break;
            }
            top->op = V_UNION;
        }
        rx_v_operand(top, false);
    }
    sjs_buf_free(p->allocator, &frames);
    if (!rx_stopped(p)) rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
}

/* ------------------------------------------------------------------------
 * Atoms, groups and quantifiers
 */

static void rx_parse_escape(rx_parser *p) {
    p->i++;
    uint32_t e = rx_at(p, p->i);
    switch (e) {
    case NONE:
        rx_syntax(p, "\\ at end of pattern");
        return;
    case 'b':
    case 'B':
        p->i++;
        rx_add_term(p, rx_new_node(p, N_ASSERT, e == 'b' ? RX_ASSERT_WORD : RX_ASSERT_NOT_WORD), false);
        return;
    case 'd': case 'D': case 's': case 'S': case 'w': case 'W': {
        p->i++;
        p->work.length = 0;
        rx_class_escape(p, e);
        uint32_t index = rx_keep_class(p, false);
        if (index != NONE) rx_add_term(p, rx_new_node(p, N_CLASS, index), true);
        return;
    }
    case 'p':
    case 'P':
        if (p->unicode) {
            rx_property_escape(p);
            rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
            return;
        }
        break;
    case 'k':
        rx_named_reference(p);
        rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
        return;
    case 'c':
        if (rx_is_ascii_letter(rx_at(p, p->i + 1u))) {
            uint32_t v = rx_at(p, p->i + 1u) % 32u;
            p->i += 2u;
            rx_add_char(p, v);
            return;
        }
        if (p->unicode) {
            rx_syntax(p, "Invalid Unicode escape");
            return;
        }
        /* The backslash is a character of its own, and c the next. */
        rx_unsupported(p, "staticjs leaves out \\c without a letter (Annex B)");
        rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
        return;
    default:
        break;
    }
    if (rx_is_digit(e)) {
        if (e == '0' && !rx_is_digit(rx_at(p, p->i + 1u))) {
            p->i++;
            rx_add_char(p, 0);
            return;
        }
        if (e == '0') {
            if (p->unicode) {
                rx_syntax(p, "Invalid decimal escape");
                return;
            }
            rx_unsupported(p, "staticjs leaves out legacy octal escapes (Annex B)");
            rx_decimal(p);
            rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
            return;
        }
        uint32_t number = rx_decimal(p);
        if (p->unicode && number > p->groups) {
            rx_syntax(p, "Invalid escape");
            return;
        }
        rx_unsupported(p, number <= p->groups ? "staticjs has no backreferences: a linear-time automaton cannot match them"
                                           : "staticjs leaves out legacy octal escapes (Annex B)");
        rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
        return;
    }
    if (e == 'p' || e == 'P') {
        rx_unsupported(p, "staticjs leaves out identity escapes of characters that are not syntax (Annex B)");
        p->i++;
        rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
        return;
    }
    uint32_t v;
    if (rx_character_escape(p, &v)) rx_add_char(p, v);
}

/* A group's name, after `(?<`: the group is numbered `number`. */
static void rx_read_group_name(rx_parser *p, uint32_t number) {
    uint32_t start = p->i;
    uint32_t j = start;
    bool exotic = false;
    bool valid = true;
    while (j < p->n && p->pat[j] != '>') {
        uint32_t c = p->pat[j];
        if (c == '\\' || c >= 0x80u) {
            exotic = true;
        } else if (!(rx_is_ascii_letter(c) || c == '$' || c == '_' || (j > start && rx_is_digit(c)))) {
            valid = false;
        }
        j++;
    }
    if (j >= p->n || j == start || (!exotic && !valid)) {
        rx_syntax(p, "Invalid capture group name");
        return;
    }
    p->i = j + 1u;
    if (exotic) {
        rx_unsupported(p, "staticjs reads group names of ASCII letters, digits, $ and _ only");
        return;
    }
    if (p->exotic_names) return;
    uint32_t first = rx_group_named(p, start, j - start);
    if (first != 0 && first != number) rx_syntax(p, "Duplicate capture group name");
}

static void rx_open_group(rx_parser *p) {
    p->i++;
    uint8_t kind = G_CAPTURE;
    if (rx_at(p, p->i) == '?') {
        uint32_t c = rx_at(p, p->i + 1u);
        if (c == ':') {
            kind = G_PLAIN;
            p->i += 2u;
        } else if (c == '=' || c == '!') {
            kind = G_LOOKAHEAD;
            p->i += 2u;
            rx_unsupported(p, "staticjs has no lookahead yet");
        } else if (c == '<' && (rx_at(p, p->i + 2u) == '=' || rx_at(p, p->i + 2u) == '!')) {
            kind = G_LOOKBEHIND;
            p->i += 3u;
            rx_unsupported(p, "staticjs has no lookbehind yet");
        } else if (c == '<') {
            p->i += 2u;
            rx_read_group_name(p, p->captured + 1u);
        } else {
            rx_syntax(p, "Invalid group");
            return;
        }
    }
    if (rx_stopped(p)) return;
    if (SJS_BUF_COUNT(p->opens, rx_open) >= p->limits.max_nesting) {
        p->outcome = SJS_REGEX_LIMIT;
        p->message = "the program nests deeper than max_nesting";
        return;
    }
    rx_open o = { 0 };
    o.kind = kind;
    o.group = rx_new_node(p, N_GROUP, 0);
    o.alt = rx_new_node(p, N_ALT, 0);
    o.concat = rx_new_node(p, N_CONCAT, 0);
    if (p->nomem) return;
    if (kind == G_CAPTURE) {
        rx_node_at(p, o.group)->capture = 1;
        rx_node_at(p, o.group)->value = ++p->captured;
    }
    rx_append_child(p, o.group, o.alt);
    rx_append_child(p, o.alt, o.concat);
    rx_add(p, &p->opens, &o, sizeof o);
    p->atom = NONE;
}

static void rx_close_group(rx_parser *p) {
    if (SJS_BUF_COUNT(p->opens, rx_open) <= 1u) {
        rx_syntax(p, "Unmatched ')'");
        return;
    }
    p->i++;
    rx_open o = *rx_top_open(p);
    p->opens.length -= (uint32_t)sizeof(rx_open);
    bool quantifiable = o.kind == G_PLAIN || o.kind == G_CAPTURE || (o.kind == G_LOOKAHEAD && !p->unicode);
    rx_add_term(p, o.group, quantifiable);
}

/* A braced quantifier at p->i, `{n}`, `{n,}` or `{n,m}`: read, or false
 * with nothing read when the text is not one. */
static bool rx_braced(rx_parser *p, uint32_t *min, uint32_t *max) {
    uint32_t save = p->i;
    p->i++;
    if (!rx_is_digit(rx_at(p, p->i))) {
        p->i = save;
        return false;
    }
    *min = rx_decimal(p);
    *max = *min;
    if (rx_at(p, p->i) == ',') {
        p->i++;
        *max = rx_is_digit(rx_at(p, p->i)) ? rx_decimal(p) : RX_INFINITE;
    }
    if (rx_at(p, p->i) != '}') {
        p->i = save;
        return false;
    }
    p->i++;
    return true;
}

static void rx_quantify(rx_parser *p, uint32_t min, uint32_t max) {
    bool greedy = true;
    if (rx_at(p, p->i) == '?') {
        greedy = false;
        p->i++;
    }
    if (p->atom == NONE) {
        rx_syntax(p, "Nothing to repeat");
        return;
    }
    if (max != RX_INFINITE && min > max) {
        rx_syntax(p, "numbers out of order in {} quantifier");
        return;
    }
    /* The atom moves to a new node; its place, in its parent's list,
     * becomes the repetition. */
    rx_node atom = *rx_node_at(p, p->atom);
    uint32_t moved = SJS_BUF_COUNT(p->nodes, rx_node);
    atom.next = NONE;
    if (!rx_add(p, &p->nodes, &atom, sizeof atom)) return;
    rx_node *r = rx_node_at(p, p->atom);
    r->type = N_REPEAT;
    r->greedy = greedy ? 1u : 0u;
    r->capture = 0;
    r->value = 0;
    r->min = min;
    r->max = max;
    r->first = moved;
    r->last = moved;
    p->atom = NONE;
}

/* The pattern, into p->nodes: node 0 is the whole pattern's group. */
static void rx_parse(rx_parser *p) {
    rx_open root = { 0 };
    root.kind = G_ROOT;
    root.group = rx_new_node(p, N_GROUP, 0);
    root.alt = rx_new_node(p, N_ALT, 0);
    root.concat = rx_new_node(p, N_CONCAT, 0);
    if (p->nomem) return;
    rx_append_child(p, root.group, root.alt);
    rx_append_child(p, root.alt, root.concat);
    if (!rx_add(p, &p->opens, &root, sizeof root)) return;
    p->atom = NONE;
    while (p->i < p->n && !rx_stopped(p)) {
        uint32_t c = p->pat[p->i];
        switch (c) {
        case '|': {
            p->i++;
            rx_open *o = rx_top_open(p);
            uint32_t concat = rx_new_node(p, N_CONCAT, 0);
            if (concat == NONE) return;
            o = rx_top_open(p);
            o->concat = concat;
            rx_append_child(p, o->alt, concat);
            p->atom = NONE;
            break;
        }
        case '(':
            rx_open_group(p);
            break;
        case ')':
            rx_close_group(p);
            break;
        case '^':
        case '$': {
            p->i++;
            bool multiline = (p->flags & SJS_REGEX_MULTILINE) != 0;
            uint32_t kind = c == '^' ? (multiline ? RX_ASSERT_LINE_START : RX_ASSERT_START)
                                     : (multiline ? RX_ASSERT_LINE_END : RX_ASSERT_END);
            rx_add_term(p, rx_new_node(p, N_ASSERT, kind), false);
            break;
        }
        case '.':
            p->i++;
            rx_add_term(p, rx_new_node(p, N_ANY, 0), true);
            break;
        case '[':
            if (p->sets) rx_parse_class_v(p);
            else rx_parse_class(p);
            break;
        case '\\':
            rx_parse_escape(p);
            break;
        case '*':
            p->i++;
            rx_quantify(p, 0, RX_INFINITE);
            break;
        case '+':
            p->i++;
            rx_quantify(p, 1, RX_INFINITE);
            break;
        case '?':
            p->i++;
            rx_quantify(p, 0, 1);
            break;
        case '{': {
            uint32_t min, max;
            if (rx_braced(p, &min, &max)) {
                rx_quantify(p, min, max);
                break;
            }
            if (p->unicode) {
                rx_syntax(p, p->atom != NONE ? "Incomplete quantifier" : "Lone quantifier brackets");
                break;
            }
            rx_unsupported(p, "staticjs leaves out a { that begins no quantifier (Annex B)");
            p->i++;
            rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
            break;
        }
        case '}':
        case ']':
            if (p->unicode) {
                rx_syntax(p, "Lone quantifier brackets");
                break;
            }
            rx_unsupported(p, "staticjs leaves out a lone ] or } (Annex B)");
            p->i++;
            rx_add_term(p, rx_new_node(p, N_EMPTY, 0), true);
            break;
        default:
            p->i++;
            rx_add_char(p, c);
            break;
        }
    }
    if (!rx_stopped(p) && SJS_BUF_COUNT(p->opens, rx_open) > 1u) rx_syntax(p, "Unterminated group");
}

/* The pattern's characters: code points with u or v, else UTF-16 code
 * units. */
static void rx_decode(rx_parser *p, sjs_str body) {
    uint32_t i = 0;
    while (i < body.length) {
        int32_t point = sjs_utf8_next(body, &i);
        if (point < 0) point = 0xFFFD;
        uint32_t c = (uint32_t)point;
        if (!p->unicode && c >= 0x10000u) {
            uint32_t pair[2] = { 0xD800u + ((c - 0x10000u) >> 10), 0xDC00u + ((c - 0x10000u) & 0x3FFu) };
            if (!rx_add(p, &p->chars, pair, sizeof pair)) return;
            continue;
        }
        if (!rx_add(p, &p->chars, &c, sizeof c)) return;
    }
    p->pat = (const uint32_t *)(const void *)p->chars.ptr;
    p->n = SJS_BUF_COUNT(p->chars, uint32_t);
}

static void rx_parser_init(rx_parser *p, sjs_regex_env env, sjs_regex_literal literal) {
    sjs_zero(p, sizeof *p);
    p->allocator = env.allocator;
    p->limits = env.limits;
    p->literal = literal;
    p->flags = literal.flags;
    p->sets = (literal.flags & SJS_REGEX_UNICODE_SETS) != 0;
    p->unicode = p->sets || (literal.flags & SJS_REGEX_UNICODE) != 0;
    p->fold = (literal.flags & SJS_REGEX_IGNORE_CASE) != 0;
    rx_decode(p, literal.body);
    if (p->sets) rx_unsupported(p, "staticjs leaves out the v flag: its sets need Unicode tables");
    if (!p->nomem) rx_prescan(p);
    if (!p->nomem) rx_parse(p);
}

static void rx_parser_free(rx_parser *p) {
    sjs_buf_free(p->allocator, &p->chars);
    sjs_buf_free(p->allocator, &p->names);
    sjs_buf_free(p->allocator, &p->nodes);
    sjs_buf_free(p->allocator, &p->opens);
    sjs_buf_free(p->allocator, &p->work);
    sjs_buf_free(p->allocator, &p->ranges);
    sjs_buf_free(p->allocator, &p->classes);
}

sjs_result sjs_regex_check(sjs_regex_env env, sjs_regex_literal literal, sjs_regex_verdict *out) {
    rx_parser p;
    rx_parser_init(&p, env, literal);
    bool nomem = p.nomem;
    out->outcome = p.outcome;
    out->message = p.message;
    rx_parser_free(&p);
    return nomem ? SJS_ERR_NOMEM : SJS_OK;
}

/* ------------------------------------------------------------------------
 * Compiling. A first walk measures each node: its instructions, whether it
 * can match empty, the groups in it and its loop registers. That says,
 * before anything is expanded, how many threads and slots the automaton
 * needs (docs/regex.md, 9.3). A second walk emits the instructions.
 */

/* Sizes saturate here: far past any limit, and far from overflowing. */
#define SIZE_CAP ((uint64_t)1 << 40)

static uint64_t rx_add_sat(uint64_t a, uint64_t b) {
    return a + b > SIZE_CAP ? SIZE_CAP : a + b;
}

static uint64_t rx_mul_sat(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0) return 0;
    return a > SIZE_CAP / b ? SIZE_CAP : a * b;
}

typedef struct {
    uint64_t size;        /* instructions */
    uint64_t clears;      /* slots its CLEAR instructions clear, together */
    uint32_t registers;   /* nesting of loops that can match empty, with an optional iteration */
    uint32_t first_group; /* the groups in it; first > last for none */
    uint32_t last_group;
    bool nullable;        /* it can match the empty string */
} rx_measure;

/* A node being walked: the child to visit next. */
typedef struct {
    uint32_t node;
    uint32_t cursor;
} rx_walk;

static bool rx_has_groups(const rx_measure *m) {
    return m->first_group <= m->last_group;
}

static void rx_measure_node(rx_parser *p, rx_measure *ms, uint32_t index) {
    const rx_node *n = rx_node_at(p, index);
    rx_measure m = { 0, 0, 0, UINT32_MAX, 0, false };
    switch (n->type) {
    case N_EMPTY:
        m.nullable = true;
        break;
    case N_CHAR:
    case N_ANY:
    case N_CLASS:
        m.size = 1;
        break;
    case N_ASSERT:
        m.size = 1;
        m.nullable = true;
        break;
    case N_GROUP:
        m = ms[n->first];
        if (n->capture) {
            /* The groups inside a group come after it. */
            m.size = rx_add_sat(m.size, 2);
            if (!rx_has_groups(&m)) m.last_group = n->value;
            m.first_group = n->value;
        }
        break;
    case N_CONCAT:
    case N_ALT: {
        bool alt = n->type == N_ALT;
        m.nullable = !alt;
        uint32_t k = 0;
        for (uint32_t c = n->first; c != NONE; c = rx_node_at(p, c)->next, k++) {
            const rx_measure *x = &ms[c];
            m.size = rx_add_sat(m.size, x->size);
            m.clears = rx_add_sat(m.clears, x->clears);
            if (x->registers > m.registers) m.registers = x->registers;
            if (rx_has_groups(x)) {
                if (x->first_group < m.first_group) m.first_group = x->first_group;
                if (!rx_has_groups(&m) || x->last_group > m.last_group) m.last_group = x->last_group;
            }
            m.nullable = alt ? (m.nullable || x->nullable) : (m.nullable && x->nullable);
        }
        if (alt && k > 1u) m.size = rx_add_sat(m.size, 2u * (uint64_t)(k - 1u));
        break;
    }
    default: { /* N_REPEAT */
        const rx_measure *c = &ms[n->first];
        if (c->size == 0) {
            /* Emitted as nothing (emit_step). */
            m.nullable = true;
            break;
        }
        uint64_t clear = rx_has_groups(c) ? 1u : 0u;
        uint64_t width = rx_has_groups(c) ? 2u * (uint64_t)(c->last_group - c->first_group + 1u) : 0u;
        bool optional = n->max == RX_INFINITE || n->max > n->min;
        uint64_t reg = optional && c->nullable ? 1u : 0u;
        uint64_t copy = rx_add_sat(c->size, clear);
        m.size = rx_mul_sat(n->min, copy);
        uint64_t copies = n->min;
        if (n->max == RX_INFINITE) {
            m.size = rx_add_sat(m.size, rx_add_sat(copy, 2u * reg + 2u));
            copies = rx_add_sat(copies, 1);
        } else if (optional) {
            m.size = rx_add_sat(m.size, rx_mul_sat(n->max - n->min, rx_add_sat(copy, 2u * reg + 1u)));
            copies = rx_add_sat(copies, n->max - n->min);
        }
        m.clears = rx_mul_sat(copies, rx_add_sat(c->clears, width));
        m.registers = c->registers + (uint32_t)reg;
        m.first_group = c->first_group;
        m.last_group = c->last_group;
        m.nullable = n->min == 0 || c->nullable;
        break;
    }
    }
    ms[index] = m;
}

/* Measure every node, children before their parents. */
static bool rx_measure_tree(rx_parser *p, rx_measure *ms) {
    sjs_buf stack = { 0 };
    rx_walk w = { 0, rx_node_at(p, 0)->first };
    if (!rx_add(p, &stack, &w, sizeof w)) return false;
    while (stack.length > 0) {
        rx_walk *top = &SJS_BUF_AT(stack, rx_walk, SJS_BUF_COUNT(stack, rx_walk) - 1u);
        if (top->cursor == NONE) {
            rx_measure_node(p, ms, top->node);
            stack.length -= (uint32_t)sizeof(rx_walk);
            continue;
        }
        uint32_t child = top->cursor;
        top->cursor = rx_node_at(p, child)->next;
        rx_walk next = { child, rx_node_at(p, child)->first };
        if (!rx_add(p, &stack, &next, sizeof next)) break;
    }
    sjs_buf_free(p->allocator, &stack);
    return !p->nomem;
}

/* Emitting: a frame per node being emitted. */
enum { E_START = 0, E_CHILD, E_REQUIRED, E_OPTIONAL, E_AFTER_COPY };

typedef struct {
    uint32_t node;
    uint8_t phase;
    uint32_t cursor;      /* N_CONCAT, N_ALT: the child being emitted */
    uint32_t copies;      /* N_REPEAT: copies emitted */
    uint32_t patches;     /* N_ALT, N_REPEAT: where its jumps to patch begin in the patch list */
    uint32_t split;       /* N_ALT: the SPLIT before the current alternative; N_REPEAT: the loop's */
    uint32_t top;         /* N_REPEAT with no bound: its loop's first pc */
} rx_emitting;

typedef struct {
    rx_parser *p;
    const rx_measure *ms;
    sjs_buf code;         /* rx_ins */
    sjs_buf patches;      /* uint32_t: pc << 1 | 1 for its y, 0 for its x */
    sjs_buf frames;       /* rx_emitting */
    uint32_t depth;       /* loop registers in force */
    uint32_t register_base; /* the slot of register 0 */
} rx_emitter;

static uint32_t rx_here(const rx_emitter *e) {
    return SJS_BUF_COUNT(e->code, rx_ins);
}

/* An instruction, at the loop registers in force: its pc. */
static uint32_t rx_emit_ins(rx_emitter *e, rx_ins ins) {
    ins.relevant = (uint8_t)e->depth;
    uint32_t pc = rx_here(e);
    rx_add(e->p, &e->code, &ins, sizeof ins);
    return pc;
}

#define rx_emit(e, op, x, y) rx_emit_ins((e), (rx_ins){ (op), 0, 0, (x), (y) })

static void rx_patch_later(rx_emitter *e, uint32_t pc, bool y) {
    uint32_t entry = pc << 1 | (y ? 1u : 0u);
    rx_add(e->p, &e->patches, &entry, sizeof entry);
}

/* Point every jump recorded from `from` on here. */
static void rx_patch_here(rx_emitter *e, uint32_t from) {
    uint32_t target = rx_here(e);
    for (uint32_t k = from; k < SJS_BUF_COUNT(e->patches, uint32_t); k++) {
        uint32_t entry = SJS_BUF_AT(e->patches, uint32_t, k);
        rx_ins *ins = &SJS_BUF_AT(e->code, rx_ins, entry >> 1);
        if ((entry & 1u) != 0) ins->y = target;
        else ins->x = target;
    }
    e->patches.length = from * (uint32_t)sizeof(uint32_t);
}

static bool rx_push_emit(rx_emitter *e, uint32_t node) {
    rx_emitting f = { 0 };
    f.node = node;
    f.cursor = rx_node_at(e->p, node)->first;
    return rx_add(e->p, &e->frames, &f, sizeof f);
}

/* A SPLIT that tries the next instruction first (greedy) or the target
 * patched later first (lazy); the patched side is recorded. */
static uint32_t rx_emit_loop_split(rx_emitter *e, bool greedy) {
    uint32_t pc = rx_here(e);
    rx_emit(e, RX_SPLIT, greedy ? pc + 1u : 0, greedy ? 0 : pc + 1u);
    rx_patch_later(e, pc, greedy);
    return pc;
}

/* The CLEAR of a copy of a repetition whose body holds groups. */
static void rx_emit_clear(rx_emitter *e, const rx_measure *body) {
    if (rx_has_groups(body)) rx_emit(e, RX_CLEAR, 2u * body->first_group, 2u * body->last_group + 1u);
}

/* One step of the node on top. */
static void rx_emit_step(rx_emitter *e) {
    rx_emitting *f = &SJS_BUF_AT(e->frames, rx_emitting, SJS_BUF_COUNT(e->frames, rx_emitting) - 1u);
    const rx_node *n = rx_node_at(e->p, f->node);
    bool done = false;
    switch (n->type) {
    case N_CHAR: {
        uint32_t c = n->value;
        bool letter = rx_is_ascii_letter(c) && e->p->fold;
        rx_emit(e, letter ? RX_CHAR_FOLD : RX_CHAR, letter ? (c | 0x20u) : c, 0);
        done = true;
        break;
    }
    case N_ANY:
        rx_emit(e, (e->p->flags & SJS_REGEX_DOT_ALL) != 0 ? RX_ANY_ALL : RX_ANY, 0, 0);
        done = true;
        break;
    case N_CLASS:
        rx_emit(e, RX_CLASS, n->value, 0);
        done = true;
        break;
    case N_ASSERT:
        rx_emit(e, RX_ASSERT, n->value, 0);
        done = true;
        break;
    case N_GROUP:
        if (f->phase == E_START) {
            if (n->capture) rx_emit(e, RX_SAVE, 2u * n->value, 0);
            f->phase = E_CHILD;
            rx_push_emit(e, n->first);
            return;
        }
        if (n->capture) rx_emit(e, RX_SAVE, 2u * n->value + 1u, 0);
        done = true;
        break;
    case N_CONCAT:
        if (f->cursor == NONE) {
            done = true;
            break;
        }
        {
            uint32_t child = f->cursor;
            f->cursor = rx_node_at(e->p, child)->next;
            rx_push_emit(e, child);
        }
        return;
    case N_ALT:
        if (f->phase == E_START) {
            f->patches = SJS_BUF_COUNT(e->patches, uint32_t);
            f->phase = E_OPTIONAL;
        }
        if (f->phase == E_OPTIONAL) {
            /* Before an alternative: a SPLIT unless it is the last. */
            if (rx_node_at(e->p, f->cursor)->next != NONE) {
                f->split = rx_emit(e, RX_SPLIT, rx_here(e) + 1u, 0);
            }
            f->phase = E_AFTER_COPY;
            rx_push_emit(e, f->cursor);
            return;
        }
        /* After an alternative. */
        if (rx_node_at(e->p, f->cursor)->next != NONE) {
            rx_patch_later(e, rx_emit(e, RX_JMP, 0, 0), false);
            SJS_BUF_AT(e->code, rx_ins, f->split).y = rx_here(e);
            f->cursor = rx_node_at(e->p, f->cursor)->next;
            f->phase = E_OPTIONAL;
            return;
        }
        rx_patch_here(e, f->patches);
        done = true;
        break;
    default: { /* N_REPEAT */
        const rx_measure *body = &e->ms[n->first];
        bool greedy = n->greedy != 0;
        bool reg = (n->max == RX_INFINITE || n->max > n->min) && body->nullable;
        if (body->size == 0) {
            /* A body of no instructions matches empty: its required copies
             * do nothing, and an optional one would fail its check. */
            done = true;
            break;
        }
        if (f->phase == E_START) {
            f->patches = SJS_BUF_COUNT(e->patches, uint32_t);
            f->phase = E_REQUIRED;
        }
        if (f->phase == E_REQUIRED) {
            if (f->copies < n->min) {
                f->copies++;
                rx_emit_clear(e, body);
                rx_push_emit(e, n->first);
                return;
            }
            f->phase = E_OPTIONAL;
        }
        if (f->phase == E_AFTER_COPY) {
            /* An optional copy ended: its check, and for a loop the jump back. */
            if (reg) {
                rx_emit(e, RX_CHECK, e->register_base + e->depth - 1u, 0);
                e->depth--;
            }
            if (n->max == RX_INFINITE) {
                rx_emit(e, RX_JMP, f->top, 0);
                rx_patch_here(e, f->patches);
                done = true;
                break;
            }
            f->phase = E_OPTIONAL;
        }
        /* E_OPTIONAL: the next optional copy, if any. */
        if (n->max != RX_INFINITE && f->copies >= n->max) {
            rx_patch_here(e, f->patches);
            done = true;
            break;
        }
        f->copies++;
        f->top = rx_here(e);
        rx_emit_loop_split(e, greedy);
        if (reg) {
            rx_emit(e, RX_MARK, e->register_base + e->depth, 0);
            e->depth++;
        }
        rx_emit_clear(e, body);
        f->phase = E_AFTER_COPY;
        rx_push_emit(e, n->first);
        return;
    }
    }
    if (done) e->frames.length -= (uint32_t)sizeof(rx_emitting);
}

/* The value: one block, laid out as sjs_regexp says. The header is made
 * first over the emitter's buffers, for sjs_regex_dfa_classes to read, then
 * pointed at the block's own copies. */
static sjs_result rx_build_value(sjs_context *context, const rx_emitter *e, sjs_value *out) {
    const rx_parser *p = e->p;
    sjs_regex_literal literal = p->literal;
    uint32_t count = SJS_BUF_COUNT(e->code, rx_ins);
    uint32_t class_count = SJS_BUF_COUNT(p->classes, rx_class);
    uint32_t range_count = p->ranges.length / 8u;
    const rx_ins *code = (const rx_ins *)(const void *)e->code.ptr;
    sjs_regexp view;
    sjs_zero(&view, sizeof view);
    view.count = count;
    view.code = code;
    view.classes = (const rx_class *)(const void *)p->classes.ptr;
    view.ranges = (const uint32_t *)(const void *)p->ranges.ptr;
    uint32_t saves = 0;
    while (saves < count && code[saves].op == RX_SAVE) saves++;
    const rx_ins *lead = &code[saves < count ? saves : 0];
    view.lead_saves = saves;
    if (saves < count && lead->op == RX_CHAR && lead->x < 0x80u) view.lead = RX_LEAD_CHAR;
    else if (saves < count && lead->op == RX_CHAR_FOLD && lead->x < 0x80u) view.lead = RX_LEAD_FOLD;
    else if (saves < count && lead->op == RX_ASSERT && lead->x == RX_ASSERT_START) view.lead = RX_LEAD_START;
    view.lead_char = view.lead == RX_LEAD_CHAR || view.lead == RX_LEAD_FOLD ? (uint8_t)lead->x : 0;
    uint16_t map[RX_DFA_ASCII];
    sjs_regex_dfa_classes(&view, map);
    uint32_t names_length = 0;
    for (uint32_t g = 0; g < p->groups; g++) names_length += SJS_BUF_AT(p->names, rx_name, g).length;
    uint64_t text_length = (uint64_t)literal.body.length + names_length;
    uint64_t size = sizeof(sjs_regexp) + (uint64_t)count * sizeof(rx_ins) + (uint64_t)class_count * sizeof(rx_class) +
                    (uint64_t)range_count * 8u + (uint64_t)p->groups * sizeof(rx_group_name) + text_length +
                    (view.dfa_classes != 0 ? sizeof map : 0u);
    if (size > UINT32_MAX) return SJS_ERR_LIMIT;
    sjs_regexp *re = sjs_mem_alloc(&context->counting, (size_t)size);
    if (re == NULL) return SJS_ERR_NOMEM;
    *re = view;
    re->h.refs = 1;
    re->h.type = SJS_HEAP_REGEXP;
    re->h.size = (uint32_t)size;
    const rx_measure *root = &e->ms[0];
    re->flags = literal.flags;
    re->groups = p->groups;
    re->registers = root->registers;
    re->slots = 2u * (p->groups + 1u) + root->registers;
    re->threads = count << root->registers;
    re->class_count = class_count;
    re->range_count = range_count;
    re->clear_slots = 0;
    re->named = p->named;
    uint8_t *at = (uint8_t *)(re + 1);
    sjs_copy(at, code, (size_t)count * sizeof(rx_ins));
    re->code = (const rx_ins *)(const void *)at;
    for (uint32_t i = 0; i < count; i++) {
        if (code[i].op == RX_CLEAR) re->clear_slots += code[i].y - code[i].x + 1u;
    }
    at += (size_t)count * sizeof(rx_ins);
    sjs_copy(at, p->classes.ptr, (size_t)class_count * sizeof(rx_class));
    re->classes = (const rx_class *)(const void *)at;
    at += (size_t)class_count * sizeof(rx_class);
    sjs_copy(at, p->ranges.ptr, (size_t)range_count * 8u);
    re->ranges = (const uint32_t *)(const void *)at;
    at += (size_t)range_count * 8u;
    rx_group_name *names = (rx_group_name *)(void *)at;
    re->names = names;
    at += (size_t)p->groups * sizeof(rx_group_name);
    /* On a boundary of 4 bytes, as every part before it is a multiple of 4. */
    re->dfa_map = NULL;
    if (re->dfa_classes != 0) {
        sjs_copy(at, map, sizeof map);
        re->dfa_map = (const uint16_t *)(const void *)at;
        at += sizeof map;
    }
    char *text = (char *)at;
    sjs_copy(text, literal.body.ptr, literal.body.length);
    re->source_length = literal.body.length;
    uint32_t used = literal.body.length;
    for (uint32_t g = 0; g < p->groups; g++) {
        const rx_name *name = &SJS_BUF_AT(p->names, rx_name, g);
        names[g].offset = used;
        names[g].length = name->length;
        for (uint32_t k = 0; k < name->length; k++) text[used++] = (char)p->pat[name->at + k];
    }
    re->text = text;
    re->text_length = used;
    *out = sjs_heap_value(SJS_REGEXP, &re->h);
    return SJS_OK;
}

sjs_str sjs_regex_source(const sjs_heap *regexp) {
    const sjs_regexp *re = (const sjs_regexp *)(const void *)regexp;
    return (sjs_str){ re->text, re->source_length };
}

uint32_t sjs_regex_flag_set(const sjs_heap *regexp) {
    return ((const sjs_regexp *)(const void *)regexp)->flags;
}

sjs_result sjs_regex_compile(sjs_context *context, sjs_regex_literal literal, sjs_value *out) {
    sjs_regex_env env = { &context->counting, context->limits };
    rx_parser p;
    rx_parser_init(&p, env, literal);
    sjs_result r = SJS_OK;
    rx_measure *ms = NULL;
    uint32_t count = SJS_BUF_COUNT(p.nodes, rx_node);
    if (p.nomem) r = SJS_ERR_NOMEM;
    else if (p.outcome != SJS_REGEX_EXACT) r = SJS_ERR_PROGRAM;
    if (r == SJS_OK) {
        ms = sjs_mem_alloc(p.allocator, (size_t)count * sizeof(rx_measure));
        if (ms == NULL || !rx_measure_tree(&p, ms)) r = SJS_ERR_NOMEM;
    }
    rx_emitter e = { 0 };
    e.p = &p;
    e.ms = ms;
    if (r == SJS_OK) {
        /* The automaton's size (docs/regex.md, 5.3 and 9.3): m instructions,
         * the root's and SAVE 0, SAVE 1 and MATCH; 2^d threads of each per
         * position; s slots each. */
        const rx_measure *root = &ms[0];
        uint64_t m = rx_add_sat(root->size, 3);
        uint32_t d = root->registers;
        uint64_t s = 2u * ((uint64_t)p.groups + 1u) + d;
        uint64_t threads = d >= 24u ? SIZE_CAP : rx_mul_sat(m, (uint64_t)1 << d);
        uint64_t states = rx_mul_sat(threads, s);
        uint64_t clears = d >= 24u ? SIZE_CAP : rx_mul_sat(root->clears, (uint64_t)1 << d);
        if (states > context->limits.max_regex_states || clears > context->limits.max_regex_states) r = SJS_ERR_LIMIT;
        e.register_base = 2u * (p.groups + 1u);
    }
    if (r == SJS_OK) {
        rx_emit(&e, RX_SAVE, 0, 0);
        if (rx_push_emit(&e, 0)) {
            while (e.frames.length > 0 && !p.nomem) rx_emit_step(&e);
        }
        rx_emit(&e, RX_SAVE, 1, 0);
        rx_emit(&e, RX_MATCH, 0, 0);
        if (p.nomem) r = SJS_ERR_NOMEM;
    }
    if (r == SJS_OK) r = rx_build_value(context, &e, out);
    sjs_buf_free(p.allocator, &e.code);
    sjs_buf_free(p.allocator, &e.patches);
    sjs_buf_free(p.allocator, &e.frames);
    if (ms != NULL) sjs_mem_free(p.allocator, ms, (size_t)count * sizeof(rx_measure));
    rx_parser_free(&p);
    return r;
}
