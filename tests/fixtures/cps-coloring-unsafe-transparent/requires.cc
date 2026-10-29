# Compile-phase dump fixture: the --dump-* output goes to the BUILD
# invocation's stream, which run.sh discards; under one-process tur jit
# it interleaves with the program's own stdout. Phase-separated by design.
