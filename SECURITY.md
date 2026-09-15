# Security

## Reporting a vulnerability

Use **[GitHub private vulnerability reporting](../../security/advisories/new)** rather
than a public issue. If that is unavailable, contact the repository owner directly.

Please include what you did, what happened, and the file or input that triggered it. A
G-code file that reproduces the problem is worth more than a description.

## What this software does, and therefore what the risk is

`flowtemp` **rewrites G-code that drives a heated nozzle at 200–300 °C**. The realistic
harm is not data loss. It is:

- A file that looks processed but is not, printed unattended.
- Temperature or feedrate values that damage a hotend or cause a failed print.
- Reading files anywhere the user can, since the tool is handed arbitrary paths.

That shapes what is treated as a security concern here.

## Threat model

| Boundary | Trust | Handling |
|---|---|---|
| G-code input | **Untrusted.** Downloaded, shared, or produced by any slicer | Parsed defensively; fuzzed in CI |
| `config.json` | **Untrusted** — describes machine limits | Parsed tolerantly; mismatch against the slicer's own estimate is reported |
| `profiles.json` | Semi-trusted; written by us, hand-editable | Parsed tolerantly; a corrupt file must not prevent startup |
| `klipper_estimator` output | **Untrusted** — a subprocess boundary | Parsed defensively; fuzzed |
| The local web UI | Loopback only | See below |

### The web interface

`flowtemp serve` binds to **127.0.0.1 only** and is not reachable from the network. This
is deliberate and load-bearing: the server reads and writes files anywhere the user can,
and opens native file dialogs. **Do not expose the port**, put it behind a proxy, or bind
it to `0.0.0.0`.

There is no authentication, because there is no remote access. If remote access is ever
added, authentication and CSRF protection become prerequisites, not enhancements.

Request bodies are capped at 1 MB and idle connections are dropped after 15 seconds.

### Subprocess execution

The estimator is launched with an **argument vector, never a shell command line**. This
is not only a correctness fix for paths containing spaces — it removes shell
interpretation of a path that ultimately came from user input.

## Silent-wrongness is treated as a defect class

The tool this replaces would write a file carrying a "processed" header that contained no
temperature commands at all, with no error shown — seven of nine sample files from one
real session. Someone printing that gets no benefit and no warning.

Accordingly:

- **No output file is written when processing fails.** Output is staged and moved into
  place only on success.
- The processed marker is written **only** on success.
- Results report what actually changed, so "it did nothing" is visible.
- Computed print time is cross-checked against the slicer's own estimate, because a
  config for the wrong machine produces an entirely self-consistent but wrong result.
- The web UI **never overwrites the input file.**

Reports of this kind — output that is wrong while appearing correct — are wanted, even
where no memory-safety issue exists.

## Supply chain

- `bin/klipper_estimator.exe` is a **vendored fork**, not upstream. It is pinned, its
  hash is recorded in the SBOM job, and it is deliberately not auto-updated while the
  in-process replacement is validated against it ([ADR-0007](docs/adr/0007-own-motion-planner.md)).
- Catch2 is pinned by git tag and is test-only.
- An SBOM is produced on every push.
- **Licence compliance is enforced in CI.** This project is MIT, and adding a GPL
  dependency would relicense it for everyone who redistributes it — a change invisible at
  code review. See `tools/check-licences.ps1`.

## What is checked automatically

Every push and pull request: build and test on Windows and Linux, ASan + UBSan,
clang-tidy, CodeQL, secret scanning, licence compliance, SBOM, fuzzing, and the
architectural guards that keep the core free of UI and I/O dependencies.

## Not yet validated

**No output from this software has been verified by a physical test print.** Compare
against a known-good tool and supervise the first print. This is stated in the README and
in the interface, and it is the honest state of the project.
