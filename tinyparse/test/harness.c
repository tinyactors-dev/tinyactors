/* tinyparse test harness — runs DATA-driven CLI tests against an xfail
 * ledger and ratchets toward a green finish line.
 *
 * This is host development tooling (it shells out, reads files, prints), so it
 * uses libc freely; it is NOT part of the freestanding library. Test cases are
 * data, never code: every behaviour tinyparse must exhibit is a `.test` file,
 * and `xfail.list` records which ones are still allowed to fail.
 *
 * --- Test file format (see test/README.md) --------------------------------
 *
 * Sections begin with a line whose first three bytes are "<<<" followed by the
 * section name. A test declares an invocation (a `cmd`, or a `grammar` that the
 * harness turns into the standard invocation) plus a list of assertions.
 *
 *   <<<input            bytes for the input, written to a temp file as $IN
 *   <<<input NAME       a named input blob, exposed as $NAME (a file path)
 *   <<<grammar          a grammar path; the harness builds the standard cmd
 *   <<<mode             run | check  (default run) for the grammar template
 *   <<<args             extra arguments appended to the templated command
 *   <<<cmd              an explicit shell command (overrides the template)
 *   <<<stdin            bytes fed to the command's standard input
 *   <<<assert           one assertion per line (see below); ALL must hold
 *   <<<stdout           exact expected standard output (golden)
 *   <<<stderr           exact expected standard error (golden)
 *   <<<end              optional terminator
 *
 * Assertion directives (inside <<<assert; '#' and blank lines ignored):
 *
 *   exit N                  exit code equals N
 *   exit-not N              exit code is not N
 *   out-contains TEXT       stdout contains TEXT (literal substring)
 *   out-absent TEXT         stdout does not contain TEXT
 *   out-regex RE            stdout matches POSIX extended regex RE
 *   out-order A ~ B ~ C     these substrings occur, in this order, in stdout
 *   out-count N TEXT        stdout contains exactly N occurrences of TEXT
 *   err-contains TEXT       stderr contains TEXT
 *   err-absent TEXT         stderr does not contain TEXT
 *   err-regex RE            stderr matches POSIX extended regex RE
 *
 * Inside cmd/args, $TP is the CLI, $IN the default input file, $GRAMMAR the
 * grammar path, and $NAME each named input file.
 *
 * Usage: harness --suite DIR --xfail FILE --cwd DIR
 *
 * Exit code: 0 only when every test passes AND xfail.list is empty (done);
 * 1 if any test fails that is not listed in xfail (a regression); 2 if the
 * remaining failures are all expected (work still pending).
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* ** Growable byte buffer ** */
typedef struct buffer {
    char  *data;
    size_t len;
    size_t cap;
} buffer;

static void buffer_init(buffer *b) { b->data = NULL; b->len = 0; b->cap = 0; }

static void buffer_push(buffer *b, const char *p, size_t n) {
    if (b->len + n + 1 > b->cap) {
        size_t want = (b->cap ? b->cap * 2 : 256);
        while (want < b->len + n + 1) want *= 2;
        b->data = realloc(b->data, want);
        if (!b->data) { perror("realloc"); exit(3); }
        b->cap = want;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    b->data[b->len] = '\0';
}

static void buffer_free(buffer *b) { free(b->data); buffer_init(b); }

static int read_file(const char *path, buffer *out) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    buffer_init(out);
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof chunk, f)) > 0) buffer_push(out, chunk, n);
    fclose(f);
    return 0;
}

static int write_file(const char *path, const char *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    if (len) fwrite(data, 1, len, f);
    fclose(f);
    return 0;
}

static void rstrip(buffer *b) {
    while (b->len > 0) {
        char c = b->data[b->len - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') b->len--;
        else break;
    }
    if (b->data) b->data[b->len] = '\0';
}

/* trim leading and trailing ASCII whitespace of a NUL-terminated string in place */
static char *trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    size_t e = strlen(s);
    while (e > 0 && (s[e-1]==' '||s[e-1]=='\t'||s[e-1]=='\r'||s[e-1]=='\n')) s[--e] = '\0';
    return s;
}

/* count non-overlapping occurrences of needle in hay */
static int count_occ(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return 0;
    int c = 0;
    const char *p = hay;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle))) { c++; p += nl; }
    return c;
}

/* ** A named input blob ** */
typedef struct input { char *name; buffer data; struct input *next; } input;

