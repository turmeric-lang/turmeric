/*
 * completion.h -- `tur completion <zsh|bash>`: emit a shell completion script.
 */
#ifndef TUR_CLI_COMPLETION_H
#define TUR_CLI_COMPLETION_H

/* A subcommand as the scripts list it: completed after `tur`, and described
 * by `summary` in zsh's menu. */
typedef struct CompletionCommand {
    const char *name;
    const char *summary;
} CompletionCommand;

/* Print the completion script for argv[2] ("zsh" or "bash") to stdout,
 * listing the n_cmds subcommands in cmds (the driver passes its command
 * table, so the scripts cannot fall behind it).  Returns 0 on success, 2 on
 * a CLI error (message already printed). */
int cmd_completion(int argc, char **argv,
                   const CompletionCommand *cmds, int n_cmds);

/* Print `tur completion` usage to stderr; always returns 0. */
int usage_completion(void);

#endif /* TUR_CLI_COMPLETION_H */
