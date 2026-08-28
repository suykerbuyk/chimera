---
name: sync-main-to-develop
description: Use when merging upstream origin/main into the Quantum develop branch of qtm-chimera - a periodic upstream code sync, a sync-code-<date> tag, or a CHIM-xxx-sync-<date> branch.
---

# Syncing upstream main into develop

## Overview

`origin/main` is the upstream Chimera mirror. `develop` adds the Quantum
downstream layer: `qtm_build/`, `qb.yaml`, `builders/`,
`.devcontainer/quantum/`, `src/vfs/snfs/`, `test_suites/`.

**Core principle: the merge reports success on exactly the things it
breaks.** Every downstream break in this workflow so far has been a silent
auto-merge, not a conflict. A clean `git merge` is not evidence of anything.
The checks below, and a build, are the evidence.

## When to Use

- "sync main into develop", "merge upstream", "code sync"
- A branch named `CHIM-<n>-sync-<date>` / `chim-<n>-sync-<date>`
- Creating a `sync-code-<YYYYMMDD>` tag

## Checklist

Create a todo per step. Do not skip a step because the merge was clean.

1. **Tag the upstream snapshot.** Lightweight tag on the upstream commit,
   pushed to `origin` only (never `arenaud16`):
   ```bash
   git fetch origin --tags --prune
   git tag sync-code-$(date +%Y%m%d) origin/main
   git push origin sync-code-$(date +%Y%m%d)
   ```
   Tag the commit you actually merge. Upstream moves during long sessions —
   re-check `git rev-parse origin/main` right before tagging.

2. **Pre-flight report.** `./.claude/skills/sync-main-to-develop/sync-report.sh pre`
   Read it. It names the conflict candidates, submodule moves, and whether
   upstream touched the Dockerfiles or devcontainer this cycle.

3. **Merge.** `git merge --no-ff --no-commit origin/main`, resolve, then
   `git submodule update --init --recursive`.
   Known resolutions in reference.md — read it before resolving.

4. **Post-merge drift checks.** `sync-report.sh post` (exit non-zero = work to do)
   plus the two it cannot check statically: upstream `Dockerfile.<os>` →
   `builders/<os>/`, and the packaged include layout.

5. **Build before believing the merge.**
   ```bash
   sudo make build_release
   ```
   This is the step that finds what the checks miss. It has found a broken
   `snfs` include the grep-based check did not model.

6. **Merge commit message.** Compose it before committing; `git commit -F`.
   Format and rationale in reference.md.

7. **Follow-up commits.** Downstream fixes are separate commits, each
   `CHIM-<n>:` prefixed with a `Signed-off-by` trailer. `qtm_build/` and
   `qb.yaml` never share a commit with anything else.

8. **Verify.** `qb -o arenaud-claude --plain check` (never `make check`
   locally). Add `qb -o arenaud-claude --plain rocky9` — Rocky9 is the only
   leg that compiles `snfs`. Re-running one leg on its own does **not**
   rebuild the builder image; name `chimera-dev` alongside it. See
   reference.md.

## Red Flags

| Thought | Reality |
|---|---|
| "The merge was clean, so it's fine" | Every break so far was a clean auto-merge. Run the checks. |
| "Only N files conflicted, small sync" | Conflict count is unrelated to breakage. A header that *moves* never conflicts. |
| "The checks passed, skip the build" | The checks encode known failures only. The build finds the new ones. |
| "No upstream Dockerfile change, skip step 4" | Still confirm it — recording "no-op" is the check. |
| "snfs is Rocky-only, local build covers it" | Local build covers it only where `/usr/cvfs/include` exists. Run the rocky9 leg. |
| "I re-ran that one qb leg, so the image has my change" | It does not. Single legs use the `develop`-tagged builder image; name `chimera-dev` too. |

## Common Mistakes

- **Tagging `origin/main` before the final fetch.** Upstream advances; the
  tag then names a commit you did not merge.
- **Letting git write the merge message.** Item 7 of the standing request is
  an annotated message: commit list, Jira key, tag, submodule moves.
- **Re-applying a downstream rename onto a moved upstream file.** Prefer an
  alias over a rename — see the fh-magic case in reference.md.
- **Trusting a substring grep as a check.** A renamed symbol often survives
  as a trailing comment, which makes a naive `grep -q` report success on the
  broken tree.

## Reference

reference.md — per-artifact detail: what mirrors what, the recurring
hazards, the merge message format, and the packaging acceptance test.
