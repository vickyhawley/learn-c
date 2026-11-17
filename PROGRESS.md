# Progress

## Toolchain
Apple clang 17.0.0, arm64-apple-darwin24.3.0, lldb at /usr/bin/lldb (checked 2026-11-10).

## Lessons finished
- 01: hello world, compiling vs running (2026-11-10). Answered both check questions correctly.
  Corrected: "header" means the .h file, not the top of the file. A header gives
  declarations only; the linker supplies printf's actual code (lesson 10).

## Lesson 01 notes
- hello.c compiles cleanly with -Wall -Wextra -g and runs, exit code 0.
- Looked at: `file` (Mach-O arm64), `clang -E` (9 lines become 584), the -O1 assembly
  (x0 holds the argument, w0 the return value, bl = call, printf→puts optimisation).

## Lesson 02 notes (in progress)
- sizes.c: measured char 1, short 2, int 4, float 4, long 8, double 8 bytes (arm64 macOS).
- Learned %zu for sizeof (size_t); -Wall caught %d mismatches.
- First linker error: had compiled an unsaved, empty file, so ld couldn't find _main.
- Found pointer arithmetic by accident: "Hello" + 1 → "ello".
- unsigned char 255 + 1 → 0 (wraparound). printf("%d", small + 1) showed 256 because of integer promotion.
- Saw signed int overflow is UB: it wraps silently without a sanitiser, and -fsanitize=undefined reports it.
- Check questions asked: 250 + 10 in unsigned char; why 256 vs 0.
- 02 finished 2026-11-12. Q1: knew it wraps but said 0, not 4. Wrapping is "mod 256", it keeps counting past 0.
  Q2: got "256 doesn't fit in one byte", but thought promotion to int *causes* overflow.
  It's the opposite: an int has room for 256, and truncation only happens on assignment back into the char.