/* ** A parsed test case ** */
typedef struct testcase {
    buffer cmd;        int have_cmd;
    buffer grammar;    int have_grammar;
    buffer mode;       /* run|check */
    buffer args;
    buffer stdin_data; int have_stdin;
    buffer assertions;
    buffer stdout_exact; int have_stdout_exact;
    buffer stderr_exact; int have_stderr_exact;
    input *inputs;     /* linked list; the default-named one is "IN" */
} testcase;

static void testcase_init(testcase *t) { memset(t, 0, sizeof *t); buffer_init(&t->mode); }

static void testcase_free(testcase *t) {
    buffer_free(&t->cmd); buffer_free(&t->grammar); buffer_free(&t->mode);
    buffer_free(&t->args); buffer_free(&t->stdin_data); buffer_free(&t->assertions);
    buffer_free(&t->stdout_exact); buffer_free(&t->stderr_exact);
    input *p = t->inputs;
    while (p) { input *n = p->next; free(p->name); buffer_free(&p->data); free(p); p = n; }
}

static input *testcase_add_input(testcase *t, const char *name) {
    input *in = calloc(1, sizeof *in);
    in->name = strdup(name && *name ? name : "IN");
    buffer_init(&in->data);
    in->next = t->inputs; t->inputs = in;
    return in;
}

/* ** Parse a .test file into a testcase ** */
static int parse_test(const char *path, testcase *t) {
    buffer file;
    if (read_file(path, &file) != 0) return -1;
    testcase_init(t);

    char *p = file.data, *end = file.data + file.len;
    buffer *section = NULL;   /* current accumulator, or NULL */

    while (p < end) {
        char *nl = memchr(p, '\n', (size_t)(end - p));
        size_t line_len = nl ? (size_t)(nl - p) : (size_t)(end - p);
        char *line = p;

        if (line_len >= 3 && line[0]=='<' && line[1]=='<' && line[2]=='<') {
            char head[128];
            size_t hl = line_len - 3; if (hl >= sizeof head) hl = sizeof head - 1;
            memcpy(head, line + 3, hl); head[hl] = '\0';
            /* split "name [arg]" */
            char *sp = head; while (*sp && *sp!=' ' && *sp!='\t') sp++;
            char arg[96] = "";
            if (*sp) { *sp = '\0'; strncpy(arg, trim(sp+1), sizeof arg - 1); }
            char *name = trim(head);

            section = NULL;
            if      (!strcmp(name, "cmd"))     { section = &t->cmd;        t->have_cmd = 1; }
            else if (!strcmp(name, "grammar")) { section = &t->grammar;    t->have_grammar = 1; }
            else if (!strcmp(name, "mode"))      section = &t->mode;
            else if (!strcmp(name, "args"))      section = &t->args;
            else if (!strcmp(name, "stdin"))   { section = &t->stdin_data; t->have_stdin = 1; }
            else if (!strcmp(name, "assert"))    section = &t->assertions;
            else if (!strcmp(name, "stdout"))  { section = &t->stdout_exact; t->have_stdout_exact = 1; }
            else if (!strcmp(name, "stderr"))  { section = &t->stderr_exact; t->have_stderr_exact = 1; }
            else if (!strcmp(name, "input"))     section = &testcase_add_input(t, arg)->data;
            else if (!strcmp(name, "end"))       section = NULL;
        } else if (section) {
            buffer_push(section, line, line_len);
            buffer_push(section, "\n", 1);
        }
        if (!nl) break;
        p = nl + 1;
    }
    buffer_free(&file);
    return 0;
}

/* single-quote a value for the shell (assumes no embedded single quotes) */
static void push_quoted(buffer *b, const char *key, const char *val) {
    buffer_push(b, key, strlen(key));
    buffer_push(b, "='", 2);
    buffer_push(b, val, strlen(val));
    buffer_push(b, "' ", 2);
}

/* ** One assertion result ** */
static int regex_match(const char *hay, const char *re) {
    regex_t rx;
    if (regcomp(&rx, re, REG_EXTENDED | REG_NEWLINE) != 0) return 0;
    int m = regexec(&rx, hay ? hay : "", 0, NULL, 0) == 0;
    regfree(&rx);
    return m;
}

/* Evaluate the assertion block against captured output. Appends per-failure
 * messages to `fails`. Returns the number of assertions that failed. */
