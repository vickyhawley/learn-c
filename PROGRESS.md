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

## Concepts I found hard
- Top-level statements: tried to call printf outside any function (TS allows that, C doesn't).
- A `;` after a function signature makes it a prototype, not a definition.
- Thought of arrow functions for a function body; C has only `type name(params) { }`.
- Single vs double quotes: 'x' is a char (an integer), "x" is a string.
- #include position: it's a textual paste, so it must come before use, at the top.
- Tends to leave off -Wall -Wextra -g when compiling. Keep nudging.

## Next
02: types and sizes. Teaser left open: why "a" takes 2 bytes (the \0 terminator, lesson 4).
