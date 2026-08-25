bcachefs-tools
==============
Userspace tools and docs for bcachefs

Bcachefs is an advanced new filesystem for Linux, with an emphasis on reliability and robustness
and the complete set of features one would expect from a modern filesystem.

This is the official development repository for bcachefs — the userspace
tools, the documentation, and the filesystem source itself. bcachefs is
maintained out of mainline and ships as a DKMS kernel module built from this
tree. To build it into a kernel source tree instead (`CONFIG_BCACHEFS_FS`),
run [scripts/install-to-kernel.sh](./scripts/install-to-kernel.sh), which
copies `fs/` into `<kernel>/fs/bcachefs/` and wires it into the kernel build.

Fork changes over upstream
--------------------------

This fork carries stability and performance work for HDD (rotational
media) deployments, layered on top of upstream bcachefs (base commit
`0aeaf7e1f0d6`, 2026-08-20). The 54 commits below are not in upstream.

### Snapshot deletion: throughput, pacing, and storm mitigation

Deleting a large snapshot (millions of keys) used to pin the system for
days: per-key transactions monopolized the journal and btree IO, copygc
was dragged into the invalidation storm, and data movers relocated
extents that were about to disappear anyway.

- **Batched migrations**: up to 32 keys staged per transaction, sized by
  a transaction-memory watermark that halves itself on ENOMEM and is
  remembered per-filesystem (module-param tunable divisor), with the
  migration-target iterator kept alive and repositioned across keys
  instead of paying a fresh root-to-leaf lookup per key.
- **Pacing**: the migration rate is capped at 50K keys/s while the
  dying-snapshot backlog is small (steady state) and unthrottled when
  it grows; the rate limit counts only keys actually migrated or
  dropped, never scanned keys; no ratelimit sleeping before mount
  completes (recovery-pass workers hold run_lock).
- **copygc coordination**: fragmentation allowance widened (1/2 → 3/4
  of free space) and wall-clock backoff (doubling per unproductive run,
  observed at 25-50% CPU spin otherwise) while a deletion pass runs;
  copygc and the other data movers skip extents owned by snapshots
  being deleted (hundreds of GB of wasted moves observed in the field).
- **Btree path correctness fixes found under the storm**: a cached path
  node pointer can dangle and the recycled struct btree be reused for a
  node of a different btree — the lock sequence number is not proof of
  identity, so the relock and path-reuse sites now verify the node's
  btree_id before trusting the cache.
- **Runtime toggle**: the `snapshot_delete_v2` module param selects the
  v2 scan vs the v1 per-key path.

Commits: `f81ef2b164cfd`, `45a833c9c40f2`, `0604a8c094d35`,
`bff6910cc9f97`, `a0739e600363e`, `ea6d2ceaaf74f`, `636abd2f51758`,
`a89c17b4f6b36`, `6c0835ad33283`, `7e7a2b005ada7`, `d59d873285e77`,
`3c5c1e64f284c`, `617496e55c4c8`, `ec9acb8a98d81`

### HDD livelock / journal deadlock defense

On HDD-only systems under heavy concurrent workloads (VMs, containers,
database servers), dozens to hundreds of processes could enter
uninterruptible D state for hours — `journal_max_open` stalls, load 50+
with unsaturated IO, no recovery without reboot. Three mechanisms break
the cycles upstream still does not guard against:

- **Transaction admission control**: a semaphore throttle limiting
  concurrent btree transactions to 32 on rotational devices.
  PF_MEMALLOC and PF_WQ_WORKER contexts bypass it, and acquisition uses
  `down_timeout()` with retry, so reclaim and write-completion workers
  can never deadlock against throttled transactions.
- **Btree lock wait timeout**: a 30-second timeout in
  `bch2_six_check_for_deadlock()` restarts any transaction that has
  been waiting too long for a btree lock. This breaks dependency chains
  that pass through non-lock resources (journal space, IO completion),
  which the lock cycle detector cannot see.
- **Background work throttling**: btree cascade-aware throttling for
  writeback and journal reclaim, plus a revert of background IO tuning
  that worsened VM stalls.

