/*
 * linked_list_v2.h
 * ---------------------------------------------------------------------------
 * Version 2 of the doubly linked list: v1 semantics + an auxiliary O(1)
 * amortized key index.
 *
 * Primary structure is UNCHANGED from v1: a sentinel-based doubly linked
 * list. Ordering, iteration, stable sort and ownership rules are exactly the
 * v1 contract:
 *   - push/insert : value becomes owned by the list
 *   - pop/remove_*: value returned, ownership transferred to caller, the
 *                   registered destructor is NOT called
 *   - clear/destroy: remaining values destroyed via the registered fn
 *   - keys are non-unique; find/index_of remove the FIRST match in LIST order
 *
 * v2 adds nothing to the public operation contract — it makes the key-based
 * operations fast by maintaining a secondary index (a salted open-addressing
 * hash multimap, layout H1 + linear probing, selected on measured data):
 *   - find / remove_by_key : O(1) amortized (was O(n))
 *   - count_of_key         : O(1) (was O(n))
 *   - remove_all_by_key    : O(group) instead of O(n)
 * List-order semantics are preserved exactly: each key's members are chained
 * in list order via the single 8-byte per-node index field (n->idx), and the
 * chain head is precisely the node a v1-style forward scan would find.
 *
 * The index internals (table arrays, salt) live inside the opaque llist_t;
 * white-box diagnostics are exposed below for tests/fuzzers only.
 *
 * Build: Compiles clean with cc -std=c11 -O2 -Wall -Wextra -pedantic
 * ---------------------------------------------------------------------------
 */
#ifndef LINKED_LIST_V2_H
#define LINKED_LIST_V2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int64_t ll_key_t;                       /* key type: an integer      */
typedef void (*ll_free_value_fn)(void *value);  /* per-list value destructor */

/* ------------------------------------------------------------------------ */
/* Node                                                                      */
/* ------------------------------------------------------------------------ */

/* `idx` is the single 8-byte per-node index field:
 * the next same-key member in LIST order (NULL at the group tail). It is an
 * implementation detail — treat it as read-only and never assign to it; all
 * maintenance happens inside the list library. */
typedef struct ll_node {
    ll_key_t         key;
    void            *value;      /* caller-allocated complex object          */
    struct ll_node  *prev;       /* list links (sentinel-based, v1 contract) */
    struct ll_node  *next;
    struct ll_node  *idx;        /* KEY INDEX: next same-key member in list  */
} ll_node_t;

/* Node accessors (node pointers are only stable until the next mutating call). */
static inline ll_key_t     ll_node_key(const ll_node_t *n)   { return n->key; }
static inline void        *ll_node_value(const ll_node_t *n) { return n->value; }

/* ------------------------------------------------------------------------ */
/* List (opaque)                                                             */
/* ------------------------------------------------------------------------ */

typedef struct llist llist_t;

/* Lifecycle. free_value may be NULL if the caller manages values itself
 * (in which case clear/destroy do NOT touch the values). OOM: create fails,
 * nothing else in this API can fail silently — mutating ops either fully
 * apply or leave the list untouched (same observable contract as v1). */
llist_t *llist_create(ll_free_value_fn free_value);

/* ------------------------------------------------------------------------
 * Arena (pooled-node) mode.
 *
 * Same API and semantics as llist_create(), but nodes are served from an
 * appended slab pool owned by the list instead of per-node malloc/free:
 *   - slabs grow geometrically by APPENDING a new block; earlier blocks (and
 *     every live node pointer) never move, so nothing needs revalidating;
 *   - freed nodes return to a per-list freelist and are reused in place;
 *   - clear() still runs the registered value destructor on every remaining
 *     value exactly once (value ownership is unchanged), but its node part is
 *     O(1) pool bookkeeping — no allocator traffic;
 *   - destroy() frees whole slabs: one free per slab, not per node.
 *
 * If a slab growth fails the mutating op returns -1 with the list and index
 * left exactly as found (same failure-atomicity as llist_create). Diagnostics:
 * llist_grow_ok(). `initial_capacity` is the first slab's node count (min 8);
 * it only tunes allocation granularity.
 * ------------------------------------------------------------------------ */
