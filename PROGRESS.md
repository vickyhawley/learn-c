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

## Next
Finish 02 (check questions), then 03: control flow and functions. Teaser left open: why "a" takes 2 bytes (the \0 terminator, lesson 4).
