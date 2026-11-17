/* ======================================================================
 * Simple To-Do List over linked list v2 (console demo application)
 * ----------------------------------------------------------------------
 * Same spirit as the v1 sample main, but built ONLY on the public v2 API
 * (linked_list_v2.h):
 *
 *   - llist_create_arena(): P2 slab-pool node allocator (the list owns and
 *     frees every todo_item_t through the registered destructor).
 *   - Keyed ops (find / count / remove_by_key) run in O(1) average via the
 *     auxiliary key index instead of v1's O(n) scans.
 *   - BULK ADD options (7 and 8) fill a user-given id range with "Task N"
 *     entries inside one llist_begin_bulk()/llist_end_bulk() window: every
 *     add skips index bookkeeping and the whole range commits in a single
 *     O(n) rebuild. Ranges are validated: numeric only, ids >= 1, last >=
 *     first, span <= BULK_MAX_SPAN.
 *   - Two sort orders: llist_sort_by_key() (zero index work — the v1-parity
 *     line) and llist_sort(l, cmp) with a custom comparator (rebuilds the
 *     index in one O(n) pass afterward).
 *
 * This file is a consumer of the library; it does not modify or depend on
 * any library-internal symbol. Build: make app   ->  ./ll_app
 * ====================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "linked_list_v2.h"

/* Max elements accepted per bulk range add (sanity bound on one user input). */
#define BULK_MAX_SPAN 100000LL

/* --------------------------------------------------------------------- *
 * Value object + presentation (v1-parity style)
 * --------------------------------------------------------------------- */
typedef struct {
    char title[64];
    int  priority;   /* 1 (low) .. 5 (urgent) */
    bool done;
} todo_item_t;

static void todo_free(void *v)          /* list destructor: owns the item */
{
    free(v);
}

static const char *prio_str(int p)
{
    switch (p) {
    case 1:  return "LOW";
    case 2:  return "NORMAL-LO";
    case 3:  return "NORMAL";
    case 4:  return "HIGH";
    default: return "URGENT";
    }
}

static void print_todo(ll_key_t key, const todo_item_t *t)
{
    printf("  [%-6lld] %-8s %s%s\n", (long long)key, prio_str(t->priority),
           t->done ? "[x] " : "[ ] ", t->title);
}

static int print_one(ll_node_t *node, void *user)
{
    (void)user;
    print_todo(ll_node_key(node), (const todo_item_t *)ll_node_value(node));
    return 0;
}

/* --------------------------------------------------------------------- *
 * Input helpers: line reading + strictly validated integers
 * --------------------------------------------------------------------- */
static int g_eof = 0;                   /* set when stdin runs out          */

static void read_line(char *buf, size_t sz, const char *prompt)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(buf, (int)sz, stdin)) { buf[0] = '\0'; g_eof = 1; return; }
    buf[strcspn(buf, "\r\n")] = '\0';
}

/* Strict non-negative decimal parse: digits only, no sign, no trailing
 * junk, bounded well below INT64_MAX. Rejected: "", "0x10", "-5", "3a",
 * "1.5", "+2", empty/EOF (g_eof is checked by the caller). */
static int parse_ll(const char *s, long long *out)
{
    if (s[0] < '0' || s[0] > '9') return 0;
    unsigned long long v = 0;
    size_t i = 0;
    while (s[i] >= '0' && s[i] <= '9') {
        v = v * 10ull + (unsigned long long)(s[i] - '0');
        if (v > 1000000000000000000ull) return 0;      /* absurd-id guard   */
        i++;
    }
    if (s[i] != '\0') return 0;
    *out = (long long)v;
    return 1;
}

/* Prompt until a valid id (>= min_id) arrives. Returns the value, or -1 on
 * EOF so callers can cancel cleanly instead of looping forever. */
