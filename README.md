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
media) deployments, layered on top of upstream bcachefs — currently
rebased onto upstream/master (`1507ece16`, 2026-10-15). The fork is 73
commits over upstream; the groups below list all of them except this
section's own documentation commits. The pre-rebase history is
preserved in this repo as the `backup-261015` branch and the
`pre-upstream-rebase-261015` tag.

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
  Both brakes yield to allocation starvation in part: a device whose
  free space sits at or under the normal reserve — the same line the
  allocator's blocked path parks waiters on — keeps its normal
  fragmentation allowance, and an unproductive run backs off at most 1s
  instead of the 30s maximum. It no longer skips the backoff entirely:
  when every candidate belongs to a deleted snapshot the tight loop
  makes no progress and only steals CPU and transactions from the
  delete pass that will free those extents. A finished deletion pass
  kicks copygc once, because the allocator only kicks copygc when a
  bucket is freed, which is exactly what is not happening while
  allocations are parked.
- **Bounded key-cache memory**: dirty key cache entries are capped at
  1M (~512MB) via the existing must_wait backpressure — alloc-key
  churn during deletion can otherwise pin gigabytes of slab that
  reclaim cannot free until btree writes complete (`c04ee0d236675`).
- **Btree path correctness found under the storm**: a cached path node
  pointer can dangle and a recycled struct btree be reused for a node
  of a different btree — the lock sequence number is not proof of
  identity. Upstream landed the root fix (`9dcc6ee31` forgets unlocked
  cached pointers to a node changing identity, `999690e20` asserts a
  relocked node is still the one the path had), so this rebase took
  upstream's copies and dropped the fork's.
- **Runtime toggle**: the `snapshot_delete_v2` module param selects the
  v2 scan vs the v1 per-key path.

Commits: `faf6789fc7cb3`, `79344313d8da4`, `8ab93b9ff92c2`,
`d28ab3b8c3028`, `121697881252f`, `7a6bdcec2d7bc`, `a5052bae3a1e0`,
`b1b86befc31d6`, `d9d357a1688e1`, `888eda53dd245`, `9d30f7ec6c8d9`,
`f767deb812879`, `c04ee0d236675`, `4717db4b1da0c`

### HDD livelock / journal deadlock defense

On HDD-only systems under heavy concurrent workloads (VMs, containers,
database servers), dozens to hundreds of processes could enter
uninterruptible D state for hours — `journal_max_open` stalls, load 50+
with unsaturated IO, no recovery without reboot. These mechanisms break
the cycles upstream still does not guard against:

- **Transaction admission control**: a semaphore throttle limiting
  concurrent btree transactions to 32 on rotational devices.
  PF_MEMALLOC and PF_WQ_WORKER contexts bypass it, and acquisition uses
  `down_timeout()` with retry, so reclaim and write-completion workers
  can never deadlock against throttled transactions.
- **Long-wait lock release**: every trans wait goes through
  `bch2_trans_short_wait_budget()`, which drops all btree locks via
  `bch2_trans_unlock_long()` once SRCU has been held ~1s. This covers
  dependency chains through non-lock resources (journal space, IO
  completion) that the lock cycle detector cannot see. It replaced an
  earlier 30-second btree-lock wait timeout in
  `bch2_six_check_for_deadlock()`, now removed: the restart it forced
  sets `trans->restarted`, which freezes the very timestamp the
  predicate reads (`trans_start_time` is only refreshed when
  `!trans->restarted`), so after one firing the waiter aborted *every*
  subsequent lock attempt instead of parking in the six_lock FIFO — on
  production this pinned journal reclaim at 100% CPU inside one key
  cache flush for ~20 hours and froze the whole filesystem.
- **Restart storm throttle**: a transaction that keeps restarting
  without completing an iteration spins with no sleep (the restart path
  deliberately skips the resched check). After 100 consecutive
  restarts, `bch2_trans_begin()` now drops locks and sleeps a jiffy
  per iteration; any successful iteration resets the counter, so the
  throttle itself can never latch.
- **Background work throttling**: btree cascade-aware throttling for
  writeback and journal reclaim, plus a revert of background IO tuning
  that worsened VM stalls.
- **Journal-congestion write errors**: a write whose index update hit
  the journal-reservation 30s timeout used to surface internal
  congestion to the application as a write I/O error (WiredTiger write
  errors observed while snapshot deletion stormed the journal). The
  index update now retries it, bounded, and when the journal is low on
  space kicks reclaim and waits on it instead of retrying blind
  (`5d9189ab32d73`).

