/*
 * linked_list_v2.c
 * ---------------------------------------------------------------------------
 * Version 2 implementation: v1's sentinel doubly linked list and
 * the selected key index:
 *
 *   Hash multimap, LINEAR probing (the measured winner over
 *   remixed-probe variant).
 *
 * Index design:
 *   - h = splitmix64(key ^ salt); index = h & (cap-1); cap a power of two,
 *     starts at 8, doubles when live distinct keys exceed 0.7*cap, never
 *     shrinks. Checked arithmetic; every table mutation is atomic under
 *     allocation failure (old table stays fully intact).
 *   - salt: per-list, mixed into the hash INPUT (a post-hoc XOR would only
 *     relabel buckets at a fixed width and is provably useless against
 *     low-bit collision). Production default derives
 *     it from pointer entropy (no OS entropy APIs, portability); tests
 *     can read it back via llist_index_salt().
 *   - slot state lives in a separate 1-byte flag array (empty / occupied /
 *     tombstone): keys span the full int64 range so no sentinel key value is
 *     safe. Deletion leaves tombstones so probe chains survive; slots are
 *     reused on the next insert that passes through them.
 *   - slots store (key, group head, member count) in parallel arrays;
 *     per-node cost is exactly one 8-byte field: n->idx = next same-key
 *     member in LIST order. The group head is the FIRST list-occurrence —
 *     exactly what a v1 forward scan would return — which is what keeps
 *     find/remove_by_key semantics identical to v1.
 *   - measured guardrail: low-bit-collision keys degrade
 *     find-hit by at most 1.5x vs uniform random (bar: <=10x). Worst case
 *     under an exposed-salt crafted attack scales linearly with cluster
 *     depth (open-addressing property); the chain-overflow fallback is
 *     pre-committed for seed-exposed deployments and was not triggered.
 *
 * Ownership / observable behaviour: byte-for-byte the v1 contract (see the
 * header). Mutating ops are failure-atomic: the index is updated first; on
 * any allocation failure the freshly allocated node is freed and the list is
 * left untouched, returning the same -1/NULL v1 would for an OOM.
 *
 * v2 Extensions:
 *   - arena mode (llist_create_arena): pooled nodes; geometric slab growth by
 *     APPENDING blocks only (live pointers never move); per-list freelist;
 *     clear's node part is O(1) pool bookkeeping while value destructors still
 *     run exactly once each; overflow -> -1 with the list left untouched.
 *   - bulk window (llist_begin_bulk/llist_end_bulk): inside a window structural
 *     ops skip index work (one predictable guard branch) and key lookups take
 *     O(n) scan fallbacks, so every op stays v1-correct; end_bulk commits with
 *     one O(n) walk-and-probe pass into a fresh table sized from the final n,
 *     transient memory = old + new table, retryable on allocation failure.
 *
 * Cost notes:
 *   push_front / find / remove_by_key : O(1) amortized
 *   push_back                         : O(group) worst case
 *   pop_back / remove_at              : O(n/2) list walk + O(rank) chain walk
 *   insert_at                         : O(n/2) walk + O(rank) chain splice
 *   sort                              : v1's merge sort + O(n) index rebuild
 *
 * Compiles clean with: cc -std=c11 -O2 -Wall -Wextra -pedantic
 * ---------------------------------------------------------------------------
 */

#include "linked_list_v2.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ========================================================================== */
/* List + key-index layout                                                    */
/* ========================================================================== */

/* slot flags: separate array on purpose. */
enum { IX_EMPTY = 0, IX_OCC = 1, IX_TOMB = 2 };

/* Arena slab descriptor: a block of N contiguous ll_node_t. Slabs are
 * only ever APPENDED — earlier blocks (and live node pointers) never move. */
struct ll2_slab {
    void   *base;
    size_t  nodes;               /* node slots in this slab                  */
};

struct llist {
    ll_node_t        head;       /* sentinels: no data stored in them (v1)   */
    ll_node_t        tail;
    size_t           count;
    ll_free_value_fn free_value; /* NULL => values are not destroyed by list */

    /* --- key index (linear probing), all [cap]-sized -------------- */
    ll_key_t        *keys;       /* slot keys (valid iff IX_OCC)             */
    ll_node_t      **heads;      /* group head = FIRST list-occurrence       */
    uint32_t        *counts;     /* live members per OCC slot                */
    uint8_t         *flags;      /* IX_EMPTY / IX_OCC / IX_TOMB              */
    size_t           cap;        /* power of two, starts at 8                */
    size_t           distinct;   /* live IX_OCC slots                        */
    size_t           rehashes;
    uint64_t         salt;       /* mixed into the hash input                */

    /* --- P2 arena pool (all [0/NULL] in malloc-mode lists) --------------- */
    int              arena;      /* 0 = per-node malloc/free, 1 = slab pool  */
    int              last_grow;  /* last slab grow: 0 ok, -1 OOM             */
    struct ll2_slab *slabs;      /* descriptor array (heap-grown)            */
    size_t           nslabs;
    size_t           slabs_cap;
    void            *bump;       /* bump pointer inside the last slab        */
    size_t           bump_left;  /* fresh slots left in the current slab     */
    size_t           slab_next;  /* node count of the next slab (geometric)  */
    ll_node_t       *pool_free;  /* freelist, linked through n->next         */
    size_t           pool_total; /* nodes ever bumped from slabs             */

    /* --- P2.5 bulk window + scratch ------------------------------------- */
    int              bulk_open;  /* structural ops pay v1 cost only          */
    size_t           bulk_muts;  /* structural changes since begin_bulk      */
    size_t           tombs;      /* IX_TOMB slots (compaction trigger)       */
    ll_node_t      **tails_scratch;     /* [cap] rebuild scratch (best-effort)*/
    size_t           tails_scratch_cap;
};

/* -------------------------------------------------------------------------- */
/* Hashing                                   */
/* -------------------------------------------------------------------------- */

static uint64_t ll2_splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static inline uint64_t ll2_hash(const llist_t *l, ll_key_t k)
{
    /* salt mixed into the INPUT (see header note). */
    return ll2_splitmix64((uint64_t)k ^ l->salt);
}

/* -------------------------------------------------------------------------- */
/* Allocations (test-injectable)                                              */
/* The next `ll2_oom_inject` internal allocations fail once each; unit tests  */
/* and the fuzzer use this to exercise failure-atomicity and the retryable    */
/* bulk-commit path. Plain malloc/calloc otherwise — no behavior change.      */
/* -------------------------------------------------------------------------- */

static size_t ll2_oom_inject = 0;

