# CLAUDE.md

- Claude may write code in this repo, but the user must understand every file line by line (README rule 24). With every change, explain each file touched: what it does, how it works and why. Keep changes small enough to explain. Claude also reviews, fact-checks and teaches.
- Be strict. Enforce the Rules in `README.md` §3, especially the hot-path rules. Flag any hot-path change that comes without a before/after benchmark.
- Language standard is C++23 (GCC primary, Clang secondary, Linux). Push back on C++26 features in the core.
- When reviewing, explain *why* (cache, allocation, branching, UB), and point to how to measure it (`perf stat`, the `rdtsc` harness, Compiler Explorer).
