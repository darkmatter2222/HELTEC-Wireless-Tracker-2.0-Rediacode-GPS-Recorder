# Copilot Session Branch Management

These instructions apply to every Copilot Chat conversation in this repo.
Read and follow them before doing any other work.

---

## Session Branch Rule

A **session** maps 1-to-1 with a Copilot Chat conversation.
Every session must have its own dedicated git branch.

### At the start of every session (first thing you do)

1. Run `git branch --show-current` to see the current branch.
2. If the current branch is **`main`** (or `master`), immediately create and
   switch to a session branch:
   ```
   git checkout -b copilot/session-YYYYMMDD-HHMMSS
   ```
   Use the actual current date-time (UTC preferred, local is fine).
   Example: `copilot/session-20260502-143000`
3. If you are already on a `copilot/session-*` branch, stay on it — this is
   a resumed session. Do not create a second branch.
4. Announce the branch name to the user so they know which branch is active.

### After every turn in which you made file changes

After completing all edits or file operations in a turn, always run:

```powershell
git add -A
git commit -m "<type>: <short description of what changed this turn>"
```

Commit message types: `feat`, `fix`, `refactor`, `docs`, `chore`.

Example commit after firmware change:
```
feat(ui): add double-long-press confirmation for stop-recording
```

**Do not skip the commit** even if the change is small or experimental. The
user's intent is that every turn's work is captured atomically.

If there is nothing to commit (no files changed), skip silently — do not report "nothing to commit" unless the user asks.

### Session wrap-up (when explicitly asked)

When the user says "done", "wrap up", "merge", or similar:
1. Show a summary of commits on the session branch vs main.
2. Ask the user if they want to merge/squash into main or leave the branch open.
3. Never merge without explicit user confirmation.

---

## Full technical context

See [AGENTS.md](../AGENTS.md) for hardware pinouts, subsystem descriptions,
build/deploy commands, code style rules, and lessons learned.
All rules in AGENTS.md apply alongside these session rules.