static long long read_id(const char *prompt, long long min_id, const char *low_msg)
{
    char buf[64];
    for (;;) {
        read_line(buf, sizeof buf, prompt);
        if (g_eof) return -1;
        long long v;
        if (!parse_ll(buf, &v)) { printf("  Not a valid number (digits only).\n"); continue; }
        if (v < min_id)         { printf("  %s\n", low_msg);                     continue; }
        return v;
    }
}

/* Prompt until a valid priority in [1..5] (empty line -> def). Returns -1
 * on EOF. */
static int read_priority(const char *prompt, int def)
{
    char buf[32];
    for (;;) {
        read_line(buf, sizeof buf, prompt);
        if (g_eof) return -1;
        if (buf[0] == '\0') return def;
        long long v;
        if (!parse_ll(buf, &v) || v < 1 || v > 5) {
            printf("  Enter a number between 1 and 5.\n");
            continue;
        }
        return (int)v;
    }
}

/* --------------------------------------------------------------------- *
 * Menu actions
 * --------------------------------------------------------------------- */
static int menu_add(llist_t *l)
{
    char buf[64];                    /* same size as title: no truncation   */
    long long id = read_id("  Id (integer >= 1): ", 1, "Id must be at least 1.");
    if (id < 0) { printf("  Cancelled.\n"); return 0; }

    for (;;) {
        if (llist_find(l, (ll_key_t)id)) {
            printf("  Id %lld already exists - pick another.\n", id);
            id = read_id("  New id: ", 1, "Id must be at least 1.");
            if (id < 0) { printf("  Cancelled.\n"); return 0; }
        } else break;
    }

    read_line(buf, sizeof buf, "  Title: ");
    if (g_eof || buf[0] == '\0') { printf("  Empty title - cancelled.\n"); return 0; }

    int prio = read_priority("  Priority [1..5, default 3]: ", 3);
    if (prio < 0) { printf("  Cancelled.\n"); return 0; }

    todo_item_t *t = malloc(sizeof *t);
    if (!t) { printf("  Out of memory.\n"); return 0; }
    memset(t, 0, sizeof *t);
    snprintf(t->title, sizeof t->title, "%s", buf);
    t->priority = prio;

    if (llist_push_back(l, (ll_key_t)id, t) != 0) {
        free(t);
        printf("  Out of memory.\n");
        return 0;
    }
    printf("  Added to-do %lld \"%s\" (%s).\n", id, t->title, prio_str(prio));
    /* No free here: on success the list owns the item and todo_free will
     * release it when the node is removed / cleared / destroyed. */
    return 0;
}

static int menu_list(llist_t *l)
{
    printf("\n-- To-dos (in list order, %zu total) ------------------------\n", llist_count(l));
    if (llist_is_empty(l)) {
        printf("  (no to-dos)\n");
        return 0;
    }
    llist_foreach(l, print_one, NULL);
    return 0;
}

/* Priority-aware comparator for custom sorting: higher priority first,
 * uncompleted before completed, then earlier id. */
static int cmp_todo_priority(const ll_node_t *a, const ll_node_t *b)
{
    const todo_item_t *ta = (const todo_item_t *)ll_node_value(a);
    const todo_item_t *tb = (const todo_item_t *)ll_node_value(b);
    if (ta->priority != tb->priority) return ta->priority < tb->priority ? 1 : -1;
    if (ta->done != tb->done)         return ta->done ? 1 : -1;
    return ll_cmp_keys(a, b);
}

static int menu_sort(llist_t *l)
{
    char buf[32];
    read_line(buf, sizeof buf, "  Sort by [1=key, 2=priority] (default 1): ");
    if (g_eof || buf[0] == '\0' || buf[0] == '1') {
        /* Zero index work: sorting by key alone preserves every group's
         * member order, so the v1-parity line applies. */
        llist_sort_by_key(l);
        printf("  Sorted by id.\n");
    } else if (buf[0] == '2') {
        /* Arbitrary comparator: one O(n) index rebuild follows the sort. */
        llist_sort(l, cmp_todo_priority);
        printf("  Sorted by priority (urgent first, then open before done, then id).\n");
    } else {
        printf("  Unknown choice - left unsorted.\n");
        return 0;
    }
    menu_list(l);
    return 0;
}