static int eval_assertions(const char *asrt, int code, const char *out, const char *err,
                           buffer *fails) {
    int failed = 0;
    if (!asrt) return 0;
    char *copy = strdup(asrt);
    char *line = copy, *save;
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s = trim(line);
        if (!*s || *s == '#') continue;
        /* keyword = first token, rest = remainder */
        char *rest = s; while (*rest && *rest!=' ' && *rest!='\t') rest++;
        char kw[32]; size_t kl = (size_t)(rest - s);
        if (kl >= sizeof kw) kl = sizeof kw - 1;
        memcpy(kw, s, kl); kw[kl] = '\0';
        while (*rest==' '||*rest=='\t') rest++;

        int ok = 1; char msg[512] = "";
        if (!strcmp(kw, "exit")) {
            ok = (code == atoi(rest));
            if (!ok) snprintf(msg, sizeof msg, "exit %d, expected %s", code, rest);
        } else if (!strcmp(kw, "exit-not")) {
            ok = (code != atoi(rest));
            if (!ok) snprintf(msg, sizeof msg, "exit %d, expected not %s", code, rest);
        } else if (!strcmp(kw, "out-contains")) {
            ok = out && strstr(out, rest) != NULL;
            if (!ok) snprintf(msg, sizeof msg, "stdout missing '%s'", rest);
        } else if (!strcmp(kw, "out-absent")) {
            ok = !(out && strstr(out, rest));
            if (!ok) snprintf(msg, sizeof msg, "stdout unexpectedly contains '%s'", rest);
        } else if (!strcmp(kw, "out-regex")) {
            ok = regex_match(out, rest);
            if (!ok) snprintf(msg, sizeof msg, "stdout does not match /%s/", rest);
        } else if (!strcmp(kw, "err-contains")) {
            ok = err && strstr(err, rest) != NULL;
            if (!ok) snprintf(msg, sizeof msg, "stderr missing '%s'", rest);
        } else if (!strcmp(kw, "err-absent")) {
            ok = !(err && strstr(err, rest));
            if (!ok) snprintf(msg, sizeof msg, "stderr unexpectedly contains '%s'", rest);
        } else if (!strcmp(kw, "err-regex")) {
            ok = regex_match(err, rest);
            if (!ok) snprintf(msg, sizeof msg, "stderr does not match /%s/", rest);
        } else if (!strcmp(kw, "out-count")) {
            int want = atoi(rest);
            char *txt = rest; while (*txt && *txt!=' ' && *txt!='\t') txt++;
            while (*txt==' '||*txt=='\t') txt++;
            int got = count_occ(out, txt);
            ok = (got == want);
            if (!ok) snprintf(msg, sizeof msg, "stdout has %d of '%s', expected %d", got, txt, want);
        } else if (!strcmp(kw, "out-order")) {
            /* parts separated by " ~ ", each must appear after the previous */
            char *partcopy = strdup(rest), *psave, *part;
            const char *cursor = out ? out : "";
            size_t base = 0;
            ok = 1;
            for (part = strtok_r(partcopy, "~", &psave); part; part = strtok_r(NULL, "~", &psave)) {
                char *needle = trim(part);
                if (!*needle) continue;
                const char *hit = strstr(cursor + base, needle);
                if (!hit) { ok = 0; snprintf(msg, sizeof msg, "stdout out-of-order: '%s' not found after prior", needle); break; }
                base = (size_t)(hit - cursor) + strlen(needle);
            }
            free(partcopy);
        } else {
            ok = 0;
            snprintf(msg, sizeof msg, "unknown assertion '%s'", kw);
        }

        if (!ok) {
            failed++;
            buffer_push(fails, "        - ", 10);
            buffer_push(fails, msg, strlen(msg));
            buffer_push(fails, "\n", 1);
        }
    }
    free(copy);
    return failed;
}

