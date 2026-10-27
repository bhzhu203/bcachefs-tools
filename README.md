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
rebased onto upstream/master (`84ece60f2`, 2026-10-24). The fork is 64
commits over upstream; the groups below list all of them except this
section's own documentation commits. The pre-rebase history is
preserved in this repo as the `backup-master-261026-pre-rust` branch.
This rebase follows upstream's latest Rust port (moving context, member
IO accounting, open-bucket and cuckoo table writers); the fork's copygc
backoff now runs on upstream's `MovingCtxt` and the cuckoo table's
serialized `insert_new()` writers.

### Snapshot deletion: throughput, pacing, and storm mitigation

Deleting a large snapshot (millions of keys) used to pin the system for
days: per-key transactions monopolized the journal and btree IO, copygc
was dragged into the invalidation storm, and data movers relocated
extents that were about to disappear anyway.

- **Batched migrations**: up to 1024 keys staged per transaction,
  bounded by a transaction-memory watermark that halves itself on ENOMEM
  and is remembered per-filesystem (module-param tunable divisor), and
  by a queued-update cap (128 for extents, 256 elsewhere): extent
  triggers allocate backpointer and disk accounting updates at commit,
  after the staging watermark is checked, so a byte-sized batch could
  still overflow the 64K transaction allocator. The migration-target
  iterator is kept alive and repositioned across keys instead of paying
  a fresh root-to-leaf lookup per key.
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
  reclaim cannot free until btree writes complete (`4b9823194`).
- **Btree path correctness found under the storm**: a cached path node
  pointer can dangle and a recycled struct btree be reused for a node
  of a different btree — the lock sequence number is not proof of
  identity. Upstream landed the root fix (`9dcc6ee31` forgets unlocked
  cached pointers to a node changing identity, `999690e20` asserts a
  relocked node is still the one the path had), so this rebase took
  upstream's copies and dropped the fork's.
- **Runtime toggle**: the `snapshot_delete_v2` module param selects the
  v2 scan vs the v1 per-key path.

Commits: `40717c9d8`, `8ba7037b7`, `a3317468b`, `313a59f6b`,
`1d9e7bea5`, `14672943e`, `673aa2d6f`, `e65c03655`, `b6417373c`,
`4b9823194`, `5c55bc30f`, `0ac1aff17`, `7c77d31c3`

### HDD livelock / journal deadlock defense

On HDD-only systems under heavy concurrent workloads (VMs, containers,
database servers), dozens to hundreds of processes could enter
uninterruptible D state for hours — `journal_max_open` stalls, load 50+
with unsaturated IO, no recovery without reboot. These mechanisms break
the cycles upstream still does not guard against:

- **Transaction admission control**: a semaphore throttle limiting
  concurrent btree transactions on rotational devices, split into a
  foreground (48 slots) and background (16 slots) pool. Workqueue
  workers (`btree_write_complete`, `write_buffer_flush`) draw from the
  background pool so they cannot be starved by foreground transactions
  whose writes they must complete; pure kthreads (reclaim, reconcile,
  copygc) and PF_MEMALLOC contexts bypass it, and acquisition uses
  `down_timeout()` with retry, so reclaim can never deadlock against
  throttled transactions. A per-process cap (default 8 concurrent
  slots, 251-way hash by thread-group id, sysfs-tunable) keeps one busy
  process from monopolizing the pool.
- **HDD lock-wait restart**: a btree lock waiter on a rotational device
  that has been waiting more than 30s restarts its transaction with
  `deadlock_waitlist_alloc`, breaking dependency cycles the lock-cycle
  detector cannot see.
- **Journal reservation timeout**: waiting for journal space retries
  with a 30s floor and then returns `journal_res_blocked` instead of
  sleeping forever; the index-update path retries that error and kicks
  reclaim, rather than surfacing internal congestion as an I/O error.
- **Restart storm throttle**: a transaction that keeps restarting
  without completing an iteration spins with no sleep (the restart path
  deliberately skips the resched check). After 100 consecutive
  restarts, `bch2_trans_begin()` now drops locks and sleeps a jiffy
  per iteration; any successful iteration resets the counter, so the
  throttle itself can never latch.
