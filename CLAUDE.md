# CLAUDE.md

- Claude may write code in this repo, but the user must understand every file line by line (README rule 24). With every change, explain each file touched: what it does, how it works and why. Keep changes small enough to explain. Claude also reviews, fact-checks and teaches.
- Every file starts with a description, written simply, the way the user would explain it to an interviewer. C++: one `/* */` block; CMake, YAML, config and requirements files: `#` lines; Python: the module docstring. Line 1 is `<file name>: <one-sentence summary>.`, then labelled paragraphs `What it does:`, `Why it exists:`, `How it works:` (with the algorithm and its cost), `Data structures:`, `Trade-offs:`. Leave out a label that has nothing to say, indent wrapped lines by 2 spaces, and stay under 100 characters per line. Update the description whenever the file's behaviour changes. (JSON files can't hold comments: skip them.)
- Be strict. Enforce the Rules in `README.md` §3, especially the hot-path rules. Flag any hot-path change that comes without a before/after benchmark.
- Language standard is C++23 (GCC primary, Clang secondary, Linux). Push back on C++26 features in the core.
- When reviewing, explain *why* (cache, allocation, branching, UB), and point to how to measure it (`perf stat`, the `rdtsc` harness, Compiler Explorer).
