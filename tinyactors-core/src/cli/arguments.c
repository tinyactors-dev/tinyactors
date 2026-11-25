#include "cli/arguments.h"

#include "base/memory.h"

/* How the parse lays out what it makes. */
enum {
    CLI_NAME_BOUND = 64,      /* the longest name the edit distance reads */
    CLI_FIRST_VALUES = 8,     /* the values array's first capacity; it doubles */
    CLI_STORAGE_ALIGNMENT = 8 /* every array the parse allocates */
};

/* One parse: the description, the words, and what it has made so far. */
typedef struct cli_parser {
    const ta_cli_program *program;
    ta_cli_words words;
    ta_cli_parsed *out;
    ta_cli_value *values;      /* out->values, writable */
    uint32_t value_capacity;
    ta_string_view *arguments; /* out->arguments, writable; words.count long */
    uint32_t index;            /* the word being read */
    uint32_t answered;         /* an option with TA_CLI_OPTION_ANSWERS was given */
} cli_parser;

typedef struct cli_table {
    uint32_t count;
    const ta_cli_option *options;
} cli_table;

typedef struct cli_found {
    ta_cli_option_id id;
    const ta_cli_option *option; /* NULL: none */
} cli_found;

/* An option as the line gave it, before it is checked and kept. */
typedef struct cli_given {
    cli_found found;
    ta_string_view written; /* "--name" or "-n", as it was written */
    uint32_t word;          /* the option's word */
    ta_string_view text;    /* the value; empty for a flag */
    uint32_t text_word;     /* the value's word */
} cli_given;

/* The nearest of the candidates seen so far, for "did you mean". */
typedef struct cli_nearest {
    ta_string_view typed;
    uint32_t distance; /* the best so far; past the limit while none is near */
    ta_string_view found;
} cli_nearest;

static ta_string_view cli_view(const char *pointer, uint32_t length) {
    ta_string_view view;
    view.pointer = pointer;
    view.length = length;
    return view;
}

static int cli_same(ta_string_view a, ta_string_view b) {
    return a.length == b.length && (a.length == 0 || ta_memcmp(a.pointer, b.pointer, a.length) == 0);
}

static ta_string_view cli_word(const cli_parser *parser, uint32_t index) {
    return ta_string(parser->words.words[index]);
}

uint32_t ta_cli_edit_distance(ta_cli_distance distance) {
    uint32_t beyond = distance.limit == UINT32_MAX ? UINT32_MAX : distance.limit + 1u;
    uint32_t from_length = distance.from.length, to_length = distance.to.length;
    if (from_length > CLI_NAME_BOUND || to_length > CLI_NAME_BOUND) return beyond;
    uint32_t gap = from_length > to_length ? from_length - to_length : to_length - from_length;
    if (gap > distance.limit) return beyond;
    const char *from = distance.from.pointer, *to = distance.to.pointer;
    /* Three rows of the optimal string alignment table: the one before the
     * last (for a transposition), the last and the current. */
    uint32_t rows[3][CLI_NAME_BOUND + 1];
    uint32_t *before = rows[0], *previous = rows[1], *current = rows[2];
    for (uint32_t column = 0; column <= to_length; column++)
        previous[column] = column;
    for (uint32_t row = 1; row <= from_length; row++) {
        current[0] = row;
        for (uint32_t column = 1; column <= to_length; column++) {
            uint32_t cost = from[row - 1] == to[column - 1] ? 0u : 1u;
            uint32_t best = previous[column] + 1u;
            if (current[column - 1] + 1u < best) best = current[column - 1] + 1u;
            if (previous[column - 1] + cost < best) best = previous[column - 1] + cost;
            if (row > 1 && column > 1 && from[row - 1] == to[column - 2] &&
                from[row - 2] == to[column - 1] && before[column - 2] + 1u < best)
                best = before[column - 2] + 1u;
            current[column] = best;
        }
        uint32_t *spare = before;
        before = previous;
        previous = current;
        current = spare;
    }
    uint32_t result = previous[to_length];
    return result > distance.limit ? beyond : result;
}

static cli_nearest cli_nearest_start(ta_string_view typed) {
    cli_nearest nearest;
    uint32_t limit = typed.length / 3u;
    nearest.typed = typed;
    nearest.distance = (limit < 1u ? 1u : limit) + 1u;
    nearest.found = cli_view(NULL, 0);
    return nearest;
}

/* Keeps `candidate` when it is nearer than every one before it: a tie goes
 * to the first. */
static void cli_nearest_consider(cli_nearest *nearest, ta_string_view candidate) {
    if (candidate.length == 0) return;
    ta_cli_distance distance;
    distance.from = nearest->typed;
    distance.to = candidate;
    distance.limit = nearest->distance - 1u;
    uint32_t found = ta_cli_edit_distance(distance);
    if (found < nearest->distance) {
        nearest->distance = found;
        nearest->found = candidate;
    }
}