- **Background work throttling**: btree cascade-aware throttling for
  writeback and journal reclaim.
- **Journal-congestion write errors**: a write whose index update hit
  the journal-reservation timeout used to surface internal congestion
  to the application as a write I/O error (WiredTiger write errors
  observed while snapshot deletion stormed the journal). The index
  update now retries it, bounded, and when the journal is low on space
  kicks reclaim and waits on it instead of retrying blind (`791663c1d`).

Commits: `06673eb54`, `3f5a01185`, `e7eec077b`, `791663c1d`,
`2464e203f`

### Btree-cache throttle deadlock/livelock under snapshot deletion

Two failure shapes observed on HDD while a multi-million-key snapshot
deletion ran against a live database workload, both diagnosed via a
stall watchdog (the watchdog itself is not part of this rebase — see
below):

- **Deadlock (whole system unreachable)**: transactions parked in the
  btree-write dirty-ratio throttle (upstream's
  `bch2_trans_commit_btree_write_ratelimit`, which blocks commits when
  dirty btree nodes exceed 3/4 of the cache) kept holding this fork's
  transaction admission-control slots. Btree write completions
  (`btree_node_write_work`) are workqueue workers that take a
  transaction from the same background slot pool — the only path that
  turns dirty nodes clean — so with sleepers holding all slots the
  dirty ratio could never drop and the wait never ended. Fix: release
  the admission slot across the throttle wait and reacquire after
  (`bch2_trans_admission_release()`/`bch2_trans_admission_reacquire()`).
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
- **Stall watchdog**: upstream removed `fs/journal/stall_watchdog.c`
  and its plumbing in the Rust port, so it is not carried over. It was
  the diagnostic that captured both signatures above on an
  unreachable-over-SSH machine via netconsole; without it, such stalls
  can only be diagnosed after the fact.
- **`__GFP_RECLAIMABLE` scope fix**: flags reaching vmalloc/kvmalloc
  paths are stripped first — `vmalloc_fix_flags()` WARNs on
  unsupported flags; only pure kmalloc paths keep RECLAIMABLE.

Commits: `7857385b8`, `e84fa7b9e`, `de3ddb540`

### Fair btree transaction slot allocation

Per-process fair allocation of btree transaction slots, split into
foreground/background priority pools so background scanners cannot
starve interactive IO, with a sysfs-tunable per-process slot cap.

Commits: `577f5aa15`, `9f3401d2e`, `9fbdf153a`

### kcompactd0 / memory compaction stabilization

Btree node data buffers (256KB each, ~550MB on a typical HDD system)
landed in MIGRATE_UNMOVABLE pageblocks, making them immovable obstacles
for kcompactd0: ~40% compaction failure rate and kcompactd0 burning 64%
CPU in retry loops. Fixes:

- `__GFP_RECLAIMABLE` on btree node data pages and all other large/hot
  allocations (journal, bio, compression workspaces, key cache, write
  buffer, darray defaults) so the pages become reclaimable and the
  shrinker can free whole pageblocks. (Later scoped in `e84fa7b9e`
  to pure kmalloc paths only — vmalloc strips and WARNs on it; a final
  audit swept the missed NOIO sites — compressed-read bounce pages,
  the journal data bio, the debugfs node-read bio — in
  `23adb9902`.)
- `__GFP_NORETRY` on direct kmalloc paths so bcachefs never triggers
  direct compaction itself; raised shrinker seeks (sysfs-tunable) so
  kcompactd prefers reclaiming page cache over btree nodes; kmalloc
  path forced for btree node allocations.

Commits: `bee60573b`, `90f611039`, `f3e51ba1f`, `3a6ee7d4b`,
`60d2cd25a`, `4a2cbc42c`, `89b661400`, `c1f4e8dae`, `4465a5fb3`,
`24840799d`, `7695735f9`, `23adb9902`, `eea39a98c`
(The fork's shrinker-seeks sysfs commit is redundant: upstream's
`btree_cache_shrinker_seeks` is already a runtime option, so the
option is exposed automatically.)

### Device-aware (HDD) IO scheduling and cache policy