llist_t *llist_create_arena(size_t initial_capacity, ll_free_value_fn free_value);
int      llist_grow_ok(const llist_t *l);   /* last arena grow: 0 ok, -1 OOM (malloc lists: always 0) */

/* ------------------------------------------------------------------------
 * Bulk window.
 *
 * Inside an open window (llist_begin_bulk ... llist_end_bulk) structural ops
 * pay exactly what v1 pays and skip the index bookkeeping (one predictable
 * guard branch); key-lookup ops take the O(n) scan fallback so every op stays
 * correct. The pre-window table is left untouched while the window is open.
 *
 * llist_end_bulk() commits: one full teardown + rebuild of the index sized
 * from the final element count — a single O(n) walk-and-probe pass; transient
 * memory = old + new table. On allocation failure it returns -1 and retries
 * with identical conditions (window stays open, pre-window table intact).
 * A window closed without mutations commits in O(1). clear() and destroy()
 * close any open window. No nesting: begin on an already-open window = -1.
 *
 * Diagnostics (llist_index_* and llist_index_verify) describe the last
 * committed table while a window is open — run them after llist_end_bulk().
 * ------------------------------------------------------------------------ */
int  llist_begin_bulk(llist_t *l);
int  llist_end_bulk(llist_t *l);    /* 0 = committed; -1 = OOM (retry) or no window open */
bool llist_bulk_active(const llist_t *l);

/* Lifecycle. free_value may be NULL if the caller manages values itself
 * (in which case clear/destroy do NOT touch the values). OOM: create fails,
 * nothing else in this API can fail silently — mutating ops either fully
 * apply or leave the list untouched (same observable contract as v1).
 */
void     llist_destroy(llist_t *l);

/* Test-only hook (unit tests / fuzzers): make the next `n` internal index/
 * pool allocations each fail once, to exercise failure-atomicity and the
 * retryable bulk-commit path. Returns the previous pending count. Not for
 * production use. */
size_t   llist_test_inject_oom(size_t n);

/* Basic state. O(1). */
size_t   llist_count(const llist_t *l);
bool     llist_is_empty(const llist_t *l);
ll_node_t *llist_front(const llist_t *l);
ll_node_t *llist_back(const llist_t *l);

/* Adding. 0 on success, -1 on allocation failure or (insert_at) out-of-range
 * index; on failure the list is left untouched. */
int      llist_push_front(llist_t *l, ll_key_t key, void *value);
int      llist_push_back (llist_t *l, ll_key_t key, void *value);
/* Insert at 0-based index `index` (0 = head, count = append at end). The new
 * element takes that position; the old occupant shifts back. O(n/2) walk +
 * O(rank) index splice, as in v1. */
int      llist_insert_at(llist_t *l, size_t index, ll_key_t key, void *value);

/* Removing. Return the removed value pointer (ownership now belongs to the
 * caller — the destructor is NOT called) or NULL if nothing was removed. */
void    *llist_pop_front(llist_t *l);
void    *llist_pop_back (llist_t *l);
void    *llist_remove_at(llist_t *l, size_t index);
/* FIRST element whose key matches: O(1) amortized via the index (v1 was
 * O(n)). Returns its value or NULL if absent. */
void    *llist_remove_by_key(llist_t *l, ll_key_t key);
/* Remove ALL elements whose key matches; returns how many were removed.
 * Their values are destroyed by the list (none can be handed back). O(group). */
size_t   llist_remove_all_by_key(llist_t *l, ll_key_t key);

/* ------------------------------------------------------------------------ */
/* Iteration                                                                 */
/* ------------------------------------------------------------------------ */

typedef int (*ll_foreach_fn)(ll_node_t *node, void *user);

/* Visit every node front-to-back; callback returns non-zero to stop early.
 * Do not mutate the list inside the callback. Returns the number of nodes
 * fully processed. O(n). */
size_t   llist_foreach(llist_t *l, ll_foreach_fn fn, void *user);

/* ------------------------------------------------------------------------ */
/* Searching                                                                 */
/* ------------------------------------------------------------------------ */