/* `text` followed by `name`, as one text in the arena, into `text`;
 * TA_ERROR_NO_MEMORY when the arena refuses. */
static ta_status cli_join(cli_parser *parser, ta_string_view *text, ta_string_view name) {
    uint32_t length = text->length + name.length;
    char *joined = (char *)ta_growable_arena_alloc(parser->words.arena, length, 1);
    if (!joined) return TA_ERROR_NO_MEMORY;
    ta_memcpy(joined, text->pointer, text->length);
    if (name.length) ta_memcpy(joined + text->length, name.pointer, name.length);
    *text = cli_view(joined, length);
    return TA_OK;
}

static cli_table cli_scope_table(const cli_parser *parser, uint32_t scope) {
    cli_table table = {0, NULL};
    if (scope == TA_CLI_SCOPE_PROGRAM) {
        table.count = parser->program->option_count;
        table.options = parser->program->options;
    } else if (parser->out->command != TA_NIL) {
        const ta_cli_command *command = &parser->program->commands[parser->out->command];
        table.count = command->option_count;
        table.options = command->options;
    }
    return table;
}

/* The scopes in the order an option is looked up and suggested: the
 * command's, which is the more particular, then the program's. */
static const uint32_t cli_scope_order[2] = {TA_CLI_SCOPE_COMMAND, TA_CLI_SCOPE_PROGRAM};

/* The option named `name` (long, when `letter` is 0) or `letter`. */
typedef struct cli_wanted {
    ta_string_view name;
    uint32_t letter;
} cli_wanted;

static cli_found cli_lookup(const cli_parser *parser, cli_wanted wanted) {
    cli_found found = {{TA_NIL, TA_NIL}, NULL};
    for (uint32_t order = 0; order < 2; order++) {
        cli_table table = cli_scope_table(parser, cli_scope_order[order]);
        for (uint32_t index = 0; index < table.count; index++) {
            const ta_cli_option *option = &table.options[index];
            int match = wanted.letter ? option->short_name == wanted.letter
                                      : option->name.length && cli_same(option->name, wanted.name);
            if (!match) continue;
            found.id.scope = cli_scope_order[order];
            found.id.option = index;
            found.option = option;
            return found;
        }
    }
    return found;
}

/* Starts out->error: every field but the kind and the word is filled in by
 * the caller. */
static ta_cli_error *cli_error(cli_parser *parser, uint32_t kind, uint32_t word) {
    ta_cli_error *error = &parser->out->error;
    error->kind = kind;
    error->word = word;
    error->option.scope = TA_NIL;
    error->option.option = TA_NIL;
    error->text = cli_view(NULL, 0);
    error->suggestion = cli_view(NULL, 0);
    return error;
}

/* An option `written` (its name `typed`) that is in no table in scope. */
static ta_status cli_unknown_option(cli_parser *parser, ta_string_view written, ta_string_view typed) {
    cli_nearest nearest = cli_nearest_start(typed);
    for (uint32_t order = 0; order < 2; order++) {
        cli_table table = cli_scope_table(parser, cli_scope_order[order]);
        for (uint32_t index = 0; index < table.count; index++)
            cli_nearest_consider(&nearest, table.options[index].name);
    }
    ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_UNKNOWN_OPTION, parser->index);
    error->text = written;
    if (nearest.found.length) {
        ta_string_view suggestion = cli_view("--", 2);
        if (cli_join(parser, &suggestion, nearest.found) != TA_OK) return TA_ERROR_NO_MEMORY;
        error->suggestion = suggestion;
    }
    return TA_ERROR_INVALID;
}

static ta_status cli_keep_value(cli_parser *parser, const cli_given *given, uint32_t choice) {
    ta_cli_parsed *out = parser->out;
    if (out->value_count == parser->value_capacity) {
        uint32_t capacity = parser->value_capacity ? parser->value_capacity * 2u : CLI_FIRST_VALUES;
        if (capacity < parser->value_capacity) return TA_ERROR_NO_MEMORY;
        ta_cli_value *values = (ta_cli_value *)ta_growable_arena_alloc(
            parser->words.arena, (size_t)capacity * sizeof *values, CLI_STORAGE_ALIGNMENT);
        if (!values) return TA_ERROR_NO_MEMORY;
        if (out->value_count) ta_memcpy(values, parser->values, (size_t)out->value_count * sizeof *values);
        parser->values = values;
        parser->value_capacity = capacity;
        out->values = values;
    }
    ta_cli_value *value = &parser->values[out->value_count];
    value->option.scope = given->found.id.scope;
    value->option.option = given->found.id.option;
    value->choice = choice;
    value->word = given->word;
    value->text = given->text;
    out->value_count++;
    if (given->found.option->flags & TA_CLI_OPTION_ANSWERS) parser->answered = 1;
    return TA_OK;
}

