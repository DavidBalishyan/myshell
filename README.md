# lsh

`lsh` is a small POSIX-oriented shell written in C. Its behavioral reference is
`dash`, not Bash: unmatched pathname patterns remain literal, redirections are
evaluated from left to right, and no Bash-only syntax is intentionally accepted.

The original `getline`/`strtok` teaching shell has been refactored without using
another shell as its parser or evaluator. Input now follows this pipeline:

```text
source -> lexer -> quote-preserving tokens -> AST parser
       -> word expansion -> redirection/process execution
```

## Supported features

### Command execution

- External commands, arguments, absolute and relative executable paths
- `PATH` lookup through `execvp(3)`
- Environment inheritance and temporary command environments such as
  `FOO=bar command`
- Conventional exit statuses, including 126 for non-executable commands and
  127 for commands that cannot be found
- Script execution, standard-input execution, interactive mode, and `-c`
- Correct propagation of `$?` through commands, lists, pipelines, conditionals,
  loops, functions, and subshells

### Lexing, parsing, and quoting

- Quote-preserving lexer and structured AST parser
- Unquoted words, single quotes, double quotes, and backslash escaping
- Comments and backslash-newline continuation
- Command separators and operators: `;`, `&`, `|`, `&&`, and `||`
- Negated pipelines using `!`
- Multiline compound commands and continuation prompts in interactive mode

### Variables and expansion

- Shell-local and exported variables
- Assignment-only commands and assignments preceding commands
- `export`, `readonly`, and `unset`
- Special parameters: `$?`, `$$`, `$#`, `$0` through positional parameters,
  `$@`, `$*`, `$!`, and `$-`
- Braced parameters and parameter string length, such as `${name}` and
  `${#name}`
- Unset/null parameter operators: `:-`, `-`, `:=`, `=`, `:+`, `+`, `:?`, and
  `?`
- Command substitution using both `$(command)` and legacy backticks, including
  nesting and removal of trailing newlines
- Integer arithmetic expansion with precedence, variables, comparisons,
  bitwise/logical operators, conditional expressions, and assignments
- `IFS` field splitting with quoted/unquoted expansion differences
- POSIX-style pathname expansion for `*`, `?`, and bracket patterns; unmatched
  patterns remain literal
- Tilde expansion for `~` and `~/path`
- Correct multi-field behavior for quoted `"$@"`

### Redirections and process composition

- Input/output redirections: `<`, `>`, `>>`, and `<>`
- Explicit descriptors such as `2>file` and `3>>file`
- Descriptor duplication and closing using `<&` and `>&`
- Left-to-right redirection ordering
- Here-documents with expanding and quoted, non-expanding delimiters
- Persistent descriptor redirections through `exec`, such as `exec 3>file`
- Arbitrarily sized pipelines with descriptor cleanup and last-command status
- Asynchronous commands, `$!`, basic job listings, and `wait`

### Shell language

- Sequential and asynchronous command lists
- Short-circuiting `&&` and `||`
- Parenthesized subshells and current-environment brace groups
- `if`, `elif`, `else`, and `fi`
- `while` and `until`
- `for` with explicit lists or the current positional parameters
- `break`, `continue`, and multi-level loop control
- `case` with shell pattern matching and quoted-pattern handling
- Shell functions with function-local positional parameters and `return`
- Aliases and alias removal
- Dot-sourcing with changes preserved in the current shell
- `eval` through the normal lexer/parser/executor pipeline
- `set -e`, `set -u`, `set -x`, `set -f`, and `set --`
- `EXIT`, `INT`, `TERM`, and `HUP` traps

### Builtins

The following builtins are currently provided:

```text
.       :        alias    break    cd       command
continue echo    eval     exec     exit     export
false   getopts  help     jobs     printf   pwd
read    readonly return   set      shift    times
trap    true     type     umask    unalias  unset
wait
```

Build and run:

```sh
make
./lsh
./lsh -c 'echo "$HOME"'
./lsh script.sh one two
make test
```

The regression suite executes the same scripts with `dash` and `lsh` and compares
stdout and exit status. This is still a learning shell, not a certification-level
or security-hardened system shell. Full interactive terminal job control,
`select`, POSIX locale edge cases, and several rarely used `set` options remain
future work; aliases and basic background jobs/`wait` are supported.