Commits: `3490361f00401`, `a7eb1e3496cdb`, `846e9d42a610c`,
`ad519a2d26494`, `f025ab0fc7271`, `5d9189ab32d73`
(The journal-reservation 30s timeout added by the first commit is
still live: the pair that dropped and then reverted it was a net-zero
change in code upstream never touched, and was itself dropped in the
rebase.)

### Btree-cache throttle deadlock/livelock under snapshot deletion

Two failure shapes observed on HDD while a multi-million-key snapshot
deletion ran against a live database workload, both diagnosed via a new
stall watchdog and both fixed in `8e358dfa93e52`:

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
  Later extended to dump system-wide meminfo and key-cache counters,
  and to kick reclaim on the journal-stuck path (`6441bd034143b`).
- **`__GFP_RECLAIMABLE` scope fix**: flags reaching vmalloc/kvmalloc
  paths are stripped first — `vmalloc_fix_flags()` WARNs on
  unsupported flags; only pure kmalloc paths keep RECLAIMABLE.

Commits: `8e358dfa93e52`, `6441bd034143b`

### Fair btree transaction slot allocation

Per-process fair allocation of btree transaction slots, split into
foreground/background priority pools so background scanners cannot
starve interactive IO, with a sysfs-tunable per-process slot cap.

Commits: `d0beab65340a3`, `e64dfb5cffe14`, `77eb7a85f973f`

### kcompactd0 / memory compaction stabilization

Btree node data buffers (256KB each, ~550MB on a typical HDD system)
landed in MIGRATE_UNMOVABLE pageblocks, making them immovable obstacles
for kcompactd0: ~40% compaction failure rate and kcompactd0 burning 64%
CPU in retry loops. Fixes:

- `__GFP_RECLAIMABLE` on btree node data pages and all other large/hot
  allocations (journal, bio, compression workspaces, key cache, write
  buffer, darray defaults) so the pages become reclaimable and the
  shrinker can free whole pageblocks. (Later scoped in `8e358dfa93e52`
  to pure kmalloc paths only — vmalloc strips and WARNs on it; a final
  audit swept the missed NOIO sites — compressed-read bounce pages,
  the journal data bio, the debugfs node-read bio — in
  `05bc323e80a35`.)
- `__GFP_NORETRY` on direct kmalloc paths so bcachefs never triggers
  direct compaction itself; raised shrinker seeks (sysfs-tunable) so
  kcompactd prefers reclaiming page cache over btree nodes; kmalloc
  path forced for btree node allocations.

Commits: `474473fbcee22`, `345636c9555ca`, `0effaa42d7835`,
`7e063a059dcf4`, `287ede1c10232`, `26c1fe0b422df`, `9827957b1eb3f`,
`fb17674a920c5`, `864fc6bf7596c`, `1332282b1cc19`, `70e377a3a8c1b`,
`05bc323e80a35`, `1bf725fc02c33`
(`1bf725fc02c33` restores the `container_of()` lookup that upstream's
journal-replay shrinker writeback call needs — the kcompactd count
change had dropped it.)

### Device-aware (HDD) IO scheduling and cache policy

Btree cache and IO scheduling decisions keyed off the rotational flag:
device-aware cache retention and scheduling, relaxed HDD throttling for
single-process heavy IO (a lone VM no longer drags the rest of the
system into D state), `dev_readahead` changes taking effect at runtime,
and a fix for the `devs_rotational` bitmap being empty at init (upstream
lacks this; device-latency based timeouts silently misbehave without
it).

Commits: `18d0aea1a5057`, `e5705d122157a`, `97dc37ef4c12e`,
`3f50a850337f8`

### Read path: cold-cache first read, readahead, random-read detection

- Btree node prefetching on the read and readdir paths — eliminates
  cold-cache node fetches on first access to large directories. (The
  readdir flag never reached the iterator in the Rust port: it was fed
  to the peek helper, which ignores it; fixed in `25760530e15e7`.)
- Shared btree iterator across the readahead window for better HDD NCQ
  utilization, snapshot ID resolved once per window. Errors and
  restarts in the shared loop now go through the plain read loop's
  protocol (`967f10ceb986c`); a hoist of the snapshot fetch out of the
  plain read loops was reverted (`c7e1db526b9cf`) — a restart out of
  the hoisted fetch skipped the iterator's `CLASS()` initialization
  and carried an uninitialized snapshot into the loop.
- Random-read detection with an extent map cache, HDD `narrow_crcs`,
  two-stream readahead tracking (avoids false random detection), and
  fixes for random-read suppression, extent cache staleness, and
  readahead folio leaks.

