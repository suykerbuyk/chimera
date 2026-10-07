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

0. **Resolve the sync ticket before touching git.** The branch name,
   every follow-up commit prefix and steps 9 and 10 all need a `CHIM-<n>` key.
   - The request names a ticket: use it.
   - The request names none: ask, with AskUserQuestion, whether to create
     one.  Do not guess a key, reuse a previous sync's ticket, or start
     the branch without one.
   - Answer is yes: create a CHIM **Story** titled `code drop <YYYY-MM-DD>`
     with the exact fields in reference.md ("Creating the sync ticket"):
     assignee Alain, Team Titans, the current Titans sprint, 1 story
     point, parent NG-684, component Unsure.  Read the key back and use it
     for the rest of the sync.  Remember that the skill created it: step
     10 fills its description from the MR body.
   - Answer is no: stop and ask for the key to use.

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
   If it stops at `Does not match the generator used previously`, the tree
   was configured by the IDE with a different generator; do not delete the
   cache, build it as configured: `sudo cmake -S . -B /build/Release &&
   sudo cmake --build /build/Release -j 8`.

6. **Merge commit message.** Compose it before committing; `git commit -F`.
   Format and rationale in reference.md.

7. **Follow-up commits.** Downstream fixes are separate commits, each
   `CHIM-<n>:` prefixed with a `Signed-off-by` trailer. `qtm_build/` and
   `qb.yaml` never share a commit with anything else.

8. **Verify.** On the cluster, never `make check` locally:
   ```bash
   .devcontainer/quantum/qb-ci.sh -a amd64 2>&1 | tee qb-ci.log
   qb -o arenaud-claude --plain rocky9
   ```
   `qb-ci.sh` builds chimera-dev once and runs the merge-train legs sharded
   the way `.gitlab-ci.yml` does, so a failure names one shard instead of
   one hour-long pod.  It does not cover the Rocky9 build legs, and Rocky9
   is the only leg that compiles `snfs`, so run the `rocky9` alias as well.
   Re-running one leg: `qb-ci.sh --no-builder --only <leg>`.  See
   reference.md.

9. **Open the MR.** Push the branch, then `glab mr create` against
   `develop`: title = the merge commit subject, label `Sync origin/main`,
   body composed in a file in the shape reference.md gives ("Opening the
   MR").  Keep that file: step 10 reuses it.

10. **Update the Jira tickets.** For every CHIM ticket whose upstream PR
    landed in this range (the merge message's "Jira coverage" paragraph),
    add a comment naming the upstream commits, the sync tag, the merge
    commit and the MR, and link it to the sync ticket with a `Relates`
    issue link.  Then the sync ticket's description:
    - step 0 created the ticket: the description is the MR body from
      step 9, verbatim, under one line linking the MR;
    - the ticket was given: the CHIM-214 shape in reference.md.
    Details and the comment shape in reference.md.

## Red Flags

| Thought | Reality |
|---|---|
| "No ticket was given, I'll branch now and fill the key in later" | The key is in the branch name and every commit. Ask first (step 0). |
| "The last sync's ticket is still open, reuse it" | One ticket per code drop. Create a new one. |
| "The merge was clean, so it's fine" | Every break so far was a clean auto-merge. Run the checks. |
| "Only N files conflicted, small sync" | Conflict count is unrelated to breakage. A header that *moves* never conflicts. |
| "The checks passed, skip the build" | The checks encode known failures only. The build finds the new ones. |
| "No upstream Dockerfile change, skip step 4" | Still confirm it — recording "no-op" is the check. |
| "snfs is Rocky-only, local build covers it" | Local build covers it only where `/usr/cvfs/include` exists. Run the rocky9 leg. |
| "I re-ran that one qb leg, so the image has my change" | A bare `qb <leg>` uses the `develop`-tagged builder image; name `chimera-dev` too, or use `qb-ci.sh --only <leg>` without `--no-builder`. |
| "qb-ci.sh was green, so snfs compiles" | Its legs are the merge train's, all Ubuntu. Only the `rocky9` alias builds snfs. |

## Common Mistakes

- **Tagging `origin/main` before the final fetch.** Upstream advances; the
  tag then names a commit you did not merge.
- **Letting git write the merge message.** Item 7 of the standing request is
  an annotated message: commit list, Jira key, tag, submodule moves.
- **Re-applying a downstream rename onto a moved upstream file.** Prefer an
  alias over a rename — see the fh-magic case in reference.md.
- **Closing the loop on git only.** A sync that merges a CHIM fix's
  upstream form resolves that ticket; if nobody comments and links it, the
  ticket sits in review until someone rediscovers it (CHIM-222/223/225 at
  the 2026-10-02 sync).
- **Trusting a substring grep as a check.** A renamed symbol often survives
  as a trailing comment, which makes a naive `grep -q` report success on the
  broken tree.

## Reference

reference.md — per-artifact detail: what mirrors what, the recurring
hazards, the merge message format, and the packaging acceptance test.
