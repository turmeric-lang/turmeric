# CLI Arguments for `tur run`

## Objective
Add support for passing CLI arguments to `tur run` to allow users to pass runtime arguments to Turmeric scripts.

## Background
Currently, `tur run` does not support passing CLI arguments directly to the script being executed. This limits the ability to write scripts that accept runtime configuration or input.

## Proposed Solution

### 1. Syntax Design
- Use `--` to separate `tur run` arguments from script arguments, following common CLI conventions:
  ```sh
  tur run script.tur -- arg1 arg2
  ```
- All arguments after `--` are passed to the script.

### 2. Implementation
- Modify the `tur run` command in the CLI to parse and forward arguments after `--` to the script execution environment.
- Introduce a new built-in variable (e.g., `*args*`) in Turmeric to expose the passed arguments as a list.

### 3. Script Access
- Scripts can access arguments via the `*args*` list:
  ```turmeric
  (defn main []
    (println "Arguments:" *args*))
  ```

### 4. Error Handling
- Validate that `--` is used correctly and provide clear error messages.
- Ensure arguments are properly escaped and passed to the script.

## Example Usage
```sh
# Pass arguments to a script
tur run script.tur -- input.txt output.txt
```

```turmeric
;; script.tur
defn main []
  (let [input  (nth *args* 0)
        output (nth *args* 1)]
    (process-file input output))
```

## Implementation Steps
1. Update the CLI argument parser in `tur` to support `--` separation.
2. Modify the script execution logic to pass arguments to the Turmeric runtime.
3. Add `*args*` to the runtime environment.
4. Write tests for argument parsing and script access.
5. Update documentation.

## Open Questions
- Should we support named arguments (e.g., `--input=file.txt`)?
- How should we handle argument type conversion (e.g., strings to numbers)?

## Alternatives Considered
- Environment variables: Rejected because the user explicitly requested CLI arguments.
- Custom syntax (e.g., `tur run script.tur :args [arg1 arg2]`): Rejected for being non-standard.