static int menu_find(llist_t *l)
{
    long long id = read_id("  Id to search for: ", 1, "Id must be at least 1.");
    if (id < 0) { printf("  Cancelled.\n"); return 0; }

    ll_node_t *n = llist_find(l, (ll_key_t)id);        /* O(1) via index    */
    if (!n) {
        printf("  No to-do with id %lld.\n", id);
        return 0;
    }
    print_todo(ll_node_key(n), (const todo_item_t *)ll_node_value(n));
    long long pos = llist_index_of(l, (ll_key_t)id);
    printf("  Found at position %lld of %zu.\n", pos, llist_count(l));
    return 0;
}

static int menu_toggle(llist_t *l)
{
    long long id = read_id("  Id to complete/uncomplete: ", 1, "Id must be at least 1.");
    if (id < 0) { printf("  Cancelled.\n"); return 0; }

    ll_node_t *n = llist_find(l, (ll_key_t)id);
    if (!n) {
        printf("  No to-do with id %lld.\n", id);
        return 0;
    }
    todo_item_t *t = (todo_item_t *)ll_node_value(n);
    t->done = !t->done;
    printf("  To-do %lld is now %s.\n", id, t->done ? "DONE" : "open");
    return 0;
}

static int menu_remove(llist_t *l)
{
    long long id = read_id("  Id to delete: ", 1, "Id must be at least 1.");
    if (id < 0) { printf("  Cancelled.\n"); return 0; }

    /* remove_by_key returns ownership of the value -> free it. */
    todo_item_t *t = (todo_item_t *)llist_remove_by_key(l, (ll_key_t)id);
    if (!t) {
        printf("  No to-do with id %lld.\n", id);
        return 0;
    }
    printf("  Deleted to-do %lld: \"%s\"\n", id, t->title);
    free(t);
    return 0;
}

/* Shared bulk-add engine.
 * Pass 1 (window closed): count ids in [lo..hi] that already exist — O(1)
 * per id via the index. Pass 2: open ONE bulk window, push_back every new
 * id (each add skips index bookkeeping), then commit with a single O(n)
 * rebuild at end_bulk. */
static int bulk_add_range(llist_t *l, long long lo, long long hi, int cycle_n, int fixed_prio)
{
    size_t span = (size_t)(hi - lo + 1);

    size_t skipped = 0;
    for (long long id = lo; id <= hi; id++)
        if (llist_find(l, (ll_key_t)id)) skipped++;

    int in_win = (llist_begin_bulk(l) == 0);

    size_t added = 0;
    int failed_at = 0;
    long long fail_id = -1;
    for (long long id = lo; !failed_at && id <= hi; id++) {
        if (llist_find(l, (ll_key_t)id)) continue;     /* exists: pass-2 recheck */
        int prio = cycle_n ? ((int)((id - lo) % (long long)cycle_n) + 1) : fixed_prio;
        todo_item_t *t = malloc(sizeof *t);
        if (!t) { failed_at = 1; fail_id = id; break; }
        memset(t, 0, sizeof *t);
        snprintf(t->title, sizeof t->title, "Task %lld", id);
        t->priority = prio;
        if (llist_push_back(l, (ll_key_t)id, t) != 0) {
            free(t);
            failed_at = 1; fail_id = id;
            break;
        }
        added++;
    }

    int commit = in_win ? llist_end_bulk(l) : 0;

    if (failed_at)
        printf("  Bulk add stopped at id %lld (out of memory); %zu of %zu committed.",
               fail_id, added + skipped, span);
    else if (commit != 0) {
        /* OOM at commit: the pre-window table is intact and retryable — one
         * immediate retry, then warn. The list itself is always consistent. */
        if (llist_end_bulk(l) != 0)
            printf("  WARNING: bulk commit failed twice (out of memory).");
    }

    printf("\n  Bulk add %lld..%lld: added %zu%s, skipped %zu existing, total now %zu.\n",
           lo, hi, added, failed_at ? " (partial)" : "", skipped, llist_count(l));
    if (!failed_at && span <= 20) {              /* small ranges: show them */
        for (long long id = lo; id <= hi; id++) {
            ll_node_t *n = llist_find(l, (ll_key_t)id);
            if (n) print_todo(ll_node_key(n), (const todo_item_t *)ll_node_value(n));
        }
    }
    return 0;
}