size_t llist_test_inject_oom(size_t n)
{
    size_t old = ll2_oom_inject;
    ll2_oom_inject = n;
    return old;
}

static void *ll2_xmalloc(size_t bytes)
{
    if (ll2_oom_inject > 0) { ll2_oom_inject--; return NULL; }
    return malloc(bytes);
}

static void *ll2_xcalloc(size_t n, size_t el)
{
    if (ll2_oom_inject > 0) { ll2_oom_inject--; return NULL; }
    return calloc(n, el);
}

/* Probe for key k. In insert mode the first tombstone seen is remembered and
 * reused if the key turns out absent (classic linear-probe deletion). */
static size_t ll2_probe(const llist_t *l, ll_key_t k, int inserting, int *existed)
{
    size_t pos = (size_t)(ll2_hash(l, k) & (l->cap - 1));
    size_t first_tomb = SIZE_MAX;
    for (;;) {
        uint8_t f = l->flags[pos];
        if (f == IX_OCC) {
            if (l->keys[pos] == k) { *existed = 1; return pos; }
        } else if (f == IX_EMPTY) {
            break;
        } else if (inserting && first_tomb == SIZE_MAX) {
            first_tomb = pos;      /* remember for reuse                     */
        }
        pos = (pos + 1) % l->cap;
    }
    *existed = 0;
    return (first_tomb != SIZE_MAX) ? first_tomb : pos;
}

/* -------------------------------------------------------------------------- */
/* Table primitives                                                           */
/* -------------------------------------------------------------------------- */

static void ll2_resize_tails_scratch(llist_t *l);   /* best-effort scratch */

/* Rebuild into a fresh `ncap` table, re-deriving every chain in its stored
 * (list) order. Old arrays are freed only after the new ones are fully live
 * — atomic under allocation failure. The probe home is derived from the NEW
 * cap: re-inserting at old-cap homes would place keys past EMPTY slots that
 * lookups stop at. */
static int ll2_rehash(llist_t *l, size_t ncap)
{
    /* Minimum table is the initial cap of 8 (llist_create); same-capacity
     * compaction at that size must be legal, so do not reject below it. */
    if (ncap < 8 || (ncap & (ncap - 1)) != 0 || ncap > SIZE_MAX / 8) return -1;

    ll_key_t   *nk = calloc(ncap, sizeof *nk);
    uint8_t    *nf = calloc(ncap, 1);          /* MUST be zeroed (IX_EMPTY)  */
    ll_node_t **nh = calloc(ncap, sizeof *nh);
    uint32_t   *nc = calloc(ncap, sizeof *nc);
    if (!nk || !nf || !nh || !nc) {
        free(nk); free(nf); free(nh); free(nc);
        return -1;                             /* old table fully intact     */
    }

    for (size_t i = 0; i < l->cap; i++) {
        if (l->flags[i] != IX_OCC || !l->heads[i]) continue;
        ll_node_t *n = l->heads[i];            /* group head, list order     */
        size_t pos = (size_t)(ll2_hash(l, n->key) & (ncap - 1));
        while (nf[pos] == IX_OCC) pos = (pos + 1) % ncap;   /* no tombs yet */
        nk[pos] = n->key;
        nf[pos] = IX_OCC;
        nh[pos] = n;                           /* nodes never move           */
        /* The idx chain already encodes list order, so only slot pointers
         * change here. Recount members from the old chain (do not trust
         * counts[]) and re-assert the tail's NULL link. */
        ll_node_t *tailn = n;
        size_t c = 0;
        for (; n; n = n->idx) { tailn = n; c++; }
        nc[pos]   = (uint32_t)c;
        tailn->idx = NULL;
    }

    free(l->keys); free(l->flags); free(l->heads); free(l->counts);
    l->keys = nk; l->flags = nf; l->heads = nh; l->counts = nc;
    l->cap = ncap;
    l->tombs = 0;                    /* fresh table: no tombstones           */
    ll2_resize_tails_scratch(l);     /* best-effort keep scratch at new cap  */
    l->rehashes++;
    return 0;
}

/* Keep a [cap]-sized tails scratch around for rebuild/commit passes (avoids
 * a per-call calloc of up to 8*cap bytes on the hot path). Best-effort: on
 * OOM the scratch stays NULL and callers fall back to a one-shot calloc. */
static void ll2_resize_tails_scratch(llist_t *l)
{
    if (l->tails_scratch_cap == l->cap) return;
    free(l->tails_scratch);
    l->tails_scratch =
        (l->cap <= SIZE_MAX / sizeof (ll_node_t *))
            ? (ll_node_t **)ll2_xcalloc(l->cap, sizeof (ll_node_t *)) : NULL;
    l->tails_scratch_cap = l->tails_scratch ? l->cap : 0;
}

/* Grow before a NEW distinct key would push live load past 0.7. Returns 0 if
 * the table is ready for one more distinct key (possibly after rehash), -1 on
 * OOM (table untouched). */
static int ll2_maybe_grow(llist_t *l)
{
    if (l->distinct + 1 <= (l->cap * 7) / 10) {
#ifndef LL_V2_NO_COMPACT
        /* P3: tombstone-heavy tables re-probe through dead slots on every op.
         * Past half the table in tombs, pay one same-cap rehash to compact
         * (old chains are re-inserted into a clean table). */
        if (l->tombs > l->cap / 2) return ll2_rehash(l, l->cap);
#endif
        return 0;
    }
    size_t ncap = l->cap * 2;                  /* checked: cap < SIZE_MAX/8  */
    if (ncap < l->cap) return -1;              /* overflow                   */
    return ll2_rehash(l, ncap);
}

/* -------------------------------------------------------------------------- */
/* P2 arena pool: node allocation / release                                   */
/* -------------------------------------------------------------------------- */

/* Append a fresh slab of `ns` nodes. 0 on success (bump ready), -1 on OOM
 * (list and index untouched; last_grow records the failure). Old slabs are
 * never touched, so no live pointer needs revalidating. */
static int ll2_new_slab(llist_t *l, size_t ns)
{
    if (ns < 8) ns = 8;
    void *base = ll2_xcalloc(ns, sizeof (ll_node_t));   /* zeroed: cheap +   */
                                                        /* ASan-defined      */
    if (!base) { l->last_grow = -1; return -1; }

    if (l->nslabs == l->slabs_cap) {
        size_t ncap = l->slabs_cap ? l->slabs_cap * 2 : 4;
        struct ll2_slab *nsd =
            (struct ll2_slab *)realloc(l->slabs, ncap * sizeof *nsd);
        if (!nsd) { free(base); l->last_grow = -1; return -1; }
        l->slabs = nsd;
        l->slabs_cap = ncap;
    }
    l->slabs[l->nslabs].base  = base;
    l->slabs[l->nslabs].nodes = ns;
    l->nslabs++;

    l->bump      = base;
    l->bump_left = ns;
    if (ns < SIZE_MAX / 2) l->slab_next = ns * 2;   /* geometric growth       */
    l->last_grow = 0;
    return 0;
}