/* Checks an option as given and keeps it. */
static ta_status cli_add_value(cli_parser *parser, const cli_given *given) {
    const ta_cli_option *option = given->found.option;
    if (option->kind != TA_CLI_OPTION_FLAG && !(option->flags & TA_CLI_OPTION_REPEATABLE) &&
        ta_cli_count(parser->out, given->found.id) > 0) {
        ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_REPEATED_OPTION, given->word);
        error->option = given->found.id;
        error->text = given->written;
        return TA_ERROR_INVALID;
    }
    uint32_t choice = 0;
    if (option->kind == TA_CLI_OPTION_CHOICE) {
        cli_nearest nearest = cli_nearest_start(given->text);
        for (choice = 0; choice < option->choice_count; choice++) {
            if (cli_same(option->choices[choice], given->text)) break;
            cli_nearest_consider(&nearest, option->choices[choice]);
        }
        if (choice == option->choice_count) {
            ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_INVALID_CHOICE, given->text_word);
            error->option = given->found.id;
            error->text = given->text;
            error->suggestion = nearest.found;
            return TA_ERROR_INVALID;
        }
    }
    return cli_keep_value(parser, given, choice);
}

/* A value option's value from the next word, when its own word held none. */
static ta_status cli_next_value(cli_parser *parser, cli_given *given) {
    if (parser->index + 1u >= parser->words.count) {
        ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_MISSING_VALUE, given->word);
        error->option = given->found.id;
        error->text = given->written;
        return TA_ERROR_INVALID;
    }
    parser->index++;
    given->text = cli_word(parser, parser->index);
    given->text_word = parser->index;
    return TA_OK;
}

/* --name, --name VALUE, --name=VALUE */
static ta_status cli_read_long(cli_parser *parser, ta_string_view word) {
    uint32_t end = 2;
    while (end < word.length && word.pointer[end] != '=')
        end++;
    cli_wanted wanted = {cli_view(word.pointer + 2, end - 2u), 0};
    cli_given given;
    given.written = cli_view(word.pointer, end);
    given.word = parser->index;
    given.text = cli_view(NULL, 0);
    given.text_word = parser->index;
    given.found = cli_lookup(parser, wanted);
    if (!given.found.option) return cli_unknown_option(parser, given.written, wanted.name);
    int has_value = end < word.length;
    if (given.found.option->kind == TA_CLI_OPTION_FLAG) {
        if (has_value) {
            ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_UNEXPECTED_VALUE, parser->index);
            error->option = given.found.id;
            error->text = given.written;
            return TA_ERROR_INVALID;
        }
    } else if (has_value) {
        given.text = cli_view(word.pointer + end + 1, word.length - end - 1u);
    } else {
        ta_status status = cli_next_value(parser, &given);
        if (status != TA_OK) return status;
    }
    return cli_add_value(parser, &given);
}

/* -n, -n VALUE, -nVALUE, -abc, -abo FILE */
static ta_status cli_read_cluster(cli_parser *parser, ta_string_view word) {
    for (uint32_t at = 1; at < word.length; at++) {
        cli_wanted wanted = {cli_view(word.pointer + at, 1), (unsigned char)word.pointer[at]};
        cli_given given;
        given.word = parser->index;
        given.text = cli_view(NULL, 0);
        given.text_word = parser->index;
        given.written = cli_view(word.pointer, 2);
        if (at > 1) {
            given.written = cli_view("-", 1);
            if (cli_join(parser, &given.written, wanted.name) != TA_OK) return TA_ERROR_NO_MEMORY;
        }
        given.found = cli_lookup(parser, wanted);
        if (!given.found.option) return cli_unknown_option(parser, given.written, wanted.name);
        if (given.found.option->kind == TA_CLI_OPTION_FLAG) {
            ta_status status = cli_add_value(parser, &given);
            if (status != TA_OK) return status;
            continue;
        }
        if (at + 1u < word.length) {
            given.text = cli_view(word.pointer + at + 1, word.length - at - 1u);
        } else {
            ta_status status = cli_next_value(parser, &given);
            if (status != TA_OK) return status;
        }
        return cli_add_value(parser, &given);
    }
    return TA_OK;
}