/* ** Run one test; return 1 on PASS, 0 on FAIL, with failure detail. ** */
static int run_test(const testcase *t, const char *cwd, buffer *detail) {
    char dir[] = "/tmp/tpharness.XXXXXX";
    if (!mkdtemp(dir)) { buffer_push(detail, "        - mkdtemp failed\n", 25); return 0; }

    /* write input blobs to temp files; remember their paths for env + cleanup */
    buffer env; buffer_init(&env);
    char in_default[300] = "/dev/null";
    char stdin_path[300]; snprintf(stdin_path, sizeof stdin_path, "%s/stdin", dir);
    write_file(stdin_path, t->stdin_data.data ? t->stdin_data.data : "", t->stdin_data.len);

    int idx = 0;
    for (input *in = t->inputs; in; in = in->next) {
        char path[300]; snprintf(path, sizeof path, "%s/in%d", dir, idx++);
        write_file(path, in->data.data ? in->data.data : "", in->data.len);
        push_quoted(&env, in->name, path);
        if (!strcmp(in->name, "IN")) strncpy(in_default, path, sizeof in_default - 1);
    }

    /* resolve the command: explicit cmd, else the grammar template */
    buffer cmd; buffer_init(&cmd);
    if (t->have_cmd) {
        buffer_push(&cmd, t->cmd.data ? t->cmd.data : "", t->cmd.len);
    } else if (t->have_grammar) {
        char g[512] = ""; if (t->grammar.data) { strncpy(g, t->grammar.data, sizeof g - 1); } trim(g);
        push_quoted(&env, "GRAMMAR", g);
        char m[16] = "run"; if (t->mode.data) { strncpy(m, t->mode.data, sizeof m - 1); } trim(m);
        char a[512] = ""; if (t->args.data) { strncpy(a, t->args.data, sizeof a - 1); } trim(a);
        char templ[1400];
        if (!strcmp(m, "check"))
            snprintf(templ, sizeof templ, "\"$TP\" check \"$GRAMMAR\" %s", a);
        else
            snprintf(templ, sizeof templ, "\"$TP\" run \"$GRAMMAR\" --input \"%s\" --dump-tree %s", in_default, a);
        buffer_push(&cmd, templ, strlen(templ));
    } else {
        buffer_push(detail, "        - test has neither <<<cmd nor <<<grammar\n", 48);
        buffer_free(&env); buffer_free(&cmd); rmdir(dir);
        return 0;
    }

    char cmd_path[300]; snprintf(cmd_path, sizeof cmd_path, "%s/cmd", dir);
    write_file(cmd_path, cmd.data, cmd.len);

    char out_path[300], err_path[300];
    snprintf(out_path, sizeof out_path, "%s/out", dir);
    snprintf(err_path, sizeof err_path, "%s/err", dir);

    buffer shell; buffer_init(&shell);
    char hdr[2048];
    snprintf(hdr, sizeof hdr, "cd '%s' && ", cwd);
    buffer_push(&shell, hdr, strlen(hdr));
    buffer_push(&shell, env.data ? env.data : "", env.len);
    snprintf(hdr, sizeof hdr, "/bin/sh '%s' < '%s' > '%s' 2> '%s'",
             cmd_path, stdin_path, out_path, err_path);
    buffer_push(&shell, hdr, strlen(hdr));

    int status = system(shell.data);
    int code = (status == -1) ? 127 : (WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status));

    buffer out, err; buffer_init(&out); buffer_init(&err);
    read_file(out_path, &out);
    read_file(err_path, &err);

    int failed = eval_assertions(t->assertions.data, code, out.data, err.data, detail);

    if (t->have_stdout_exact) {
        buffer want; buffer_init(&want);
        buffer_push(&want, t->stdout_exact.data ? t->stdout_exact.data : "", t->stdout_exact.len);
        rstrip(&want); rstrip(&out);
        if (strcmp(out.data ? out.data : "", want.data ? want.data : "")) {
            failed++; buffer_push(detail, "        - stdout exact mismatch\n", 32);
        }
        buffer_free(&want);
    }
    if (t->have_stderr_exact) {
        buffer want; buffer_init(&want);
        buffer_push(&want, t->stderr_exact.data ? t->stderr_exact.data : "", t->stderr_exact.len);
        rstrip(&want); rstrip(&err);
        if (strcmp(err.data ? err.data : "", want.data ? want.data : "")) {
            failed++; buffer_push(detail, "        - stderr exact mismatch\n", 32);
        }
        buffer_free(&want);
    }

    /* cleanup */
    for (int i = 0; i < idx; i++) { char path[300]; snprintf(path, sizeof path, "%s/in%d", dir, i); unlink(path); }
    unlink(stdin_path); unlink(cmd_path); unlink(out_path); unlink(err_path); rmdir(dir);
    buffer_free(&env); buffer_free(&cmd); buffer_free(&shell);
    buffer_free(&out); buffer_free(&err);
    return failed == 0;
}

/* ** xfail ledger ** */
typedef struct nameset { char **names; size_t count, cap; } nameset;

static void nameset_add(nameset *s, const char *name) {
    if (s->count == s->cap) { s->cap = s->cap ? s->cap*2 : 32; s->names = realloc(s->names, s->cap*sizeof *s->names); }
    s->names[s->count++] = strdup(name);
}
static int nameset_has(const nameset *s, const char *name) {
    for (size_t i = 0; i < s->count; i++) if (!strcmp(s->names[i], name)) return 1;
    return 0;
}
static void load_xfail(const char *path, nameset *s) {
    buffer f; if (read_file(path, &f) != 0) return;
    char *p = f.data;
    while (p && *p) {
        char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char line[512]; size_t L = len; if (L >= sizeof line) L = sizeof line - 1;
        memcpy(line, p, L); line[L] = '\0';
        char *a = trim(line);
        if (*a && *a != '#') nameset_add(s, a);
        if (!nl) break;
        p = nl + 1;
    }
    buffer_free(&f);
}