Commits: `c1d621e9f7eda`, `c0482666ef4b4`, `17e351793b552`,
`e7e3b88aa33f9`, `e46e88f4e2c04`, `89cc9c6126620`, `0fb3f980a98d1`,
`967f10ceb986c`, `25760530e15e7`, `c7e1db526b9cf`

### nocow write path: throughput and correctness

- Nocow trylock fast path taken while still holding btree locks (bucket
  stale check skipped under lock), an expanded nocow lock table
  (1024→4096 buckets) to cut hash-collision waits, single-replica fast
  submit, and per-IO overhead reduction on the nocow DIO path.
- **Deferred reservation was silently dead**: the nocow DIO path sets
  the 17th write flag, `defer_reservation` (index 16) — one past the
  u16 that held the flags, so `BIT(16)` truncated to 0 and both the
  set and the check were no-ops: those writes ran with no reservation
  and no ENOSPC check. `bch_write_op.flags` is now u32, with the
  `BUILD_BUG_ON` pointed at the highest-index flag so the next
  addition trips it instead of truncating silently (`637d459a52a0e`).
- Correctness refinements: the snapshot ID is resolved per direct IO
  instead of cached (a stale cached id could send in-place writes into
  snapshot-shared blocks); in-place writes to unwritten extents and
  fallocate's unwritten extent allocation are preserved; the DIO
  completion path puts its quota reservation back under
  `ei_quota_lock` — it is not atomic ops, two completions on one inode
  race a plain read-modify-write and the `BUG_ON` can trip
  (`4d310a81ed538`).

Commits: `fad4743a81d47`, `132841049c8f4`, `896d38ef276a9`,
`dc53e2dd20282`, `3090fe1a5ed60`, `637d459a52a0e`, `4d310a81ed538`
(`dd71afb57a127` — this fork's "disable unwritten extents under nocow"
ENOSPC workaround — was fully reverted by `dc53e2dd20282` +
`3090fe1a5ed60`: upstream's `BCH_TRANS_COMMIT_no_enospc` fixes the
unwritten→written conversion failure natively, so avoiding unwritten
extents is no longer needed.)

### Background maintenance and recovery tuning

`move_ratelimit` drops all btree locks before waiting on HDD
backpressure so the wait cannot hold up lock holders; reconcile/move IO
raised from IDLE to best-effort priority (IDLE starves it completely
under any concurrent foreground IO); recovery pass timing stats for
mount analysis, with btree bitmap GC thresholds tuned (fragmentation
4x→2x, periodic check 24h→6h).

- **Failing-pass retry cap**: upstream retries a failing pass on an
  exponential backoff (2^n × runtime, capped at 20×); the fork caps
  the window at an hour and lets an explicit request — an fsck, a
  sysfs trigger, a dependent pass — clear the in-memory backoff
  immediately, so a repair request is never swallowed by automatic
  backoff (`53c1912c74e4a`, `cc4cfdb43677c`). Production case:
  `delete_dead_snapshots` failed after running 766s under a
  journal-full storm and the uncapped backoff left it unscheduled for
  21.28 hours — automatic recovery never retried, and the work only
  happened because the operator poked the sysfs trigger by hand.
- **Writeback-timer self-deadlock**: `bch2_vfs_writeback_fn()` ends
  with an `iput()` on the reference it grabbed on entry; when that is
  the last reference, eviction runs inside the work itself and
  `bch2_evict_inode()`'s `cancel_delayed_work_sync()` waits on the
  very work executing it — the kworker parks in D state forever, and
  the ordered (max_active=1) writeback wq stalls with it
  (`5ae02e80909ef`).

Commits: `08551ae1fa61a`, `c5316a39a61d8`, `91ee1bd59cc40`,
`53c1912c74e4a`, `cc4cfdb43677c`, `5ae02e80909ef`

### Build and versioning

The kernel tree is not a git repo, so the in-tree copy's `git describe`
failed and the loaded module reported "git: unknown", which cost real
debugging time identifying the module from an OOM log.
`install-to-kernel.sh` now snapshots `git describe --always --dirty`
into `.bcachefs-git-version` in the copy (`5b0b88b62ddc9`), and the
Makefile prefers that snapshot over git — in a git-managed kernel tree
`git -C $(src) describe` finds the kernel repository and embeds its
version instead of bcachefs' (`cb795128d013f`).

Commits: `5b0b88b62ddc9`, `cb795128d013f`

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