/* Bulk add with one user-chosen priority for the whole range. */
static int menu_bulk_single(llist_t *l)
{
    printf("\n-- Bulk add: Task <first>..Task <last>, one priority ----------\n");
    long long lo = read_id("  First id (>= 1): ", 1, "First id must be at least 1.");
    if (lo < 0) { printf("  Cancelled.\n"); return 0; }
    long long hi = read_id("  Last id  (>= first): ", lo, "Last id must be >= first id.");
    if (hi < 0) { printf("  Cancelled.\n"); return 0; }
    if (hi - lo + 1 > BULK_MAX_SPAN) {
        printf("  Range too large: max %lld per bulk add (asked %lld).\n",
               BULK_MAX_SPAN, hi - lo + 1);
        return 0;
    }
    int prio = read_priority("  Priority [1..5, default 3]: ", 3);
    if (prio < 0) { printf("  Cancelled.\n"); return 0; }
    bulk_add_range(l, lo, hi, 0, prio);
    return 0;
}

/* Bulk add with priorities cycling 1..N across the range. */
static int menu_bulk_cycle(llist_t *l)
{
    printf("\n-- Bulk add: Task <first>..Task <last>, priorities cycle 1..N --\n");
    long long lo = read_id("  First id (>= 1): ", 1, "First id must be at least 1.");
    if (lo < 0) { printf("  Cancelled.\n"); return 0; }
    long long hi = read_id("  Last id  (>= first): ", lo, "Last id must be >= first id.");
    if (hi < 0) { printf("  Cancelled.\n"); return 0; }
    if (hi - lo + 1 > BULK_MAX_SPAN) {
        printf("  Range too large: max %lld per bulk add (asked %lld).\n",
               BULK_MAX_SPAN, hi - lo + 1);
        return 0;
    }
    char buf[32];
    int n = -1;
    for (;;) {
        read_line(buf, sizeof buf, "  Cycle length N [2..5]: ");
        if (g_eof) break;
        long long v;
        if (!parse_ll(buf, &v) || v < 2 || v > 5) {
            printf("  Enter a number between 2 and 5.\n");
            continue;
        }
        n = (int)v;
        break;
    }
    if (n < 0) { printf("  Cancelled.\n"); return 0; }
    bulk_add_range(l, lo, hi, n, 0);
    return 0;
}

static int menu_clear(llist_t *l)
{
    char buf[8];
    read_line(buf, sizeof buf, "  Clear ALL to-dos? [y/N]: ");
    if (buf[0] != 'y' && buf[0] != 'Y') { printf("  Cancelled.\n"); return 0; }
    size_t n = llist_count(l);
    llist_clear(l);   /* closes any open window and frees items via todo_free */
    printf("  Removed %zu to-do(s).\n", n);
    return 0;
}

typedef struct { size_t open, done; size_t by_prio[6]; } stats_ctx_t;

static int count_one(ll_node_t *node, void *user)
{
    stats_ctx_t *ctx = (stats_ctx_t *)user;
    const todo_item_t *t = (const todo_item_t *)ll_node_value(node);
    if (t->done) ctx->done++; else ctx->open++;
    if (t->priority >= 1 && t->priority <= 5) ctx->by_prio[t->priority]++;
    return 0;
}