/* Allocate one node. Malloc mode: per-node malloc (v1 behavior). Arena mode:
 * freelist first (in-place reuse), then bump from the current slab, growing a
 * new slab when it runs dry. NULL on OOM — the caller treats that exactly as
 * v1's allocation failure (-1, list untouched). */
static ll_node_t *ll2_node_alloc(llist_t *l)
{
    if (!l->arena) {
        ll_node_t *n = ll2_xmalloc(sizeof *n);
        return n;
    }

    ll_node_t *n = l->pool_free;
    if (n) {
        l->pool_free = n->next;                      /* pool link, see free  */
        return n;
    }
    if (l->bump_left == 0 &&
        ll2_new_slab(l, l->slab_next < 8 ? 16 : l->slab_next) != 0)
        return NULL;                                 /* grow failed: OOM     */

    n = (ll_node_t *)l->bump;
    l->bump      = (char *)l->bump + sizeof *n;
    l->bump_left--;
    l->pool_total++;
    return n;
}

/* Release one node back to its mode's owner. Arena nodes go to the freelist
 * (their list links are dead at this point; `next` doubles as the pool link)
 * and their memory stays reserved. Malloc-mode nodes are freed as in v1. */
static void ll2_node_release(llist_t *l, ll_node_t *n)
{
    if (!l->arena) { free(n); return; }
    n->next = l->pool_free;
    l->pool_free = n;
}

/* -------------------------------------------------------------------------- */
/* Index maintenance: insertions                                              */
/* All of these run BEFORE the list splice, so an OOM leaves the list and the
 * index exactly as found (the caller frees the node and returns -1).         */
/* -------------------------------------------------------------------------- */

/* Append/prepend n to key group k's chain. `rank` is only meaningful when the
 * key already exists (see ll2_index_splice_rank); front/back are rank 0 /
 * end fast paths that never walk the existing chain beyond what the mode
 * requires (front: O(1); back: O(group) tail walk). */

static int ll2_index_insert_front(llist_t *l, ll_node_t *n)
{
    /* Bulk window: structural ops skip the index entirely (guard branch);
     * the pre-window table stays untouched until end_bulk() commits. */
    if (l->bulk_open) { l->bulk_muts++; return 0; }

    int existed = 0;
    size_t i = ll2_probe(l, n->key, 1, &existed);

    if (existed) {                             /* prepend: O(1)              */
        n->idx = l->heads[i];
        l->heads[i] = n;
        l->counts[i]++;
        return 0;
    }

    if (ll2_maybe_grow(l) != 0) return -1;     /* atomic: table untouched    */
    i = ll2_probe(l, n->key, 1, &existed);     /* rehash may have moved it   */
    if (l->flags[i] == IX_TOMB) l->tombs--;    /* slot reuse: tomb -> OCC    */
    l->distinct++;
    l->flags[i]  = IX_OCC;
    l->keys[i]   = n->key;
    l->heads[i]  = n;
    n->idx       = NULL;
    l->counts[i] = 1;
    return 0;
}

static int ll2_index_insert_back(llist_t *l, ll_node_t *n)
{
    /* Bulk window: structural ops skip the index entirely (guard branch);
     * the pre-window table stays untouched until end_bulk() commits. */
    if (l->bulk_open) { l->bulk_muts++; return 0; }

    int existed = 0;
    size_t i = ll2_probe(l, n->key, 1, &existed);

    if (existed) {                             /* append: O(group) tail walk */
        ll_node_t *t = l->heads[i];
        while (t->idx) t = t->idx;
        t->idx = n;
        n->idx = NULL;
        l->counts[i]++;
        return 0;
    }

    if (ll2_maybe_grow(l) != 0) return -1;
    i = ll2_probe(l, n->key, 1, &existed);
    if (l->flags[i] == IX_TOMB) l->tombs--;    /* slot reuse: tomb -> OCC */
    l->distinct++;
    l->flags[i]  = IX_OCC;
    l->keys[i]   = n->key;
    l->heads[i]  = n;
    n->idx       = NULL;
    l->counts[i] = 1;
    return 0;
}

/* Middle splice (insert_at): place n as member `rank` of its key group
 * (0-based position among that key's existing members, in list order). When
 * the key is absent the rank is immaterial — a single-member group. */
