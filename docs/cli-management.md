# MCP and skill management

These commands run directly from the shell. They do not open an agent session,
call a model, or start the first-run configuration wizard. Run `morph mcp --help`
or `morph skills --help` for command usage. Argument parsing uses the system's
`getopt_long`; long options accept both `--option value` and `--option=value`.

Global `-c PATH` selects the configuration file and `-w PATH` selects the working
directory. Put global options before the command group:

```sh
morph -c ./config.toml mcp list
morph -w /path/to/project skills add owner/repo --skill review
```

`-c` is also accepted after the management action. A global relative config path
is interpreted before changing directory with `-w`; a subcommand's relative
`-c` path is interpreted in the selected working directory. Session options such
as `-p`, `-s`, `--events`, and `--trace-json` cannot be combined with management
commands. The session option `-s` is not a scope abbreviation.

## MCP servers

The CLI accepts the common Claude Code form and the Codex `--url` form:

```sh
morph mcp add filesystem -- npx -y @modelcontextprotocol/server-filesystem /tmp
morph mcp add remote --transport http https://example.com/mcp
morph mcp add remote --url https://example.com/mcp
```

The two HTTP examples are alternatives for the same entry. Stdio is the default
transport. Everything after `--` is the program and its arguments, passed
without shell evaluation. Morph's options go before that separator. `--url`
implies HTTP; an explicit conflicting transport is rejected. Supported transports
are `stdio` and Streamable HTTP (`http`).

Environment variables and bearer authentication:

```sh
morph mcp add example --env MODE=production --env REGION=west -- example-mcp
morph mcp add remote --url https://example.com/mcp --auth-token-env MCP_TOKEN
```

`--env` / `-e` is repeatable and stores stdio environment assignments in the
configuration. `--auth-token-env` stores the environment variable's name and
resolves its value when connecting. `--bearer-token-env-var` is an alias.
`--transport` has the short form `-t`.

```sh
morph mcp list
morph mcp get remote
morph mcp test remote
morph mcp remove remote
morph mcp add remote --replace --url https://new.example.com/mcp
```

- `add` saves a `[[mcp.servers]]` entry. It does not test the connection.
- `list` and `get` read configuration without connecting. `get` displays program
  arguments and environment names; environment values are redacted.
- `test` starts/connects the selected server, performs the MCP initialization
  handshake, prints the result, and disconnects.
- `remove` deletes the entry. Duplicate names are rejected by `add` unless
  `--replace` is supplied.

The default file is `~/.morph/config.toml`. `--scope user` makes the supported
scope explicit; project MCP configuration is not supported. `-c` selects another
file. Existing comments and unrelated configuration are retained; mutations are
locked, schema-validated, and written through a temporary file and rename. New
files have mode `0600`. Existing files must be regular files, not symlinks.
Inline `mcp.servers = [{...}]` configurations can be inspected, but must be
converted to `[[mcp.servers]]` before CLI mutation.

Changes are read by the next Morph runtime; restart an existing chat to use them.

## Skills

`morph skills` is the documented spelling; `morph skill` is an equivalent alias.

```sh
morph skills add owner/repo
morph skills add https://github.com/owner/repo.git
morph skills add git@git.example.com:team/skills.git
morph skills add ./my-skill
```

`OWNER/REPO` expands to a GitHub repository. Other Git sources use their normal
Git HTTPS, SSH, or `file://` URLs. A local directory with a `.git` entry is treated
as a Git repository and installs committed content; other local directories are
copied directly. Git operations use the system Git executable and its configured
credentials. Private SSH repositories use the existing SSH agent/key setup.
Package scripts and Git hooks are not executed; submodules are not initialized.

Morph discovers a root `SKILL.md`, or recursively discovers skill directories in
the source if the root has none. Each installed skill must declare a nonempty
name and description. Installed names use letters, digits, hyphens, and
underscores. A discovered skill directory is a complete package; its resources
and executable scripts are copied with it. Relative symlinks inside the package
are preserved; absolute, broken, or escaping links are rejected.

### Choose skills and versions

```sh
morph skills add owner/repo --list
morph skills add owner/repo --skill review
morph skills add owner/repo --skill review testing
morph skills add owner/repo --skill review --skill testing
morph skills add owner/repo --all
morph skills add owner/repo --path skills/review --ref v1.2.0
```

- `--list` lists available skills without installing or creating the install
  directory.
- `--skill` selects by the name in `SKILL.md`, independently of directory names.
  It can be repeated and accepts additional skill names as positional arguments.
- `--path` limits discovery to a directory within the source. Paths escaping the
  source are rejected. It can also disambiguate duplicate names in a repository.
- `--ref` accepts a branch, tag, or commit. Omit it to follow the repository's
  default branch. It applies only to Git sources.
- One discovered skill installs directly. Multiple skills prompt for selection
  in a terminal. In noninteractive runs, pass `--skill` or `--all`.
- `-y` / `--yes` disables interactive selection; it never implies `--all`.

Git downloads are prepared in temporary checkouts under
`~/.morph/cache/skills/` and removed when the command finishes. Source URLs are
stored in installation metadata, so use Git's credential configuration rather
than credentials embedded in URLs.

### Installation scope and discovery

```sh
morph skills add owner/repo --skill review       # current working directory
morph skills add owner/repo --skill review -g    # user installation
morph skills list                               # effective discovered catalog
morph skills list -g                            # user catalog
```

The project install root is `<working-directory>/.agents/skills/`. There is no
automatic search for a parent Git root; use `morph -w PROJECT ...` when needed.
The global root is `~/.morph/skills/`, overridden by `[skill].dir` in the selected
configuration. `--global` / `-g` selects global scope for add, remove, and update.

Discovery order (first name wins):

1. `<runtime-working-directory>/.agents/skills/`.
2. `[skill].dir`, or `~/.morph/skills/` when unset.
3. `~/.agents/skills/` when the default user directory is used.
4. Bundled skills.

`list` shows the effective catalog with installation directory, recorded source,
and Git commit when available. Files installed manually or by another installer
are identified as unmanaged. A global listing omits project and bundled skills.
CLI and runtime use the same discovery ordering. Restart an existing chat to
discover a new installation.

### Updates and removal

```sh
morph skills update review
morph skills update              # all managed project installations
morph skills update -g           # all managed user installations
morph skills remove review
morph skills remove review testing -g
morph skills add owner/repo --skill review --replace
```

An installation stores `.morph-install.json` with its source, repository path,
requested ref, resolved commit, and a BLAKE3 snapshot of its installed files.
Branch/default-branch installations follow that branch on update; a requested
commit remains pinned. A requested tag is resolved again under that same name.
Local directory installations update by copying their original source again.

Update and replacement prepare a new installation before moving the old one
aside and replacing it. A failed preparation retains the existing installation;
a failed replacement attempts to restore it. Installation changes are serialized
per install root. Multi-skill operations report each completed skill and may
leave completed installations in place if a later skill fails.

Update, remove, and `--replace` reject installations with local file changes.
Preserve those changes outside the installation or restore its original files
before retrying. They also reject unmanaged skills; no-argument update skips
unmanaged directories. Manual or third-party installations remain discoverable.
Default mutation scope is always the project, even when `list` shows an effective
global skill; pass `-g` to update or remove that installation.