static int menu_stats(llist_t *l)
{
    stats_ctx_t ctx = {0, 0, {0}};
    llist_foreach(l, count_one, &ctx);
    printf("\n  total=%zu  open=%zu  done=%zu\n", llist_count(l), ctx.open, ctx.done);
    for (int p = 5; p >= 1; p--)
        if (ctx.by_prio[p]) printf("  %-8s %6zu\n", prio_str(p), ctx.by_prio[p]);
    return 0;
}

/* --------------------------------------------------------------------- *
 * main: menu loop, v1-style
 * --------------------------------------------------------------------- */
int main(void)
{
    /* Arena slab pool (P2): nodes are recycled geometrically instead of one
     * malloc/free per node. The list owns all values via todo_free. */
    llist_t *todos = llist_create_arena(16, todo_free);
    if (!todos) {
        fprintf(stderr, "Failed to allocate list.\n");
        return 1;
    }

    printf("=============================================================\n");
    printf("  Simple To-Do List (linked list v2: key index + arena pool)\n");
    printf("  keyed ops are O(1) average; bulk adds commit in one window\n");
    printf("=============================================================\n");

    /* Seed items so the menu has something to show (heap-allocated: the
     * list owns them and frees them via todo_free). */
    {
        static const struct { ll_key_t id; const char *title; int prio; int done; } seed[] = {
            { 1, "Write unit tests",      4, 0 },
            { 2, "Review pull request",   3, 0 },
            { 3, "Buy milk",              5, 1 },
        };
        for (size_t i = 0; i < sizeof seed / sizeof *seed; i++) {
            todo_item_t *t = malloc(sizeof *t);
            if (!t) { fprintf(stderr, "Failed to seed demo data.\n"); return 1; }
            memset(t, 0, sizeof *t);
            snprintf(t->title, sizeof t->title, "%s", seed[i].title);
            t->priority = seed[i].prio;
            t->done = (bool)seed[i].done;
            if (llist_push_back(todos, seed[i].id, t) != 0) {
                free(t);
                fprintf(stderr, "Failed to seed demo data.\n");
                return 1;
            }
        }
    }

    char line[16];
    for (;;) {
        printf(
            "\n  1. Add to-do\n"
            "  2. List all\n"
            "  3. Sort (key / priority)\n"
            "  4. Find by id\n"
            "  5. Toggle done\n"
            "  6. Delete by id\n"
            "  7. Bulk add range, one priority\n"
            "  8. Bulk add range, priorities cycle 1..N\n"
            "  9. Clear all\n"
            " 10. Statistics\n"
            " 11. Close (exit)\n"
            "> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) break;   /* EOF -> exit clean */
        line[strcspn(line, "\r\n")] = '\0';

        /* Whole-line validation: exactly an integer 1..11, nothing else —
         * so "99", "2x" etc. cannot silently dispatch on their first digit
         * the way a single-char switch could. */
        long long choice;
        if (!parse_ll(line, &choice) || choice < 1 || choice > 11) {
            printf(" Unknown option (choose 1..11).\n");
            continue;
        }

        switch ((int)choice) {
        case 1:  menu_add(todos);         break;
        case 2:  menu_list(todos);        break;
        case 3:  menu_sort(todos);        break;
        case 4:  menu_find(todos);        break;
        case 5:  menu_toggle(todos);      break;
        case 6:  menu_remove(todos);      break;
        case 7:  menu_bulk_single(todos); break;
        case 8:  menu_bulk_cycle(todos);  break;
        case 9:  menu_clear(todos);       break;
        case 10: menu_stats(todos);       break;
        case 11: goto done;
        }
    }

done:
    llist_destroy(todos);   /* frees list, pool and any remaining values */
    printf("\nGoodbye!\n");
    return 0;
}