static int ll2_index_splice_rank(llist_t *l, ll_node_t *n, size_t rank)
{
    if (l->bulk_open) { l->bulk_muts++; return 0; }

    int existed = 0;
    size_t i = ll2_probe(l, n->key, 1, &existed);

    if (!existed) {                            /* new group: any rank        */
        if (ll2_maybe_grow(l) != 0) return -1;
        i = ll2_probe(l, n->key, 1, &existed);
        if (l->flags[i] == IX_TOMB) l->tombs--;    /* slot reuse: tomb -> OCC */
        l->distinct++;
        l->flags[i]  = IX_OCC;
        l->keys[i]   = n->key;
        l->heads[i]  = n;
        n->idx       = NULL;
        l->counts[i] = 1;
        return 0;
    }

    /* Existing group: rank must be in [0, counts[i]] (guaranteed by the list
     * walk that computed it — it counted existing members before `index`). */
    if (rank == 0) {
        n->idx = l->heads[i];
        l->heads[i] = n;
    } else if (rank >= l->counts[i]) {         /* append at group tail       */
        ll_node_t *t = l->heads[i];
        while (t->idx) t = t->idx;
        t->idx = n;
        n->idx = NULL;
    } else {
        ll_node_t *pred = l->heads[i];
        for (size_t r = 1; r < rank; r++) pred = pred->idx;   /* O(rank)      */
        n->idx = pred->idx;
        pred->idx = n;
    }
    l->counts[i]++;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Index maintenance: removals                                                */
/* -------------------------------------------------------------------------- */

/* Remove the FIRST list-occurrence of key k (chain head): O(1). Used by
 * remove_by_key and pop_front (the front node is always its group's head). */
static int ll2_index_remove_head(llist_t *l, ll_key_t k)
{
    if (l->bulk_open) { l->bulk_muts++; return 0; }

    int existed = 0;
    size_t i = ll2_probe(l, k, 0, &existed);
    if (!existed) return -1;

    ll_node_t *h = l->heads[i];
    ll_node_t *nh = h->idx;
    if (nh) {
        l->heads[i] = nh;
        l->counts[i]--;
    } else {
        /* last member: slot becomes a tombstone (probe chains survive).     */
        l->flags[i]  = IX_TOMB;
        l->heads[i]  = NULL;
        l->counts[i] = 0;
        l->distinct--;
        l->tombs++;
    }
    h->idx = NULL;
    return 0;
}

/* Remove a specific member n from its key group. Fast path: n is the chain
 * head (pop_front / remove_by_key targets) — O(1). Otherwise walks from the
 * head to n's predecessor: O(rank) <= O(group). Returns 0 on success, -1 if
 * n is not a live indexed member of its key group (invariant violation). */
static int ll2_index_remove_node(llist_t *l, ll_node_t *n)
{
    if (l->bulk_open) { l->bulk_muts++; return 0; }

    int existed = 0;
    size_t i = ll2_probe(l, n->key, 0, &existed);
    if (!existed || !l->heads[i]) return -1;

    if (l->heads[i] == n)                      /* head: O(1)                 */
        return ll2_index_remove_head(l, n->key);

    ll_node_t *p = l->heads[i];
    while (p && p->idx != n) p = p->idx;       /* O(rank) predecessor walk   */
    if (!p || p->idx != n) return -1;          /* not a member: invariant    */
    p->idx = n->idx;
    n->idx = NULL;
    l->counts[i]--;
    if (l->counts[i] == 0) {                   /* group now empty            */
        l->flags[i]  = IX_TOMB;
        l->heads[i]  = NULL;
        l->distinct--;
        l->tombs++;
    }
    return 0;
}

/* Rebuild every group's chain in the list's CURRENT order (called after a
 * sort re-links the nodes). Fast path: one temp [cap]-sized tails array, one
 * O(n) pass. On temp-array OOM a correct slower fallback walks each group's
 * tail per node. Never fails (the public sort() is void and must succeed). */
/* Single O(n) walk-and-probe pass: rebuild every key group's chain in the
 * list's CURRENT order into the list's current table, which must be FRESH
 * (all slots IX_EMPTY; no tombstones). Group heads end up as FIRST
 * list-occurrences. Fast path: one [cap]-sized tails array, O(1) per node.
 * On scratch OOM a correct slower fallback walks each group's tail per node.
 * Never fails (callers: llist_sort / llist_end_bulk — both void-on-success). */
static void ll2_populate(llist_t *l)
{
    const size_t cap = l->cap;
    l->distinct = 0;
    l->tombs    = 0;

    ll_node_t **tails = NULL;
    if (l->count > 0 && cap <= SIZE_MAX / sizeof (ll_node_t *)) {
        if (l->tails_scratch_cap == cap) tails = l->tails_scratch;
        else tails = (ll_node_t **)ll2_xcalloc(cap, sizeof *tails);
    }

    if (tails) {
        /* Fresh table => no IX_TOMB anywhere; `existed` means an earlier
         * member of the SAME key was already placed in this pass. */
        for (ll_node_t *n = l->head.next; n != &l->tail; n = n->next) {
            int existed = 0;
            size_t i = ll2_probe(l, n->key, 0, &existed);
            if (existed) {                      /* append via running tail    */
                tails[i]->idx = n;
                l->counts[i]++;
            } else {                            /* first member of this key   */
                l->distinct++;
                l->flags[i]  = IX_OCC;
                l->keys[i]   = n->key;
                l->heads[i]  = n;
                l->counts[i] = 1;
            }
            tails[i] = n;
        }
        for (size_t i = 0; i < cap; i++)
            if (l->flags[i] == IX_OCC && tails[i]) tails[i]->idx = NULL;
        if (tails != l->tails_scratch) free(tails);
    } else {
        /* Slow fallback (scratch OOM): append by walking each group's chain. */
        for (ll_node_t *n = l->head.next; n != &l->tail; n = n->next) {
            int existed = 0;
            size_t i = ll2_probe(l, n->key, 1, &existed);
            if (existed) {
                ll_node_t *t = l->heads[i];
                while (t->idx) t = t->idx;
                t->idx = n;
                l->counts[i]++;
            } else {
                l->distinct++;
                l->flags[i]  = IX_OCC;
                l->keys[i]   = n->key;
                l->heads[i]  = n;
                l->counts[i] = 1;
            }
            n->idx = NULL;
        }
    }
}

/* Re-establish the index in the list's current order IN PLACE (table size
 * unchanged): zero the slots, then one populate pass. O(n + cap). */
static void ll2_index_rebuild(llist_t *l)
{
    memset(l->flags, 0, l->cap);                /* -> IX_EMPTY               */
    memset(l->heads, 0, l->cap * sizeof *l->heads);
    memset(l->counts, 0, l->cap * sizeof *l->counts);
    ll2_populate(l);
}


/* ========================================================================== */
/* v1-compatible public API (sentinel list) + index maintenance               */
/* ========================================================================== */

/* The sentinels always form a circular doubly-linked ring (the list's nodes
 * are spliced into it). One uniform invariant for empty and non-empty lists:
 * every node's prev/next is always valid. */
static void ll2_init_sentinels(llist_t *l)
{
    l->head.value = NULL;
    l->tail.value = NULL;
    l->head.next = &l->tail;
    l->head.prev = &l->tail;
    l->tail.prev = &l->head;
    l->tail.next = &l->head;
}

/* Shared core of both constructors. On failure everything allocated so far
 * is released and NULL is returned (same contract as v1's create). */
static llist_t *ll2_new_common(ll_free_value_fn free_value, int arena, size_t initial)
{
    llist_t *l = calloc(1, sizeof *l);
    if (!l) return NULL;

    ll2_init_sentinels(l);
    l->count      = 0;
    l->free_value = free_value;

    /* Per-list salt from pointer entropy (production default). No OS entropy APIs. */
    uint64_t e = (uint64_t)(uintptr_t)l ^ ((uint64_t)(uintptr_t)&free_value >> 4);
    l->salt = ll2_splitmix64(e);

    /* Initial table, cap = 8. */
    l->cap      = 8;
    l->distinct = 0;
    l->rehashes = 0;
    l->keys   = ll2_xcalloc(8, sizeof *l->keys);
    l->flags  = ll2_xcalloc(8, 1);            /* MUST be zeroed (IX_EMPTY)   */
    l->heads  = ll2_xcalloc(8, sizeof *l->heads);
    l->counts = ll2_xcalloc(8, sizeof *l->counts);
    if (!l->keys || !l->flags || !l->heads || !l->counts) {
        free(l->keys); free(l->flags); free(l->heads); free(l->counts);
        free(l);
        return NULL;
    }

    l->arena     = arena;
    l->last_grow = 0;
    if (arena && ll2_new_slab(l, initial < 8 ? 8 : initial) != 0) {
        free(l->keys); free(l->flags); free(l->heads); free(l->counts);
        free(l->slabs);
        free(l);
        return NULL;
    }
    return l;
}

/* Create a list. free_value may be NULL (then clear/destroy do not touch the
 * values). Returns NULL only on allocation failure. */
llist_t *llist_create(ll_free_value_fn free_value)
{
    return ll2_new_common(free_value, 0, 0);
}

/* Create an arena-mode list: nodes are served from an appended
 * slab pool instead of per-node malloc/free. initial_capacity tunes the first
 * slab only; growth is geometric and failure-atomic (see header). */
llist_t *llist_create_arena(size_t initial_capacity, ll_free_value_fn free_value)
{
    return ll2_new_common(free_value, 1, initial_capacity);
}

int llist_grow_ok(const llist_t *l)
{
    /* Malloc-mode lists have no pool growth: the last attempt trivially ok. */
    return (l && !l->arena) ? 0 : (l ? l->last_grow : -1);
}

static void ll2_node_dispose(llist_t *l, ll_node_t *n)
{
    /* Value destructor only — node memory is owned by the list's mode and   */
    /* released explicitly via ll2_node_release() at every removal site.     */
    if (l->free_value && n->value)
        l->free_value(n->value);
}

/* Destroy the list and all remaining elements. */
void llist_destroy(llist_t *l)
{
    if (!l) return;
    ll_node_t *n = l->head.next;
    while (n != &l->tail) {
        ll_node_t *next = n->next;
        ll2_node_dispose(l, n);                /* value destructor           */
        if (!l->arena) free(n);                /* arena nodes: whole slab below */
        n = next;
    }
    for (size_t i = 0; i < l->nslabs; i++)     /* one free per slab   */
        free(l->slabs[i].base);
    free(l->slabs);
    free(l->tails_scratch);
    free(l->keys); free(l->flags); free(l->heads); free(l->counts);
    free(l);
}

size_t      llist_count(const llist_t *l)   { return l->count; }
bool        llist_is_empty(const llist_t *l){ return l->count == 0; }
ll_node_t  *llist_front(const llist_t *l)   { return (l->head.next != &l->tail) ? l->head.next : NULL; }
ll_node_t  *llist_back(const llist_t *l)    { return (l->tail.prev  != &l->head) ? l->tail.prev  : NULL; }

/* -------------------------------------------------------------------------- */
/* Adding                                                                     */
/* All add functions return 0 on success, -1 on allocation failure            */
/* (insert_at additionally returns -1 for an out-of-range index); on failure  */
/* the list is left untouched.                                                */
/* -------------------------------------------------------------------------- */

static int ll2_attach_front(llist_t *l, ll_key_t key, void *value)
{
    ll_node_t *n = ll2_node_alloc(l);
    if (!n) return -1;
    n->key   = key;
    n->value = value;
    n->prev  = NULL; n->next = NULL;          /* spliced below              */

    if (ll2_index_insert_front(l, n) != 0) { ll2_node_release(l, n); return -1; }

    /* splice before the current first node (or before &tail when empty) */
    ll_node_t *first = l->head.next;
    n->prev         = first->prev;
    n->next         = first;
    first->prev->next = n;
    first->prev   = n;
    l->count++;
    return 0;
}

static int ll2_attach_back(llist_t *l, ll_key_t key, void *value)
{
    ll_node_t *n = ll2_node_alloc(l);
    if (!n) return -1;
    n->key   = key;
    n->value = value;
    n->prev  = NULL; n->next = NULL;

    if (ll2_index_insert_back(l, n) != 0) { ll2_node_release(l, n); return -1; }

    /* splice after the current last node (or after &head when empty) */
    ll_node_t *last  = l->tail.prev;
    n->prev          = last;
    n->next          = &l->tail;
    last->next       = n;
    l->tail.prev     = n;
    l->count++;
    return 0;
}

int llist_push_front(llist_t *l, ll_key_t key, void *value)
{
    return ll2_attach_front(l, key, value);
}

int llist_push_back(llist_t *l, ll_key_t key, void *value)
{
    return ll2_attach_back(l, key, value);
}

/* Insert at 0-based index `index` (0 = head, count = append at end).
 * Walks from the closer end (as in v1) AND counts how many existing nodes
 * with the SAME key precede `index` — that is n's rank within its key group,
 * which is all the index splice needs. */
int llist_insert_at(llist_t *l, size_t index, ll_key_t key, void *value)
{
    if (index > l->count) return -1;

    if (index == 0)
        return ll2_attach_front(l, key, value);   /* fast path */

    /* `after` = node at position index-1. Walk from the closer end while
     * computing rank = #existing nodes with this key in [0, index). The
     * backward walk derives its rank from llist_count_of_key(), which takes
     * the O(n) scan fallback inside a bulk window — so during a window we
     * always take the forward walk (still exactly v1's cost class). */
    ll_node_t *after;
    size_t rank;
    if (!l->bulk_open && index <= l->count - index) { /* forward from head   */
        after = &l->head;
        rank  = 0;
        for (size_t i = 0; i < index; i++) {
            if (after->next->key == key) rank++;   /* node i precedes index   */
            after = after->next;
        }
    } else {                                       /* backward from tail     */
        size_t steps = l->count - index;           /* nodes in [index, n)    */
        after = &l->tail;
        rank  = llist_count_of_key(l, key);        /* total existing (O(1)) */
        for (size_t i = 0; i < steps; i++) {
            if (after->prev->key == key) rank--;   /* node in [index, n)     */
            after = after->prev;
        }
        after = after->prev;                       /* land at index-1; it is
                                                     BEFORE the insertion and
                                                     must NOT be uncounted   */
    }

    ll_node_t *n = ll2_node_alloc(l);
    if (!n) return -1;
    n->key   = key;
    n->value = value;
    n->prev  = NULL; n->next = NULL;

    if (ll2_index_splice_rank(l, n, rank) != 0) { ll2_node_release(l, n); return -1; }

    n->next        = after->next;
    n->prev        = after;
    after->next->prev = n;
    after->next    = n;
    l->count++;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Removing                                                                   */
/* All pop/remove functions return the removed value pointer (ownership now   */
/* belongs to the caller) or NULL if nothing was removed.                     */
/* -------------------------------------------------------------------------- */

static void ll2_node_unlink(ll_node_t *n)
{
    n->prev->next = n->next;
    n->next->prev = n->prev;
}

/* Remove and return the first element. Index maintenance is the O(1) head
 * path: the front node is by definition its key group's chain head. */
void *llist_pop_front(llist_t *l)
{
    if (l->count == 0) return NULL;
    ll_node_t *n = l->head.next;
    void *value = n->value;

    if (ll2_index_remove_head(l, n->key) != 0) {
        /* Cannot happen if the index is consistent; bail out without freeing
         * so a debug build can inspect the still-consistent state. */
        abort();
    }

    ll2_node_unlink(n);
    l->count--;
    ll2_node_release(l, n);
    return value;
}

/* Remove and return the last element. The back node is its group's LAST
 * member, so index maintenance walks the group to its predecessor: O(group)
 * (the single-8B-node-field budget forbids a backlink). */
void *llist_pop_back(llist_t *l)
{
    if (l->count == 0) return NULL;
    ll_node_t *n = l->tail.prev;
    void *value = n->value;

    if (ll2_index_remove_node(l, n) != 0) { abort(); }   /* invariant breach */

    ll2_node_unlink(n);
    l->count--;
    ll2_node_release(l, n);
    return value;
}

/* Remove the element at 0-based index `index`; returns its value. The list
 * walk finds the node (closer end, as in v1); the index removal is then a
 * chain walk of O(rank) from the group head. */
void *llist_remove_at(llist_t *l, size_t index)
{
    if (index >= l->count) return NULL;

    ll_node_t *n;
    if (index <= l->count - 1 - index) {
        n = &l->head;
        for (size_t i = 0; i <= index; i++) n = n->next;
    } else {
        size_t steps = l->count - index;           /* hops back from tail    */
        n = &l->tail;
        for (size_t i = 0; i < steps; i++) n = n->prev;
    }

    if (ll2_index_remove_node(l, n) != 0) { abort(); }   /* invariant breach */

    void *value = n->value;
    ll2_node_unlink(n);
    l->count--;
    ll2_node_release(l, n);
    return value;
}

/* Remove the FIRST element whose key matches: O(1) amortized — the index's
 * group head IS the node a v1 forward scan would find. (v1: O(n).) */
void *llist_remove_by_key(llist_t *l, ll_key_t key)
{
    /* Open window: the table predates the mutations — v1's forward scan is
     * the fallback (same first-occurrence answer, O(n)). */
    if (l->bulk_open) {
        for (ll_node_t *n = l->head.next; n != &l->tail; ) {
            ll_node_t *next = n->next;
            if (n->key == key) {
                void *value = n->value;
                ll2_index_remove_head(l, key);     /* guarded: mut-counter only */
                ll2_node_unlink(n);
                l->count--;
                ll2_node_release(l, n);
                return value;
            }
            n = next;
        }
        return NULL;
    }

    int existed = 0;
    size_t i = ll2_probe(l, key, 0, &existed);
    if (!existed) return NULL;

    ll_node_t *n = l->heads[i];              /* first list-occurrence        */
    void *value = n->value;

    if (ll2_index_remove_head(l, key) != 0) { abort(); }   /* cannot happen  */

    ll2_node_unlink(n);
    l->count--;
    ll2_node_release(l, n);
    return value;

}

/* Remove ALL elements whose key matches; returns how many were removed.
 * Their values are destroyed by the list (none can be handed back). One probe
 * + one O(group) chain walk replaces v1's O(n) scan. */
size_t llist_remove_all_by_key(llist_t *l, ll_key_t key)
{
    /* Open window: single forward scan removes every match (no index work). */
    if (l->bulk_open) {
        size_t removed = 0;
        for (ll_node_t *n = l->head.next; n != &l->tail; ) {
            ll_node_t *next = n->next;         /* capture before rewiring   */
            if (n->key == key) {
                /* unlink BEFORE release: reading n->prev/n->next after the
                 * node has gone back to its allocator is a use-after-free
                 * (benign on plain malloc, fatal with slab recycling).     */
                ll2_node_unlink(n);
                ll2_node_dispose(l, n);
                l->count--;
                removed++;
                ll2_node_release(l, n);        /* last use of n             */
            }
            n = next;
        }
        if (removed) l->bulk_muts++;
        return removed;
    }

    int existed = 0;
    size_t i = ll2_probe(l, key, 0, &existed);
    if (!existed) return 0;

    size_t removed = 0;
    for (ll_node_t *n = l->heads[i]; n; ) {
        ll_node_t *next = n->idx;            /* capture before any rewiring  */
        n->idx = NULL;
        ll2_node_unlink(n);                  /* list links are O(1) each     */
        ll2_node_dispose(l, n);
        ll2_node_release(l, n);
        l->count--;
        removed++;
        n = next;
    }

    /* group now empty: tombstone the slot (probe chains survive).          */
    l->flags[i]  = IX_TOMB;
    l->heads[i]  = NULL;
    l->counts[i] = 0;
    l->distinct--;
    l->tombs++;
    return removed;
}

/* -------------------------------------------------------------------------- */
/* Iteration (unchanged from v1)                                              */
/* -------------------------------------------------------------------------- */

size_t llist_foreach(llist_t *l, ll_foreach_fn fn, void *user)
{
    size_t visited = 0;
    for (ll_node_t *n = l->head.next; n != &l->tail; n = n->next) {
        if (fn(n, user)) break;
        visited++;
    }
    return visited;
}

/* -------------------------------------------------------------------------- */
/* Searching                                                                  */
/* -------------------------------------------------------------------------- */

/* First node whose key matches, or NULL: O(1) amortized via the index. The
 * group head is the first list-occurrence by construction.
 * Inside an open bulk window the table predates the mutations, so a plain
 * forward scan gives the identical first-list-occurrence answer. */
ll_node_t *llist_find(const llist_t *l, ll_key_t key)
{
    if (!l->bulk_open) {
        int existed = 0;
        size_t i = ll2_probe(l, key, 0, &existed);
        return existed ? l->heads[i] : NULL;
    }
    for (const ll_node_t *n = l->head.next; n != &l->tail; n = n->next)
        if (n->key == key) return (ll_node_t *)n;
    return NULL;
}

/* Node at 0-based index, or NULL. Closer-end walk, <= n/2 hops (as in v1). */
ll_node_t *llist_at(const llist_t *l, size_t index)
{
    if (index >= l->count) return NULL;

    const ll_node_t *n;
    if (index <= l->count - 1 - index) {
        n = &l->head;
        for (size_t i = 0; i <= index; i++) n = n->next;
    } else {
        size_t steps = l->count - index;
        n = &l->tail;
        for (size_t i = 0; i < steps; i++) n = n->prev;
    }
    return (ll_node_t *)n;
}

/* 0-based index of the first element whose key matches, or -1 if absent.
 * Returns a POSITION, so a list walk is unavoidable — same cost as v1. */
long long llist_index_of(const llist_t *l, ll_key_t key)
{
    size_t i = 0;
    for (const ll_node_t *n = l->head.next; n != &l->tail; n = n->next, i++)
        if (n->key == key) return (long long)i;
    return -1;
}

/* True iff at least one element has `key`: one probe (or an early-exit scan
 * inside an open bulk window). */
bool llist_contains(const llist_t *l, ll_key_t key)
{
    if (!l->bulk_open) {
        int existed = 0;
        (void)ll2_probe(l, key, 0, &existed);
        return existed;
    }
    for (const ll_node_t *n = l->head.next; n != &l->tail; n = n->next)
        if (n->key == key) return true;
    return false;
}

/* Number of elements whose key matches: O(1) via the per-slot counter, or an
 * O(n) scan inside an open bulk window. */
size_t llist_count_of_key(const llist_t *l, ll_key_t key)
{
    if (!l->bulk_open) {
        int existed = 0;
        size_t i = ll2_probe(l, key, 0, &existed);
        return existed ? (size_t)l->counts[i] : 0;
    }
    size_t c = 0;
    for (const ll_node_t *n = l->head.next; n != &l->tail; n = n->next)
        if (n->key == key) c++;
    return c;
}

/* Visit all nodes with `key` in LIST order (llist_foreach contract). Closed
 * window: one probe + O(group) chain walk (the chain IS list order). Open
 * window: single forward scan. */
size_t llist_foreach_key(const llist_t *l, ll_key_t key, ll_foreach_fn fn, void *user)
{
    size_t visited = 0;

    if (!l->bulk_open) {
        int existed = 0;
        size_t i = ll2_probe(l, key, 0, &existed);
        if (!existed) return 0;
        for (const ll_node_t *n = l->heads[i]; n; n = n->idx) {
            if (fn((ll_node_t *)n, user)) break;
            visited++;
        }
        return visited;
    }
    for (const ll_node_t *n = l->head.next; n != &l->tail; n = n->next) {
        if (n->key != key) continue;
        if (fn((ll_node_t *)n, user)) break;
        visited++;
    }
    return visited;
}

/* -------------------------------------------------------------------------- */
/* Bulk window (P2.5)                                                         */
/* -------------------------------------------------------------------------- */

int llist_begin_bulk(llist_t *l)
{
    if (!l || l->bulk_open) return -1;          /* no nesting                 */
    l->bulk_open = 1;
    l->bulk_muts = 0;
    return 0;
}

bool llist_bulk_active(const llist_t *l) { return l != NULL && l->bulk_open; }

/* Smallest power of two >= 16 whose 0.7 load factor holds `n`+1 distinct keys. */
static size_t ll2_commit_cap(size_t n)
{
    size_t p = 16;
    while ((p * 7) / 10 < n + 1) {
        if (p > SIZE_MAX / 8) return 0;         /* cannot fit in size_t       */
        p *= 2;
    }
    return p;
}

int llist_end_bulk(llist_t *l)
{
    if (!l || !l->bulk_open) return -1;

    if (l->bulk_muts == 0) {                    /* nothing changed: O(1)      */
        l->bulk_open = 0;
        return 0;
    }

    size_t p = ll2_commit_cap(l->count);
    if (p == 0) return -1;

    /* Fresh zeroed table sized from the FINAL element count. Any allocation
     * failure leaves the pre-window table byte-for-byte intact and the window
     * open: end_bulk is retryable with identical conditions. */
    ll_key_t   *nk = ll2_xcalloc(p, sizeof *nk);
    uint8_t    *nf = ll2_xcalloc(p, 1);
    ll_node_t **nh = ll2_xcalloc(p, sizeof *nh);
    uint32_t   *nc = ll2_xcalloc(p, sizeof *nc);
    if (!nk || !nf || !nh || !nc) {
        free(nk); free(nf); free(nh); free(nc);
        return -1;
    }

    /* Swap the fresh table in and run ONE walk-and-probe pass. populate()
     * cannot fail, so no restore path is needed; transient memory = old + new
     * table while both are live. */
    ll_key_t   *ok  = l->keys;
    uint8_t    *of  = l->flags;
    ll_node_t **oh  = l->heads;
    uint32_t   *oc  = l->counts;

    l->keys = nk; l->flags = nf; l->heads = nh; l->counts = nc;
    l->cap  = p;
    l->rehashes++;                  /* commit IS a table rebuild (diagnostic) */
    ll2_populate(l);

    free(ok); free(of); free(oh); free(oc);
    ll2_resize_tails_scratch(l);
    l->bulk_open = 0;
    l->bulk_muts = 0;
    return 0;
}

/* -------------------------------------------------------------------------- */
/* Clearing / sorting                                                         */
/* -------------------------------------------------------------------------- */

/* Remove every element, invoking the destructor on each value. The index
 * table is zeroed in place (no realloc: clear never fails). O(n + cap). */
void llist_clear(llist_t *l)
{
    ll_node_t *n = l->head.next;
    while (n != &l->tail) {
        ll_node_t *next = n->next;
        ll2_node_dispose(l, n);
        ll2_node_release(l, n);
        n = next;
    }

    memset(l->flags, 0, l->cap);              /* -> IX_EMPTY                */
    memset(l->heads, 0, l->cap * sizeof *l->heads);
    memset(l->counts, 0, l->cap * sizeof *l->counts);
    l->distinct = 0;
    l->tombs = 0;

    l->bulk_open = 0;                            /* clear closes any window  */
    l->bulk_muts = 0;

    ll2_init_sentinels(l);
    l->count = 0;
}

/* Stable bottom-up merge sort over the list's real nodes (verbatim v1):
 * O(n log n), O(1) auxiliary memory for the re-linking. */
static void ll2_merge_runs_after(ll_node_t *P, size_t la, size_t lb, ll_compare_fn cmp)
{
    /* Two adjacent sorted runs start immediately after P:
     *   A = the next `la` nodes (head at P->next), then B = the next `lb`.
     * Merges them in place into one run occupying exactly the same region,
     * ending right before Q (the node after the whole region).
     * O(1) temporaries, no allocation; ties take from A => stable. */
    ll_node_t *a = P->next;
    ll_node_t *b = a;
    for (size_t i = 0; i < la; i++) b = b->next;   /* head of run B */
    ll_node_t *Q = b;
    for (size_t i = 0; i < lb; i++) Q = Q->next;   /* node after the region */

    size_t ra = la, rb = lb;
    ll_node_t *res = P;                            /* P acts as merge dummy */

    while (ra > 0 && rb > 0) {
        if (cmp(a, b) <= 0) {
            ll_node_t *pick = a; a = a->next; ra--;
            res->next = pick;
            pick->prev = res;
            res = pick;
        } else {
            ll_node_t *pick = b; b = b->next; rb--;
            res->next = pick;
            pick->prev = res;
            res = pick;
        }
    }
    /* Attach whichever run still has unpicked nodes (its internal links are
     * intact). The remainder's tail must be rewired to Q explicitly. */
    ll_node_t *rest = (ra > 0) ? a : b;
    res->next = rest;
    if (ra + rb > 0) {
        rest->prev = res;
        ll_node_t *last = rest;
        for (size_t i = 1; i < ra + rb; i++) last = last->next;
        last->next = Q;
        Q->prev = last;
    } else {
        Q->prev = res;
    }
}

static void ll2_insertion_sort_counted(ll_node_t *P, size_t n, ll_compare_fn cmp)
{
    for (size_t i = 1; i < n; i++) {
        ll_node_t *x = P->next;
        for (size_t j = 0; j < i; j++) x = x->next;

        ll_node_t *slot = x;
        ll_node_t *y = x->prev;
        while (y != P && cmp(y, x) > 0) {
            slot = y;
            y = y->prev;
        }
        if (slot == x) continue;                   /* already in place       */

        ll_node_t *xp = x->prev;
        ll_node_t *xn = x->next;
        xp->next = xn;
        xn->prev = xp;

        x->prev = y;
        x->next = slot;
        y->next = x;
        slot->prev = x;
    }
}

/* Sort the list (v1 algorithm) and then rebuild the key index in the new
 * order: O(n log n) + O(n). The index rebuild uses one temporary [cap]
 * array; on its OOM a correct slower fallback runs, so sort never fails. */
/* The merge sort body (v1 algorithm), index-agnostic. */
static void ll2_sort_core(llist_t *l, ll_compare_fn cmp)
{
    if (!l || !cmp || l->count < 2) {
        /* Even for count < 2 the index must be consistent (it always is —
         * nothing changed); keep the guard identical to v1. */
        return;
    }

    const size_t MIN_RUN = 8;                      /* seed run size          */

    size_t left = l->count;
    for (ll_node_t *P = &l->head; P->next != &l->tail; ) {
        size_t rn = (left < MIN_RUN) ? left : MIN_RUN;
        ll2_insertion_sort_counted(P, rn, cmp);
        for (size_t i = 0; i < rn; i++) P = P->next;
        left -= rn;
    }

    for (size_t width = MIN_RUN; width < l->count; width *= 2) {
        left = l->count;
        ll_node_t *P = &l->head;
        while (left > 0) {
            size_t la = (width < left) ? width : left;
            size_t rest_left = left - la;
            if (rest_left == 0) break;             /* nothing to merge with  */
            size_t lb = (width < rest_left) ? width : rest_left;
            ll2_merge_runs_after(P, la, lb, cmp);
            for (size_t i = 0; i < la + lb; i++) P = P->next;
            left -= la + lb;
        }
    }
}

/* Convenience: sort ascending by key (stable). ZERO index work: ll_cmp_keys
 * treats same-key members as equal and the sort is stable, so every group's
 * within-list member order — the only thing the idx chains encode — is
 * provably unchanged. */
void llist_sort_by_key(llist_t *l)
{
    ll2_sort_core(l, ll_cmp_keys);
}

/* General user comparator: same-key members MAY be reordered, so the index's
 * within-group order must be re-established after the merge. O(n log n) + one
 * O(n) pass (see header note). Inside an open bulk window the table stays
 * dormant and end_bulk() rebuilds everything at once. */
void llist_sort(llist_t *l, ll_compare_fn cmp)
{
    if (!l || !cmp) return;
    size_t was_count = l->count;
    ll2_sort_core(l, cmp);
    if (was_count < 2) return;
    if (l->bulk_open) { l->bulk_muts++; return; }
    ll2_index_rebuild(l);                          /* index follows the sort */
}

/* -------------------------------------------------------------------------- */
/* White-box index diagnostics (tests / fuzzers only)                         */
/* -------------------------------------------------------------------------- */

/* Arena pool accounting (diagnostics): nodes_bumped == list_count + freelist
 * length always holds; malloc-mode lists report 0. */
size_t llist_arena_pool_total(const llist_t *l)
{
    return l && l->arena ? l->pool_total : 0;
}

size_t llist_arena_freelist_len(const llist_t *l)
{
    if (!l || !l->arena) return 0;
    size_t n = 0;
    for (ll_node_t *f = l->pool_free; f; f = f->next) n++;   /* debug walk */
    return n;
}

size_t llist_bulk_muts(const llist_t *l)      /* structural ops since begin */
{
    return l && l->bulk_open ? l->bulk_muts : 0;
}

uint64_t llist_index_salt(const llist_t *l)       { return l->salt; }
size_t   llist_index_capacity(const llist_t *l)   { return l->cap; }
size_t   llist_index_distinct(const llist_t *l)   { return l->distinct; }
size_t   llist_index_rehashes(const llist_t *l)   { return l->rehashes; }
size_t   llist_index_tombstones(const llist_t *l) { return l->tombs; }

/* Full invariant sweep: slot/flag consistency,
 * chain lengths vs per-slot counts, every indexed node reachable by probing
 * its own key, counters consistent. Returns 0 iff consistent. Debug cost. */
long llist_index_verify(const llist_t *l)
{
    size_t total = 0;
    for (size_t i = 0; i < l->cap; i++) {
        if (l->flags[i] == IX_OCC) {
            if (!l->heads[i]) return (long)(i + 1);
            size_t c = 0;
            for (ll_node_t *n = l->heads[i]; n; n = n->idx) {
                if (++c > l->count + 1) return -100;          /* cycle */
                if (n->key != l->keys[i]) return (long)(i + 1);
            }
            if (l->counts[i] != c) return -200 - (long)i;
            total += c;
        } else {
            if (l->heads[i]) return (long)(i + 1);
        }
    }
    size_t d = 0;
    for (size_t i = 0; i < l->cap; i++) if (l->flags[i] == IX_OCC) d++;
    if (d != l->distinct) return -300;
    if (total != l->count) return -400;

    /* every indexed node must be reachable by probing its own key */
    for (size_t i = 0; i < l->cap; i++) {
        if (l->flags[i] != IX_OCC) continue;
        for (ll_node_t *n = l->heads[i]; n; n = n->idx) {
            int existed = 0;
            size_t j = ll2_probe(l, n->key, 0, &existed);
            if (!existed || l->keys[j] != n->key) return -500 - (long)i;
        }
    }
    return 0;
}