## Concepts I found hard
- Top-level statements: tried to call printf outside any function (TS allows that, C doesn't).
- A `;` after a function signature makes it a prototype, not a definition.
- Thought of arrow functions for a function body; C has only `type name(params) { }`.
- Single vs double quotes: 'x' is a char (an integer), "x" is a string.
- #include position: it's a textual paste, so it must come before use, at the top.
- Tends to leave off -Wall -Wextra -g when compiling. Keep nudging.
- Missing semicolons is the most common error (5+ times in lesson 2). TS's ASI habit.
- Expression vs assignment: wrote `small + 1` expecting it to change small.
- Mixed up the C# column of the comparison table with C (used `byte`). Make the C line in tables stand out more.
- Compile vs run: compiled and expected output; needs `&& ./prog`.
- Lesson 03: defined is_even *inside* main (TS closure habit). C has no nested functions; all are top level.
  Another missing semicolon in the same attempt.
- Lesson 03: top-level statements again. Moved the calls (not the function) below main, so they sat outside any function.
  Second time this has come up (also lesson 01).
- Lesson 03: saw "call to undeclared function" when is_even was below main. C reads top to bottom, with no hoisting.
  Fixed it with a prototype `int is_even(int n);` (2026-11-13). Needed both hints to get there.
  The `;` that confused them in lesson 01 is now understood as "promise only, the body comes later".
- even.c still prints "evenodd" with no newlines. Mentioned twice and not fixed yet.

- Check questions answered 2026-11-15: both correct (definition above main needs no prototype;
  TS hoisting ↔ C prototypes). Clarified: a definition is also a declaration. Skipped the newline bonus.

## Lesson 03 still to do
- Assembly exercise set 2026-11-15: is_even at -O1 is `mov w8,#1; bic w0,w8,w0; ret` (no branch, 1 & ~n),
  and main has no call to is_even (inlined and constant-folded into two printfs).
  Answered: 3 instructions, no branch, "it worked out the answers". Said "immediately", so taught compile time
  vs runtime (C folds at compile time; V8/RyuJIT optimise at runtime). Didn't trace bic themselves; I showed it.
- Loop exercise set 2026-11-15: sum.c with int sum_to(int n) using a for loop, print sum_to(10) with \n.
  Then look at _sum_to's -O1 assembly. As of 2026-11-17 sum.c only has the bare signature (no body, no ;).
- 2026-11-17: correctly said `break` inside a switch leaves only the switch, hence `goto done` in the
  colleague's todo app. Showed the TS labelled-break equivalent.
- sum.c first attempt (2026-11-17): prototype route, but missing ; on the prototype and on printf (semicolons
  again), and `for (i = 0; ...)` with no `int` (undeclared i). Also a logic bug: `sum += n` instead of
  `sum += i`, which will print 110 (11 × 10), and the loop starts at 0. Not mentioned yet; let them spot it from the output.
  Second attempt: all 3 compile fixes correct, prints 110. Gave hint 1 (110 = 11 × 10, look at line 12).
  Third attempt: changed `+=` to `=` (prints 10), fixing the wrong half. Gave hint 2: keep +=, the right-hand
  side should be the loop variable. Concept to watch: = overwrites vs += accumulates.
  Fourth attempt: `sum += i`, prints 55 (2026-11-17). Needed both hints. Loop still starts at i = 0 (harmless, mentioned).
- sum.c -O1 assembly shown: main has `mov w8, #55` (constant folded), and _sum_to has no loop. It's the closed
  form n(n-1)/2 + n with cmp/csel for n < 0 (branchless, like a ternary). Asked 3 questions: where the loop
  went in main, what formula _sum_to computes, and csel ↔ ternary.
  Q1 answered correctly: "worked out 55 at compile time again". Used "compile time" unprompted, so the
  compile-time vs runtime concept has landed.
  Q2: worked through 9 → 90 → 45 → 55 by hand once I'd explained what "trace it" meant (asked what to do).
  Didn't answer the n = 1000 part; I explained O(n) → O(1). Q3: csel ↔ ternary, correct.
- 03 finished 2026-11-17.

## Colleague's linked list (reference/linked_list_v2/), added 2026-11-17
Copied from ~/Downloads: linked_list_v2.h, linked_list_v2.c (library, ~1,400 lines) and app_todolist_v2.c
(an interactive to-do menu app that uses it). (I first copied dlist.* by mistake and have removed that copy.)
Doubly linked list with sentinel head/tail nodes, plus a hash index on an int64 key (O(1) find) and an
optional arena/slab allocator. Very advanced. Teach from the APP first, and from the library selectively.
Build: clang -Wall -Wextra -g -fsanitize=address,undefined linked_list_v2.c app_todolist_v2.c -o todo
Builds with no warnings and ran a scripted session cleanly (2026-11-17).
At the end of each lesson, point them at the parts it unlocks:
- 03 control flow: app switch statements (prio_str, the menu in main), for(;;) + break/continue (read_id),
  while loop in parse_ll, goto done in main; linked_list_v2.h is mostly prototypes.
- 04 arrays/strings: todo_item_t's char title[64], fgets/strcspn/snprintf, parse_ll's '0'..'9' char maths.
- 05 pointers: ->, NULL checks, out-parameters (parse_ll's long long *out, ll2_probe's int *existed).
- 06 heap: malloc/free of todo items, who frees what (menu_remove frees, the list's todo_free otherwise).
- 07 structs: ll_node_t, todo_item_t, the opaque llist_t (struct only in the .c).
- 08 memory bugs: the use-after-free comment in llist_remove_all_by_key; saving `next` before release.
- 09 our own list: compare ours with ll2_attach_back / ll2_node_unlink; why sentinels remove the NULL checks.
- 10 headers/linking: include guards, static helpers, static inline functions in the header.
- Not in the curriculum: function pointers (foreach, compare, free_value), void* values, enums, hashing
  (splitmix64, linear probing), arena allocation, merge sort. Cover after 09, probably as an extra lesson.

## Next
04: arrays and strings (why C doesn't check bounds). Open with the teaser from lesson 02: why "a" takes 2 bytes
(the \0 terminator). Colleague hooks: todo_item_t's char title[64], parse_ll's '0'..'9' char maths.
Housekeeping still to do: add a .gitignore for build outputs (even, sum, todo, *.dSYM) before committing.