Btree cache and IO scheduling decisions keyed off the rotational flag:
device-aware cache retention and scheduling, relaxed HDD throttling for
single-process heavy IO (a lone VM no longer drags the rest of the
system into D state), `dev_readahead` changes taking effect at runtime,
and a fix for the `devs_rotational` bitmap being empty at init (upstream
lacks this; device-latency based timeouts silently misbehave without
it).

Commits: `61988f2ef`, `3f5a01185`, `1801101a7`, `37ab49d53`

### Read path: cold-cache first read, readahead, random-read detection

- Btree node prefetching on the read and readdir paths — eliminates
  cold-cache node fetches on first access to large directories. (The
  readdir flag never reached the iterator in the Rust port: it was fed
  to the peek helper, which ignores it; fixed in `eeea4357b`.)
- Shared btree iterator across the readahead window for better HDD NCQ
  utilization, snapshot ID resolved once per window. Errors and
  restarts in the shared loop now go through the plain read loop's
  protocol (`e7eb54b06`); the snapshot fetch is hoisted into the read
  loops (`f17d2244b`) — a restart out of a hoisted fetch skips the
  iterator's `CLASS()` initialization and would carry an uninitialized
  snapshot into the loop.
- Random-read detection with a call-local extent map cache, two-stream
  readahead tracking (avoids false random detection), snapshot fetch
  retry, and fixes for random-read suppression, extent cache staleness,
  and readahead folio leaks. (`narrow_crcs` was not ported: upstream
  removed the fields it keys on.)

Commits: `7384708b4`, `7f60d3f56`, `eeea4357b`, `c56ab415c`,
`ca292bd2c`, `b1ac3144d`, `e7eb54b06`, `f17d2244b`

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
  addition trips it instead of truncating silently (`0714e895e`).
- Correctness refinements: in-place writes to unwritten extents and
  fallocate's unwritten extent allocation are preserved; the DIO
  completion path puts its quota reservation back under
  `ei_quota_lock` — it is not atomic ops, two completions on one inode
  race a plain read-modify-write and the `BUG_ON` can trip
  (`ac1dff567`). The fork's per-DIO snapshot lookup was not ported:
  upstream's direct-IO read path was refactored and already resolves
  the snapshot correctly.

Commits: `d6b85c89a`, `c2f82a0ee`, `956d09f7a`, `8afb088b8`,
`0714e895e`, `ac1dff567`
(`e114d79e1` fixes the unwritten→written conversion failure under
nocow+ENOSPC natively via `BCH_TRANS_COMMIT_no_enospc`, so avoiding
unwritten extents is no longer needed.)

### Background maintenance and recovery tuning

`move_ratelimit` drops all btree locks before waiting on HDD
backpressure so the wait cannot hold up lock holders; on rotational
devices it waits only down to half the write limit (upstream waits to a
quarter), so a single heavy mover does not stall the whole device.
Recovery pass timing stats are recorded for mount analysis
(`40ae3bf88`). Upstream's
rewritten passes framework already handles failing-pass backoff and
explicit requests, so the fork's retry-cap and explicit-request
workarounds are not carried.

- **Writeback-timer self-deadlock**: `bch2_vfs_writeback_fn()` ends
  with an `iput()` on the reference it grabbed on entry; when that is
  the last reference, eviction runs inside the work itself and
  `bch2_evict_inode()`'s `cancel_delayed_work_sync()` waits on the
  very work executing it — the kworker parks in D state forever, and
  the ordered (max_active=1) writeback wq stalls with it
  (`12cba144f`).

Commits: `3f5a01185`, `40ae3bf88`, `12cba144f`

### Build and versioning

The kernel tree is not a git repo, so the in-tree copy's `git describe`
failed and the loaded module reported "git: unknown", which cost real
debugging time identifying the module from an OOM log.
`install-to-kernel.sh` now snapshots `git describe --always --dirty`
into `.bcachefs-git-version` in the copy (`5487267a4`), and the
Makefile prefers that snapshot over git — in a git-managed kernel tree
`git -C $(src) describe` finds the kernel repository and embeds its
version instead of bcachefs' (`f6a71e159`).

Commits: `5487267a4`, `f6a71e159`

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
