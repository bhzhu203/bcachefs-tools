# TL;DR (self-contained)

**Symptom**: On a self-built 7.1.5-uksm kernel (CONFIG_BCACHEFS_DEBUG=y), during a **large-scale snapshot deletion storm**, multiple unrelated bcachefs subsystems randomly crash: `kernel BUG at fs/bcachefs/btree/bkey_cmp.h:118` (reboots the machine), bset search-outside-node WARNs, btree topology errors, and read-only (RO) remounts.

**Root cause (confirmed)**: `__bch2_btree_node_relock()` (`fs/btree/locking.c:889`) re-acquires locks on the cached stale `path->l[level].b` pointer using only a `six_lock` sequence-number collision as proof of node identity — it **never checks `b->c.btree_id == path->btree_id`**. While a btree path sits in its "unlocked window" (transaction restarts, lazy-commit→restart, lock downgrades), the original node can be freed and its `struct btree` memory **recycled for a node of a different btree** that the same transaction/subsystem is allocating at high frequency (need_discard, alloc). The seq counter is inherited with the memory, so `six_relock_type()` happily succeeds on the foreign node and the path becomes **permanently attached to the wrong btree**. Everything downstream (unpacking/comparing keys against the wrong node's format/nr_key_bits) produces the observed WARNs/BUGs/topology errors.

**Hard evidence**: A probe placed right before `six_relock_type()` (compare btree_id; on mismatch, fail the relock and dump_stack) fired **4 times across 3 independent subsystems** in a single storm, and 2 of those stacks (copygc, discards) contain **zero custom code** — proving this is an upstream (Kent) bug, not a caller bug:

| Timestamp | path btree | relocked node btree | level | lock_seq | caller |
|---|---|---|---|---|---|
| 2209.160767 | 12 freespace_h | 13 need_discard | L0 | 186 | bch2_do_discards_work |
| 2211.331864 | 0 extents | 13 need_discard | **L1** | 9 | bch-copygc/vdb |
| 2727.381873 | 1 inodes | 4 alloc | L0 | 642 | snapshot deletion v2 (local batching) |
| 2734.779851 | 1 inodes | 4 alloc | L0 | 120 | ditto |

**Fix**: Add a ~6-line btree_id check in `__bch2_btree_node_relock()` before/after `six_relock_type()`; on mismatch `goto fail` (path retraverses from the root). Verified locally as a permanent guard: after each hit the path self-heals via root retraversal and the deletion storm continues (migrated 3830432 → 7293184 keys) with no RO, no BUG, machine alive.

**Why only under the storm**: The storm simultaneously produces (a) a huge number of unlocked path windows (transaction restarts, lazy-commit restarts, lock downgrades) and (b) massive `struct btree` free/realloc churn (need_discard/alloc/key-cache fills). Both together make seq collisions frequent. Normal workloads have few windows and little memory recycling, so they rarely collide.

**Verdict**: Upstream core bug. Our local 32-key batching (commit `1f47e67ff7653`) merely intensifies the memory recycling churn; it is not the culprit. The btree_id check should be submitted upstream to Kent.

---

# Evidence

## A. Wrong-btree relock probe hits (decisive)

Probe location: `fs/btree/locking.c`, `__bch2_btree_node_relock()`, before `six_relock_type()`: if `b->c.btree_id != path->btree_id` → `pr_err` + `dump_stack()` + `goto fail`. Module build fingerprint: `82e8a6ed7824802e96cfc8d58ef2d747843cd6d1`.

### A1. Discards subsystem (t=2209.160767)
```
bcachefs: relock on node of wrong btree (probe): path idx 1 btree 12 level 0 pos 348424447:46547:0 | node btree 13 level 0 lock_seq 186
CPU: 3 PID: 93361 Comm: kworker/u64:6 ... RIP: 0010:__bch2_btree_node_relock+0x1b4/0x2c0 [bcachefs]
Call Trace:
 btree_path_get_locks+0x62/0x90 [bcachefs]
 __bch2_trans_relock+0x11a/0x1c0 [bcachefs]
 bch2_btree_node_fill+0x275/0x2a0 [bcachefs]
 __bch2_btree_node_get+0x1b1/0x2b0 [bcachefs]
 bch2_btree_path_traverse_one+0x5b1/0x11b0 [bcachefs]
 bch2_btree_iter_peek_slot+0x8d/0x360 [bcachefs]
 btree_key_cache_fill+0x13d/0x2a0 [bcachefs]
 bch2_btree_path_traverse_cached+0x146/0x2e0 [bcachefs]
 bch2_discard_one_bucket+0x25b/0x520 [bcachefs]
 bch2_do_discards+0x99/0x1a0 [bcachefs]
 bch2_do_discards_work+0x10/0x30 [bcachefs]
 process_one_work+0x1c3/0x380
```
The path belongs to freespace_h (btree 12) yet relocked an L0 node of need_discard (btree 13).

### A2. Copygc subsystem (t=2211.331864)
```
bcachefs: relock on node of wrong btree (probe): path idx 2 btree 0 level 1 pos 2147483797:65320:4294958557 | node btree 13 level 1 lock_seq 9
CPU: 2 PID: 97341 Comm: bch-copygc/vdb ... RIP: 0010:__bch2_btree_node_relock+0x1b4/0x2c0 [bcachefs]
Call Trace:
 bch2_btree_path_relock_norestart+0x6d/0x90 [bcachefs]
 __btree_path_up_until_good_node+0x3f/0x230 [bcachefs]
 bch2_btree_path_traverse_one+0x14f/0x11b0 [bcachefs]
 __bch2_trans_begin+0x35/0x190 [bcachefs]
 bch2_trans_begin+0xe/0x20 [bcachefs]
 __bch2_move_data_phys+0x18b/0x1a0 [bcachefs]
 bch2_evacuate_bucket+0x539/0x950 [bcachefs]
 bch2_copygc+0x174/0x2f0 [bcachefs]
 bch2_copygc_thread+0xd7/0x150 [bcachefs]
 kthread+0xd2/0xe0
```
An **L1 (interior-node)** extents (btree 0) path relocked an L1 need_discard node — even interior levels drift, meaning the whole path detaches onto the wrong tree.

### A3/A4. Snapshot deletion v2 (t=2727.381873, t=2734.779851)
```
bcachefs: relock on node of wrong btree (probe): path idx 1 btree 1 level 0 pos 0:2305843009213694841:4294958555 | node btree 4 lock_seq 642
Call Trace:
 bch2_btree_path_relock_norestart+0x6d/0x90 [bcachefs]
 __btree_path_up_until_good_node+0x3f/0x230 [bcachefs]
 bch2_btree_path_traverse_one+0x14f/0x11b0 [bcachefs]
 __bch2_trans_begin+0x35/0x190 [bcachefs]
 bch2_trans_begin+0xe/0x20 [bcachefs]
 delete_dead_snapshot_keys_batched+0x60c/0x8d0 [bcachefs]      ← local batching code
 delete_dead_snapshot_keys_v2+0x3a6/0x6b0 [bcachefs]
 delete_dead_snapshots_locked+0x29/0x80 [bcachefs]
 __bch2_delete_dead_snapshots+0x1e0/0x2f0 [bcachefs]
 bch2_async_recovery_passes_work+0x1d/0x30 [bcachefs]
 process_one_work+0x1c3/0x380
```
An inodes (btree 1) path relocked an alloc (btree 4) node (lock_seq 642 / 120 — small numbers typical of freshly allocated nodes).

## B. Downstream symptoms (each maps to the wrong attach)

- `fs/bcachefs/btree/bset.c:1484` (`bch2_btree_node_iter_peek/peek_all` search outside node) WARNed repeatedly after demotion: binary search against the wrong btree's key format puts pos outside the node bounds. The demotion clamps to node bounds — a symptomatic mitigation only.
- `iter.c:930` "path outside node (probe)": after a transaction restart, path pos doesn't fit the leaf's bounds (e.g., inodes pos `0:2305843009213694841...` against an extents leaf) — the direct signature of a path on the wrong tree.
- `kernel BUG at fs/bcachefs/btree/bkey_cmp.h:118` (rebooted the machine while panic_on_oops=1): `__bkey_cmp_bits` comparing packed keys with the wrong node's `nr_key_bits`/format → out-of-bounds read/assert. The two EBUG_ONs there were demoted to WARN + fallback paths.
- Btree topology validation errors and read-only remounts are later cascade effects.

## C. Ruled-out alternatives (why it is nothing else)

1. **Not "missed lazy-commit restart"**: `commit.c:1544` explicitly does `if (lazy && !ret) ret = btree_trans_restart(trans, BCH_ERR_transaction_restart_commit);`, and the batching loop's restart handling (rewind via begin_may_drop_updates + set_pos(batch_start)) matches that contract.
2. **Not a rogue writer of `path->l[].b`**: with DEBUG=y the legitimate writer `bch2_btree_path_level_init` (iter.c:797) carries a live `EBUG_ON(!btree_path_pos_in_node(path, b))` which never fired; ERR_PTR writers are semantically fine; the memcpy block only copies paths. The only entry that bypasses all checks is relock on the cached pointer.
3. **Self-heal verified**: the probe converts each wrong relock into a relock failure → `__btree_path_up_until_good_node` retraverses from the root → the storm continues (migrated 3830432 → 7293184) with no RO/BUG/crash throughout.

---

# Detailed analysis

## 1. Faulting call chain (full causal chain)

```
[storm precondition] large-scale snapshot deletion:
  bch2_async_recovery_passes_work
   → __bch2_delete_dead_snapshots
    → delete_dead_snapshot_keys_v2        (walk inodes btree, upstream code)
     → delete_dead_snapshot_keys_batched  (one transaction per 32 keys, local commit 1f47e67ff7653)
      → commit every 32 keys + bch2_trans_begin → masses of unlocked path windows
  meanwhile discards/copygc threads constantly fill need_discard/alloc/freespace nodes
  meanwhile old nodes get bch2_btree_node_free → struct btree returned to slab

[accident window] the struct btree cached in some path->l[level].b is freed:
  1) node freed: bch2_btree_node_free → six lock released → struct btree to slab
  2) memory recycled: the same slab memory is picked by bch2_btree_node_alloc
     for a NEW node of a DIFFERENT btree (need_discard/alloc/...)
     b->c.btree_id = new tree id; b->c.lock.seq starts from leftover state
     (observed small seqs 9/120/186/642)
  3) wrong relock: on transaction restart / relock
       bch2_trans_begin
        → bch2_btree_path_traverse_one
         → __btree_path_up_until_good_node
          → bch2_btree_path_relock_norestart
           → __bch2_btree_node_relock(trans, path, level)
              six_relock_type(&b->c.lock, want, path->l[level].lock_seq)
              // only seq is checked! nobody knows b is now a foreign node
              // seq happens to reach the value the path recorded → lock succeeds
            → path attaches to the wrong btree's node (no btree_id validation anywhere)
  4) cascade: subsequent peek/advance unpacks keys with the wrong node's
     format/nr_key_bits
     → bset search outside node WARN (bset.c:1484)
     → bkey_cmp BUG (bkey_cmp.h:118)
     → topology validation failure / RO
```

Alternate entry (A1): `btree_key_cache_fill` triggers `bch2_btree_node_fill` (async read) → on return `__bch2_trans_relock` relocks all paths and hits the same window.

## 2. Code attribution

| Code | Owner |
|---|---|
| `__bch2_btree_node_relock` / six-lock relock machinery / `six_relock_type` | upstream (Kent), rebase base `abdbd61f320a8` |
| `delete_dead_snapshot_keys_v1/_v2/range` driver, `delete_dead_snapshots_process_key`, `bch2_delete_dead_snapshot_key` | upstream (Kent) |
| `delete_dead_snapshot_keys_batched` (32-key batching, +154/-18 lines) | local `1f47e67ff7653` (bhzhu203) |
| Trigger surface | copygc / discards stacks carry zero local code → no causal link to the batching; batching only amplifies memory churn |

## 3. Why the sequence number "collides"

`path->l[level].lock_seq` records the node's seq at the time the path held the lock. `six_relock_type`'s design premise is "same node": if the node was never freed, seq only moves forward and an old seq cannot re-enter; after the node is freed the seq restarts, and recycling places a new-tree node at the same address — so the stale `(pointer, lock_seq)` pair in the path "authenticates" successfully once the new node's seq reaches the recorded value. Upstream's `__bch2_btree_node_relock` performs neither (1) a btree_id check nor (2) a descendant-consistency check — an identity-verification gap.

## 4. Reproduction

1. Environment: self-built 7.1.5-uksm kernel (CONFIG_BCACHEFS_DEBUG=y, LOCK_TIME_STATS=y, PATH_TRACEPOINTS=y, panic_on_oops=0), bcachefs single device /dev/vdb mounted at /data, netconsole to a remote collector.
2. Workload: create multi-level snapshots and write several million keys (this run migrated >7M extent keys total), then delete a large batch of snapshots at once (deletion storm).
3. Observation: run netconsole capture; during the storm discards, copygc and recovery passes run concurrently. The wrong-relock probe fires multiple times per storm within a 5–10 minute window.
4. Without the probe: within minutes — bset.c:1484 WARNs, iter.c:930 pos-outside-node, bkey_cmp.h:118 BUG (fatal with panic_on_oops=1), topology errors/RO.

## 5. Fix

### Immediate fix (propose upstream, ~6 lines)
```c
bool __bch2_btree_node_relock(struct btree_trans *trans,
			      struct btree_path *path, unsigned level,
			      bool trace)
{
	struct btree *b = btree_path_node(path, level);
	int want = __btree_lock_want(path, level);
	...
	if (!six_relock_type(&b->c.lock, want, path->l[level].lock_seq))
		goto fail;
+	/*
+	 * The cached pointer may dangle if the node was freed and its struct
+	 * btree recycled for a node of a different btree; the seq match above
+	 * is not proof of identity. Fail the relock and retraverse from root.
+	 */
+	if (unlikely(b->c.btree_id != path->btree_id))
+		goto fail;
+
	return true;
fail:
	...
}
```
(The check also works placed before `six_relock_type`; failing takes the existing `fail` path — the only cost is one root retraversal.)

### Locally deployed mitigations (transitional guard)
- btree_id probe inside `__bch2_btree_node_relock`: pr_err + dump_stack + WARN_ONCE + goto fail (permanent).
- bset.c:1484 search-outside-node clamping fix (symptomatic).
- Two EBUG_ONs in bkey_cmp.h demoted to WARN + fallback comparison path.
- `/etc/sysctl.d/99-bcachefs-debug.conf`: `kernel.panic_on_oops=0`.
- Verified: probe hit → root retraversal self-heal → storm runs to completion.

### Follow-ups
- Restore the three demoted EBUG_ONs (bset.c, bkey_cmp.h) once the upstream fix lands.
- If upstream accepts the btree_id check, drop the probe's printing and keep the bare check.
