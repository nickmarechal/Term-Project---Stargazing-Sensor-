# Transcripts

Raw Claude Code `.jsonl` session files, **per partner, per milestone.** These are a graded
deliverable (10 individual points each, spec §13) and committing them here is how they survive
the 30-day local purge.

## The rule

> "the raw `.jsonl` session files from `~/.claude/projects/`, copied in as-is. Not summarized.
> Not exported. Not retyped." — spec §10.4

Copy out at **every** milestone M1 through M5, covering work since the previous milestone.
This costs a minute and is the only way to guarantee the transcripts graded at M5 actually cover
the whole semester.

## Where they come from

Claude Code stores sessions per working directory:

```
~/.claude/projects/<path-with-dashes>/<session-id>.jsonl
```

For work done inside this repo the directory is:

```
~/.claude/projects/-Users-<you>-Desktop-CS-370-Term-Project-term-project/
```

To copy your sessions for a milestone:

```bash
mkdir -p transcripts/<your-name>/<date>-<milestone>
cp ~/.claude/projects/-Users-<you>-Desktop-CS-370-Term-Project-term-project/*.jsonl \
   transcripts/<your-name>/<date>-<milestone>/
```

## The 30-day purge

Local transcripts are deleted after 30 days by default, and this project spans more than 30 days.
Either copy out at every milestone (do this anyway) or raise the retention window:

```bash
# in ~/.claude/settings.json
{ "cleanupPeriodDays": 180 }
```

Losing Week-5 transcripts in Week 15 is a **foreseeable loss, and foreseeable losses are not
excused** (spec §10.4).

## Important: sessions are personal

Each partner works in **their own clone and their own Claude Code sessions**, and commits their
own transcripts. The five-level transcript scale is applied per partner, to that partner's own
sessions. Working in one shared login defeats this and costs **both** partners.

## What's here

- `nick/2026-09-23-m0-m1/` — Nick's sessions through M0/M1. These predate this repo existing, so
  they come from the parent `CS 370` project directory and cover **all** CS 370 work in that
  window (HW1 and term-project ideation together). Copied as-is, unedited, as required. Sessions
  from here on will be scoped to this repo's own project directory.
- `nick/2026-10-28-m2/` — Nick's term-project session through 2026-10-28 (the same session ID
  as in `m0-m1`, so this is a later, longer snapshot of it), plus its two subagent sessions
  (`subagent-*.jsonl`). Still from the parent `CS 370` project directory. **One edit from
  as-is:** a password Nick typed into chat on 2026-10-27 is replaced with `[REDACTED-PASSWORD]`
  (12 occurrences, a `sed` over the copy; every line is still valid JSON). Nothing else changed.
- `liam/2026-09-23-m0-m1/` — Liam Sagal's sessions from this repo's own project directory
  (`~/.claude/projects/-s-parsons-g-under-C837127061-cs370-Term-Project/`). Copied as-is,
  unedited.
- `liam/2026-10-27-m2/` — Liam's sessions through the M2 design document, copied as-is.

Each partner uses their own named folder (`nick/`, `liam/`). The `partner/` placeholder from the
initial setup has been removed now that both are populated.