/* ** Collect *.test files recursively, names relative to root, sorted ** */
static void collect(const char *root, const char *rel, nameset *tests) {
    char path[1024];
    snprintf(path, sizeof path, "%s%s%s", root, *rel ? "/" : "", rel);
    DIR *d = opendir(path); if (!d) return;
    struct dirent *de;
    nameset entries; memset(&entries, 0, sizeof entries);
    while ((de = readdir(d))) { if (de->d_name[0] == '.') continue; nameset_add(&entries, de->d_name); }
    closedir(d);
    for (size_t i = 1; i < entries.count; i++) {
        char *key = entries.names[i]; size_t j = i;
        while (j > 0 && strcmp(entries.names[j-1], key) > 0) { entries.names[j] = entries.names[j-1]; j--; }
        entries.names[j] = key;
    }
    for (size_t i = 0; i < entries.count; i++) {
        char *name = entries.names[i];
        char childrel[1024];
        snprintf(childrel, sizeof childrel, "%s%s%s", rel, *rel ? "/" : "", name);
        char full[2300];
        snprintf(full, sizeof full, "%s/%s", root, childrel);
        struct stat st; if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) collect(root, childrel, tests);
        else {
            size_t l = strlen(childrel);
            if (l > 5 && !strcmp(childrel + l - 5, ".test")) { childrel[l-5] = '\0'; nameset_add(tests, childrel); }
        }
        free(name);
    }
    free(entries.names);
}

int main(int argc, char **argv) {
    const char *suite = "test/suite", *xfail_path = "test/xfail.list", *cwd = ".";
    int verbose = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--suite") && i+1 < argc) suite = argv[++i];
        else if (!strcmp(argv[i], "--xfail") && i+1 < argc) xfail_path = argv[++i];
        else if (!strcmp(argv[i], "--cwd") && i+1 < argc) cwd = argv[++i];
        else if (!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")) verbose = 1;
    }

    nameset xfail; memset(&xfail, 0, sizeof xfail); load_xfail(xfail_path, &xfail);
    nameset tests; memset(&tests, 0, sizeof tests); collect(suite, "", &tests);

    int passing = 0, hardfail = 0, xfail_fail = 0, xpass = 0;

    for (size_t i = 0; i < tests.count; i++) {
        const char *name = tests.names[i];
        char file[1024]; snprintf(file, sizeof file, "%s/%s.test", suite, name);
        testcase t; buffer detail; buffer_init(&detail);
        int ok = 0;
        if (parse_test(file, &t) == 0) { ok = run_test(&t, cwd, &detail); testcase_free(&t); }
        else buffer_push(&detail, "        - could not read test file\n", 35);

        int listed = nameset_has(&xfail, name);
        const char *tag;
        if (ok && listed)  { tag = "XPASS"; xpass++; passing++; }
        else if (ok)       { tag = "PASS";  passing++; }
        else if (listed)   { tag = "xfail"; xfail_fail++; }
        else               { tag = "FAIL";  hardfail++; }

        if (ok && listed)
            printf("  %-6s %s  (remove from xfail.list)\n", tag, name);
        else
            printf("  %-6s %s\n", tag, name);
        /* show why a test failed: always for a regression, on -v for an xfail */
        if (!ok && (!listed || verbose) && detail.len)
            fputs(detail.data, stdout);
        buffer_free(&detail);
    }

    int n = (int)tests.count;
    printf("\nPROGRESS=%d/%d  PASS=%d  xfail=%d  XPASS=%d  FAIL=%d  xfail-ledger=%zu\n",
           passing, n, passing, xfail_fail, xpass, hardfail, xfail.count);

    if (hardfail > 0) { printf("RESULT: RED — %d regression(s): a test not in xfail.list failed.\n", hardfail); return 1; }
    if (passing == n && xfail.count == 0) { printf("RESULT: GREEN — every test passes and the xfail ledger is empty.\n"); return 0; }
    if (xpass > 0) printf("RESULT: RATCHET — %d test(s) now pass; remove them from xfail.list.\n", xpass);
    else printf("RESULT: pending — %d expected failure(s) remain in the ledger.\n", xfail_fail);
    return 2;
}
