#define _POSIX_C_SOURCE 200809L
/* macOS hides what POSIX does not define (INADDR_LOOPBACK,
 * _SC_NPROCESSORS_ONLN, st_mtimespec) unless asked; Linux ignores this. */
#define _DARWIN_C_SOURCE
#include "../harness/harness.h"
#include "../../tools/torture/postgres.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct fixture {
    bench_loop *loop;
    bench_postgres *postgres;
    int server, calls;
    char json[1024], error[512];
} fixture;
static void done(void *user, const char *json, const char *error) {
    fixture *f = user;
    f->calls++;
    snprintf(f->json, sizeof f->json, "%s", json);
    snprintf(f->error, sizeof f->error, "%s", error ? error : "");
}
static fixture connect_fake(int timeout) {
    fixture f = {0};
    f.loop = bench_loop_new();
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    CHECK_EQUAL(bind(listener, (struct sockaddr *)&a, sizeof a), 0);
    CHECK_EQUAL(listen(listener, 1), 0);
    socklen_t length = sizeof a;
    CHECK_EQUAL(getsockname(listener, (struct sockaddr *)&a, &length), 0);
    f.postgres = bench_postgres_connect(f.loop, ntohs(a.sin_port), "test", timeout);
    CHECK(f.postgres != NULL);
    f.server = accept(listener, NULL, NULL);
    close(listener);
    int one = 1;
    setsockopt(f.server, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    return f;
}
static void cleanup(fixture *f) {
    bench_postgres_close(f->postgres);
    close(f->server);
    bench_loop_free(f->loop);
}
/* One byte per dispatch, including headers, field lengths, NULs and row data. */
static void feed(fixture *f, const unsigned char *bytes, size_t n) {
    for (size_t i = 0; i < n; i++) {
        CHECK_EQUAL(send(f->server, bytes + i, 1, 0), 1);
        bench_loop_poll(f->loop, bench_loop_now() + 2);
    }
}
#define FEED(f, text) feed(f, (const unsigned char *)(text), sizeof(text) - 1)
static void test_fragmented_results_and_sql_error(void) {
    fixture f = connect_fake(500);
    CHECK_EQUAL(bench_postgres_query(f.postgres, "select 1", done, &f), 0);
    CHECK_EQUAL(f.calls, 0);
    FEED(&f, "R\0\0\0\10\0\0\0\0Z\0\0\0\5I");
    /* Two columns, then two asymmetric rows: escaped string/null and empty/7. */
    FEED(&f, "T\0\0\0\56\0\2a\0"
             "\0\0\0\0\0\0\0\0\0\31\377\377\377\377\377\377\0\0"
             "b\0\0\0\0\0\0\0\0\0\0\31\377\377\377\377\377\377\0\0"
             "D\0\0\0\22\0\2\0\0\0\4x\"\n\\\377\377\377\377"
             "D\0\0\0\17\0\2\0\0\0\0\0\0\0\1"
             "7");
    CHECK_EQUAL(f.calls, 0); /* rows are not a completion: wait for ReadyForQuery */
    FEED(&f, "Z\0\0\0\5I");
    CHECK_EQUAL(f.calls, 1);
    CHECK(!f.error[0]);
    CHECK(!strcmp(f.json, "[{\"a\":\"x\\\"\\u000a\\\\\",\"b\":null},{\"a\":\"\",\"b\":\"7\"}]"));
    CHECK_EQUAL(bench_postgres_query(f.postgres, "bad sql", done, &f), 0);
    FEED(&f, "E\0\0\0\12Mbad\0\0");
    CHECK_EQUAL(f.calls, 1);
    FEED(&f, "Z\0\0\0\5I");
    CHECK_EQUAL(f.calls, 2);
    CHECK(!strcmp(f.error, "bad"));
    CHECK(!bench_postgres_broken(f.postgres));
    CHECK_EQUAL(bench_postgres_query(f.postgres, "select after error", done, &f), 0);
    FEED(&f, "Z\0\0\0\5I");
    CHECK_EQUAL(f.calls, 3);
    CHECK(!strcmp(f.json, "[]"));
    CHECK(!f.error[0]);
    cleanup(&f);
}
static void test_blackhole_and_eof(void) {
    fixture f = connect_fake(25);
    CHECK_EQUAL(bench_postgres_query(f.postgres, "select never", done, &f), 0);
    double end = bench_loop_now() + 1000;
    while (!f.calls && bench_loop_now() < end)
        bench_loop_poll(f.loop, end);
    CHECK_EQUAL(f.calls, 1);
    CHECK(strstr(f.error, "timed out") != NULL);
    CHECK(bench_postgres_broken(f.postgres));
    cleanup(&f);
    f = connect_fake(500);
    CHECK_EQUAL(bench_postgres_query(f.postgres, "select truncated", done, &f), 0);
    FEED(&f, "R\0\0\0\10\0\0\0\0Z\0\0\0\5I"
             "D\0\0");
    shutdown(f.server, SHUT_RDWR);
    end = bench_loop_now() + 1000;
    while (!f.calls && bench_loop_now() < end)
        bench_loop_poll(f.loop, end);
    CHECK_EQUAL(f.calls, 1);
    CHECK(strstr(f.error, "closed") != NULL);
    CHECK(bench_postgres_broken(f.postgres));
    cleanup(&f);
}
int main(void) {
    RUN(test_fragmented_results_and_sql_error);
    RUN(test_blackhole_and_eof);
    TEST_MAIN_END();
}