/* First node whose key matches, or NULL: O(1) amortized via the index (v1
 * was O(n)). List-order semantics unchanged from v1. */
ll_node_t *llist_find(const llist_t *l, ll_key_t key);

/* Node at 0-based index, or NULL. Closer-end walk, <= n/2 hops (as in v1). */
ll_node_t *llist_at(const llist_t *l, size_t index);

/* 0-based index of the first element whose key matches, or -1 if absent.
 * Requires a list walk by nature (returns a POSITION), same cost as v1. */
long long llist_index_of(const llist_t *l, ll_key_t key);

/* True iff at least one element has `key`. Same cost class as llist_find()
 * (O(1) amortized via the index; O(n) scan inside an open bulk window). */
bool     llist_contains(const llist_t *l, ll_key_t key);

/* Number of elements whose key matches: O(1) via the index (v1 was O(n));
 * O(n) scan inside an open bulk window. */
size_t    llist_count_of_key(const llist_t *l, ll_key_t key);

/* Visit every element whose key equals `key`, in LIST order (exactly the
 * order a forward scan would yield), passing its node to fn. If fn returns
 * true, iteration stops early; the returned count excludes the stopped-at
 * element (llist_foreach contract). O(1) + O(group) via the index; O(n) scan
 * inside an open bulk window. Do not mutate the list inside the callback. */
size_t   llist_foreach_key(const llist_t *l, ll_key_t key, ll_foreach_fn fn, void *user);

/* ------------------------------------------------------------------------ */
/* Clearing / sorting                                                        */
/* ------------------------------------------------------------------------ */

/* Remove every element, invoking the destructor on each value. The list is
 * left empty and ready for further use. O(n + index capacity) (the table is
 * kept and zeroed, so clear never reallocates). */
void     llist_clear(llist_t *l);

/* Comparator signature: return <0, 0 or >0 (like strcmp). Must define a weak
 * total order. Ties keep their original relative order (stable sort). */
typedef int (*ll_compare_fn)(const ll_node_t *a, const ll_node_t *b);

/* Default comparator: ascending key. */
static inline int ll_cmp_keys(const ll_node_t *a, const ll_node_t *b)
{
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    return 0;
}

/* Stable bottom-up merge sort over the list's real nodes: O(n log n), O(1)
 * auxiliary memory for the re-linking itself (the key index is then rebuilt
 * in O(n) with one temporary array; on temp-array OOM a correct slower
 * fallback path runs — the sort never fails). */
/* NOTE on index cost of sorting:
 * the sort is stable and each idx chain only encodes its key group's member
 * order, so any comparator that treats same-key members as equal leaves every
 * chain valid. llist_sort_by_key() therefore performs ZERO index work after
 * the merge (the v1-parity line). llist_sort(l, cmp) with an arbitrary user
 * comparator can reorder same-key members and re-establishes the index in one
 * O(n) pass afterward. The fuzzer checks both paths against ground truth. */
void     llist_sort(llist_t *l, ll_compare_fn cmp);

/* Convenience: sort ascending by key (stable); zero index work (see note). */
void     llist_sort_by_key(llist_t *l);

/* ------------------------------------------------------------------------ */
/* White-box index diagnostics (tests / fuzzers / benches only; not part of  */
/* the operational contract)                                                 */
/* ------------------------------------------------------------------------ */

uint64_t llist_index_salt(const llist_t *l);        /* effective per-list salt */
size_t   llist_index_capacity(const llist_t *l);    /* table size (power of 2) */
size_t   llist_index_distinct(const llist_t *l);    /* live distinct keys      */
size_t   llist_index_rehashes(const llist_t *l);
size_t   llist_index_tombstones(const llist_t *l);    /* IX_TOMB slots         */

/* Full index invariant sweep: slot/flag consistency, chain lengths vs
 * per-slot counts, every indexed node reachable by probing its own key,
 * counters consistent. Returns 0 if consistent, nonzero fault code otherwise.
 * Debug cost — not for hot paths. */
long     llist_index_verify(const llist_t *l);

#endif /* LINKED_LIST_V2_H */
