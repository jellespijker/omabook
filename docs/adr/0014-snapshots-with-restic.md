# 14. Snapshots with restic, and synced copies don't count

- Status: accepted (#178)
- Date: 2026-11-22
- Builds on: ADR 0013 (one writer)

## Context

A library should be kept three times, on two kinds of medium, one away from home (3-2-1). The person's
computers already hold synced copies of it, but the sync bugs of #154, #158, #164 and #166 carried one mistake
to every computer within one sync: a synced copy shares a library's mistakes, and keeps no past to go back to.
Omabook backs up today only before a library upgrade, on the same disk.

## Decision

- **Only snapshots count as copies**: versioned, read-only, encrypted. A synced copy never does.
- **restic makes them**: encryption, deduplication (each snapshot stores only what changed), and many kinds of
  destination (a disk, SFTP, S3/B2, rclone, another hub). Omabook drives it and shows it; it builds no
  encryption or storage of its own.
- **A snapshot is consistent.** The library database is copied through SQLite's backup API inside the hub's
  write lock (ADR 0013), then restic runs over that copy and the files.
- **Secrets stay off a desktop keyring a hub doesn't have:** restic's password and destination credentials
  live in systemd credentials or a 0600 file in the hub's state. The password is also shown **once** as a
  recovery key for the person to keep; without it no restore is possible.
- **When:** every night, and a **moment snapshot** just before every sync, import, tidy, restore, upgrade or
  bulk change, kept 14 days; then 7 daily, 4 weekly, 12 monthly and yearly ones.
- **Checked:** `restic check` every week, and a real test restore every month, the restored database opened
  and checked.

## Consequences

- `restic` becomes a dependency of the package.
- A lost recovery key loses the snapshots: the setup says so, and the check once in a while restores with it.
- The library diff (#183) and the sync guard (#187) compare against moment snapshots.

## Alternatives considered

- **borg**: fine, but fewer destinations (no S3 without a mount).
- **Our own snapshots**: encryption, deduplication and destinations are a product's work, not ours.