Commits: `102879d271660`, `b3ca9a7452e39`, `3bcec538cc3c5`
(`fb8fe3622ea9a` later dropped the journal-reservation 30s timeout from
the first commit — upstream's 1s SRCU lock-drop already covers it.)

### Btree-cache throttle deadlock/livelock under snapshot deletion

Two failure shapes observed on HDD while a multi-million-key snapshot
deletion ran against a live database workload, both diagnosed via a new
stall watchdog and both fixed in `ad98acf326bc7`:

- **Deadlock (whole system unreachable)**: transactions parked in the
  btree-write dirty-ratio throttle (upstream's
  `bch2_trans_commit_btree_write_ratelimit`, which blocks commits when
  dirty btree nodes exceed 3/4 of the cache) kept holding this fork's
  transaction admission-control slots. Btree write completions
  (`btree_node_write_work`) are workqueue workers that take a
  transaction from the same background slot pool — the only path that
  turns dirty nodes clean — so with sleepers holding all slots the
  dirty ratio could never drop and the wait never ended. Fix: release
  the admission slot across the throttle wait and reacquire after.
- **Livelock (deletion frozen at 52% across reboots)**: the deletion
  pass is the one workload that must dirty the cache faster than it
  drains; the dirty-ratio throttle starved it at the 75% boundary
  (dirty oscillated 75–81% with zero net drain, journal reclaim flushed
  166K pins yet in-flight btree writes sat at zero). Fixes: deletion
  commits at `BCH_WATERMARK_btree` (above-normal, exempt from the
  dirty-ratio throttle but still bounded by the in-flight write
  limit), and journal reclaim skips its cascade back-off while dirty >
  3/4 live — when committers are throttled, reclaim is the only
  drainer.
- **Stall watchdog** (`fs/journal/stall_watchdog.c`): a kthread that
  samples per-bdi `WB_WRITEBACK`/`WB_RECLAIMABLE`/`WB_WRITTEN` every
  10s and, after 60s of no progress, dumps journal state, btree cache
  dirty/live/in-flight/throttle counters, and every blocked task's
  stack. Zero hot-path overhead; this is what captured both failure
  signatures above on an unreachable-over-SSH machine via netconsole.
- **`__GFP_RECLAIMABLE` scope fix**: flags reaching vmalloc/kvmalloc
  paths are stripped first — `vmalloc_fix_flags()` WARNs on
  unsupported flags; only pure kmalloc paths keep RECLAIMABLE.

Commits: `ad98acf326bc7`

### Fair btree transaction slot allocation

Per-process fair allocation of btree transaction slots, split into
foreground/background priority pools so background scanners cannot
starve interactive IO, with a sysfs-tunable per-process slot cap.

Commits: `17e76e80990f0`, `599561bd7e52a`, `bf0c32b63eb1b`

### kcompactd0 / memory compaction stabilization

Btree node data buffers (256KB each, ~550MB on a typical HDD system)
landed in MIGRATE_UNMOVABLE pageblocks, making them immovable obstacles
for kcompactd0: ~40% compaction failure rate and kcompactd0 burning 64%
CPU in retry loops. Fixes:

- `__GFP_RECLAIMABLE` on btree node data pages and all other large/hot
  allocations (journal, bio, compression workspaces, key cache, write
  buffer, darray defaults) so the pages become reclaimable and the
  shrinker can free whole pageblocks. (Later scoped in `ad98acf326bc7`
  to pure kmalloc paths only — vmalloc strips and WARNs on it.)
- `__GFP_NORETRY` on direct kmalloc paths so bcachefs never triggers
  direct compaction itself; raised shrinker seeks (sysfs-tunable) so
  kcompactd prefers reclaiming page cache over btree nodes; kmalloc
  path forced for btree node allocations.

Commits: `34a6669c3769d`, `d0488fbd79ba5`, `6afc4024799c1`,
`4c75479a955f4`, `fb93fd355ddea`, `7ef2f2d9d51cb`, `5b063e98d3d43`,
`9ec51c5bd39e0`, `e5c28b43ab04e`, `5aea94b3d73f3`, `169fb6a3d220d`

### Device-aware (HDD) IO scheduling and cache policy

Btree cache and IO scheduling decisions keyed off the rotational flag:
device-aware cache retention and scheduling, relaxed HDD throttling for
single-process heavy IO (a lone VM no longer drags the rest of the
system into D state), `dev_readahead` changes taking effect at runtime,
and a fix for the `devs_rotational` bitmap being empty at init (upstream
lacks this; device-latency based timeouts silently misbehave without
it).

Commits: `0225bf1f808e5`, `47f49cbb8432c`, `8d2539960832a`,
`5e82d35701891`

### Read path: cold-cache first read, readahead, random-read detection

- Btree node prefetching on the read and readdir paths, with snapshot
  ID resolution hoisted out of the per-extent loop — eliminates
  cold-cache node fetches and redundant subvolume lookups on first
  access to large directories.
- Shared btree iterator across the readahead window for better HDD NCQ
  utilization.
- Random-read detection with an extent map cache, HDD `narrow_crcs`,
  two-stream readahead tracking (avoids false random detection), and
  fixes for random-read suppression, extent cache staleness, and
  readahead folio leaks.

Commits: `8e0e91b775f94`, `2325be458d414`, `9a1ee2e198bb7`,
`3976b460d777c`, `c19772e888b36`, `db52acfea6dfa`, `05f11a5ffea90`

### nocow write path: throughput and correctness

- Nocow trylock fast path taken while still holding btree locks (bucket
  stale check skipped under lock), an expanded nocow lock table
  (1024→4096 buckets) to cut hash-collision waits, single-replica fast
  submit, and per-IO overhead reduction on the nocow DIO path.
- Correctness refinements: the snapshot ID is resolved per direct IO
  instead of cached (a stale cached id could send in-place writes into
  snapshot-shared blocks); in-place writes to unwritten extents and
  fallocate's unwritten extent allocation are preserved.

Commits: `f018a15286166`, `a20704f471c55`, `7304faa27089f`,
`17923e040b587`, `30185cfcfabd8` (`82793cc838970` — this fork's
"disable unwritten extents under nocow" ENOSPC workaround — was fully
reverted by `17923e040b587` + `30185cfcfabd8`: upstream's
`BCH_TRANS_COMMIT_no_enospc` fixes the unwritten→written conversion
failure natively, so avoiding unwritten extents is no longer needed.)

### Background maintenance and recovery tuning

`move_ratelimit` drops all btree locks before waiting on HDD
backpressure so the wait cannot hold up lock holders; reconcile/move IO
raised from IDLE to best-effort priority (IDLE starves it completely
under any concurrent foreground IO); recovery pass timing stats for
mount analysis, with btree bitmap GC thresholds tuned (fragmentation
4x→2x, periodic check 24h→6h).

Commits: `ee59a08d52575`, `0ec56985a74ed`, `9b205e8b26361`

This repo primarily consists of the following:

- bcachefs tool, the reason this repo exists.
- {mkfs,mount,fsck}.bcachefs utils, which is just wrappers calling the corresponding subcommands
  in the main tool
- docs in the form of man-pages and a user manual

Please refer to the main site for [getting started](https://bcachefs.org/#Getting_started)
An in-depth user manual is (also) found on the [official website](https://bcachefs.org/#Documentation)

Version semantics
-----------------

The tools relies on an expected disk format structure which is reflected by your current kernel version.
Disk format can be upgraded or downgraded automatically by the kernel, if needed.

- Any patch-level change means no disk format change
- Any minor-level change means a potential disk format change which **is not breaking**
- Any major-level change means **breaking changes**

Build and install
-----------------

Refer to [INSTALL.md](./INSTALL.md)

Bug reports and contributions
-----------------------------

- GitHub issues for bug reports and focused feature requests
- GitHub Discussions for support questions and general usage discussion
- The official mailing list, linux-bcachefs@vger.kernel.org
- IRC: #bcache on OFTC (irc.oftc.net). Note that IRC messages can be easily missed.
