# Turmeric Argument Parser

## Objective
Design and implement a standard argument parser for Turmeric scripts to handle named arguments, flags, and type conversion.

## Background
The [CLI Arguments plan](cli-arguments-plan.md) introduces raw argument passing via `*args*`. This plan builds on that foundation to provide a structured way to parse and validate arguments.

## Proposed Solution

### 1. API Design
- Provide a `parse-args` function that takes a specification and the raw `*args*` list:
  ```turmeric
  (defn parse-args [spec args] :map
    "Parses CLI arguments according to the given spec.")
  ```

- **Spec Format**:
  ```turmeric
  {:flags       ["--verbose" "--help"]          ; boolean flags
   :options     {"--input"  :string               ; bare keyword = required, no default
                "--count"  {:type :int    :default 1}
                "--output" {:type :string :default "out.txt"}}
   :args        [:string :string]                 ; positional args
   :subcommands {"commit" {:flags   ["--amend"]
                           :options {"--message" :string}
                           :args    []}
                "push"   {:flags   ["--force"]
                          :options {"--remote" {:type :string :default "origin"}}
                          :args    []}
                "remote" {:flags   []
                          :options {}
                          :args    []
                          :subcommands {"add"    {:flags   []
                                                  :options {}
                                                  :args    [:string :string]}
                                        "remove" {:flags   []
                                                  :options {}
                                                  :args    [:string]}}}}}  ; subcommand specs
  ### 2. Features
  - **Flags**: Boolean flags (e.g., `--verbose`).
  - **Options**: Named arguments with types, preferring `--key=value` syntax (e.g., `--input=file.txt`), but also accepting a space-separated form (e.g., `--input file.txt`). Option values may be a bare type keyword (required, no default) or a map `{:type :int :default 1}` (optional with default).
  - **Positional Args**: Ordered arguments with types (e.g., `input.txt output.txt`).
  - **Type Conversion**: Automatic conversion to `:string`, `:int`, `:float`, or `:bool`.
  - **Subcommands**: Support arbitrarily nested subcommands (e.g., `git remote add`), each with their own spec containing `:flags`, `:options`, `:args`, and further `:subcommands`.
  - **Help Generation**: Auto-generate help text from the spec, including per-subcommand help.
  - **Error Handling**: Clear error messages for invalid arguments.

  ### 3. Example Usage

  **Simple options** (`--key=value` preferred, space also accepted):
  ```turmeric
  (defn main []
  (let [args-spec {:flags   ["--verbose"]
                   :options {"--input"  :string                          ; required
                             "--count"  {:type :int    :default 1}
                             "--output" {:type :string :default "out.txt"}}}
        parsed    (parse-args args-spec *args*)]
    ; parsed always contains :count and :output (from defaults if not supplied)
    (if (contains? parsed :help)
      (print-help args-spec)
      (process (get parsed :input)
               (get parsed :count)
               (get parsed :output)))))
```

**Subcommands**:
```turmeric
(defn main []
  (let [args-spec {:subcommands {"build" {:options {"--output" :string}
                                          :flags   ["--release"]}
                                 "test"  {:options {"--filter" :string}
                                          :flags   ["--verbose"]}}}
        parsed    (parse-args args-spec *args*)]
    (case (get parsed :subcommand)
      "build" (do-build (get parsed :output) (get parsed :release false))
      "test"  (do-test  (get parsed :filter)  (get parsed :verbose false))
      (print-help args-spec))))
```

### 4. Error Handling
- Validate required arguments.
- Check argument types and provide descriptive errors.
- Support custom validation functions.

## Implementation Steps
1. Design the `parse-args` function and spec format.
2. Implement `--key=value` parsing (with fallback to `--key value` space form).
3. Implement subcommand dispatch recursively: at each level, detect the first non-flag token, look it up in `:subcommands`, and parse the remaining tokens against the matched sub-spec. Repeat until no further subcommand matches or no `:subcommands` key is present. The result map contains a `:subcommand` path (e.g., `["remote" "add"]`) reflecting the full chain.
4. Add type conversion and validation.
5. Resolve option entries: a bare keyword means required; a `{:type T :default V}` map means optional — merge defaults into the result map before returning.
6. Implement help text generation (top-level and per-subcommand), including default values where present.
7. Write tests for all features (both `=` and space forms, subcommand routing, nested subcommand routing, default injection).
8. Update documentation.

## Open Questions

(None remaining.)

## Alternatives Considered
- **Manual Parsing**: Rejected for being error-prone and inconsistent.
- **External Libraries**: Rejected to keep the standard library self-contained.