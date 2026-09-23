# Security

Please report vulnerabilities privately through GitHub's *Report a
vulnerability* (Security advisories) on this repository rather than in a
public issue.

Things worth knowing about Nala's design:

- The model only acts through the tools in `src/assistant/assistant.cpp`.
  Arguments are validated against a schema; writing files and running shell
  commands always require the user's confirmation, whatever the settings; the
  shell is off by default.
- File tools are confined to configured folders (home by default), resolve
  symlinks before checking, and refuse credentials, keys, browser profiles and
  Nala's own data.
- Prompt injection: text the model reads — files, command output, window
  titles, memories — may have been written by someone else. After reading any
  of it, opening a URL needs confirmation, as does any URL that could carry
  data (a query, fragment or long path), so a planted instruction cannot
  quietly send data out.
- Helpers are started with argument lists, never through a shell, except the
  shell tool itself, which is its purpose and is confirmed each time.
- The control socket is in `$XDG_RUNTIME_DIR` with user-only permissions.
- Settings, logs, screenshots and the memory database are created
  owner-only; logs redact anything that looks like a credential.

In scope: anything that lets content on screen, in a file, a web page or a
model reply cause an action the user did not approve, or that keeps what the
privacy settings say is not kept.