/* A word that is not an option: the command, or else an argument. */
static ta_status cli_read_operand(cli_parser *parser, ta_string_view word) {
    ta_cli_parsed *out = parser->out;
    const ta_cli_program *program = parser->program;
    if (out->command == TA_NIL) {
        cli_nearest nearest = cli_nearest_start(word);
        for (uint32_t index = 0; index < program->command_count; index++) {
            if (program->commands[index].name.length && cli_same(program->commands[index].name, word)) {
                out->command = index;
                return TA_OK;
            }
            cli_nearest_consider(&nearest, program->commands[index].name);
        }
        ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_UNKNOWN_COMMAND, parser->index);
        error->text = word;
        error->suggestion = nearest.found;
        return TA_ERROR_INVALID;
    }
    if (out->argument_count >= program->commands[out->command].maximum) {
        ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_TOO_MANY_ARGUMENTS, parser->index);
        error->text = word;
        return TA_ERROR_INVALID;
    }
    if (!parser->arguments) {
        parser->arguments = (ta_string_view *)ta_growable_arena_alloc(
            parser->words.arena, (size_t)parser->words.count * sizeof *parser->arguments,
            CLI_STORAGE_ALIGNMENT);
        if (!parser->arguments) return TA_ERROR_NO_MEMORY;
        out->arguments = parser->arguments;
    }
    parser->arguments[out->argument_count++] = word;
    return TA_OK;
}

/* The required options of one table that were not given. */
static ta_status cli_check_required(cli_parser *parser, uint32_t scope) {
    cli_table table = cli_scope_table(parser, scope);
    for (uint32_t index = 0; index < table.count; index++) {
        const ta_cli_option *option = &table.options[index];
        ta_cli_option_id id = {scope, index};
        if (!(option->flags & TA_CLI_OPTION_REQUIRED) || ta_cli_count(parser->out, id) > 0) continue;
        ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_MISSING_OPTION, TA_NIL);
        error->option = id;
        /* "--name", or "-n" for an option with no long name */
        char letter = (char)option->short_name;
        ta_string_view spelled = option->name.length ? cli_view("--", 2) : cli_view("-", 1);
        ta_string_view name = option->name.length ? option->name : cli_view(&letter, 1);
        if (cli_join(parser, &spelled, name) != TA_OK) return TA_ERROR_NO_MEMORY;
        error->text = spelled;
        return TA_ERROR_INVALID;
    }
    return TA_OK;
}

/* What the line as a whole must hold, unless an option answers by itself. */
static ta_status cli_finish(cli_parser *parser) {
    ta_cli_parsed *out = parser->out;
    if (parser->answered) return TA_OK;
    if (out->command == TA_NIL) {
        cli_error(parser, TA_CLI_ERROR_NO_COMMAND, TA_NIL);
        return TA_ERROR_INVALID;
    }
    const ta_cli_command *command = &parser->program->commands[out->command];
    if (out->argument_count < command->minimum) {
        ta_cli_error *error = cli_error(parser, TA_CLI_ERROR_TOO_FEW_ARGUMENTS, TA_NIL);
        error->text = command->name;
        return TA_ERROR_INVALID;
    }
    ta_status status = cli_check_required(parser, TA_CLI_SCOPE_PROGRAM);
    if (status != TA_OK) return status;
    return cli_check_required(parser, TA_CLI_SCOPE_COMMAND);
}

ta_status ta_cli_parse(const ta_cli_program *program, ta_cli_words words, ta_cli_parsed *out) {
    if (!out) return TA_ERROR_INVALID;
    out->command = TA_NIL;
    out->value_count = 0;
    out->values = NULL;
    out->argument_count = 0;
    out->arguments = NULL;
    cli_parser parser;
    parser.program = program;
    parser.words = words;
    parser.out = out;
    parser.values = NULL;
    parser.value_capacity = 0;
    parser.arguments = NULL;
    parser.index = 0;
    parser.answered = 0;
    cli_error(&parser, TA_CLI_ERROR_NONE, TA_NIL);
    if (!program || (words.count && !words.words)) return TA_ERROR_INVALID;
    int options_ended = 0;
    for (; parser.index < words.count; parser.index++) {
        ta_string_view word = cli_word(&parser, parser.index);
        ta_status status;
        if (!options_ended && word.length == 2 && word.pointer[0] == '-' && word.pointer[1] == '-') {
            options_ended = 1;
            continue;
        }
        if (options_ended || word.length < 2 || word.pointer[0] != '-')
            status = cli_read_operand(&parser, word);
        else if (word.pointer[1] == '-')
            status = cli_read_long(&parser, word);
        else
            status = cli_read_cluster(&parser, word);
        if (status != TA_OK) return status;
    }
    return cli_finish(&parser);
}

uint32_t ta_cli_count(const ta_cli_parsed *parsed, ta_cli_option_id option) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < parsed->value_count; index++)
        if (parsed->values[index].option.scope == option.scope &&
            parsed->values[index].option.option == option.option)
            count++;
    return count;
}

const ta_cli_value *ta_cli_last(const ta_cli_parsed *parsed, ta_cli_option_id option) {
    for (uint32_t index = parsed->value_count; index > 0; index--) {
        const ta_cli_value *value = &parsed->values[index - 1u];
        if (value->option.scope == option.scope && value->option.option == option.option) return value;
    }
    return NULL;
}
