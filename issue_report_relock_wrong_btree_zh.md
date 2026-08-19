# 省流版（不看后面也能懂）

**问题**：bcachefs 内核模块（自建 7.1.5-uksm 内核，CONFIG_BCACHEFS_DEBUG=y）在**大规模快照删除风暴**期间，多个子系统随机崩溃：`kernel BUG at fs/bcachefs/btree/bkey_cmp.h:118`（重启机器）、bset 搜索越界 WARN、btree 拓扑错误、甚至只读（RO）挂载。

**根因（已实锤）**：`__bch2_btree_node_relock()`（`fs/btree/locking.c:889`）在用缓存的旧 `path->l[level].b` 指针重新加锁时，只凭 `six_lock` 序列号碰撞判定节点身份，**从不校验 `b->c.btree_id == path->btree_id`**。当 btree path 处于“已解锁窗口”时，原节点可被释放，其 `struct btree` 内存被**同事务/同子系统正在高频分配的另一个 btree 的节点复用**（如 need_discard、alloc）。序列号随内存一起继承，于是 `six_relock_type()` 在“别人家的节点”上成功加锁，path 从此**永久挂到错误的 btree**上，后续按错 btree 的 format/nr_key_bits 解包比较 → 一连串 WARN/BUG/拓扑错误。

**铁证**：加在 `six_relock_type()` 之前的探针（比对 btree_id，不一致即 fail 并 dump_stack）在一场删除风暴里命中 **4 次、横跨 3 个互不相干的子系统**，且其中 2 个（copygc、discards）的调用栈里**完全没有用户自定义代码**——证明这是上游（Kent）的 bug，不是调用方的问题：

| 时刻 | path 自身 btree | 误锁节点 btree | 层级 | lock_seq | 调用方 |
|---|---|---|---|---|---|
| 2209.160767 | 12 freespace_h | 13 need_discard | L0 | 186 | bch2_do_discards_work |
| 2211.331864 | 0 extents | 13 need_discard | **L1** | 9 | bch-copygc/vdb |
| 2727.381873 | 1 inodes | 4 alloc | L0 | 642 | 快照删除 v2（我方批量化） |
| 2734.779851 | 1 inodes | 4 alloc | L0 | 120 | 同上 |

**修复**：在 `__bch2_btree_node_relock()` 里 `six_relock_type()` 之前加 6 行 btree_id 校验，不匹配则 `goto fail`（让 path 从根重走）。已在本机作为常驻防线验证：命中后重走即自愈，删除风暴继续推进（migrated 3830432 → 7293184 keys），无 RO、无 BUG、机器存活。

**为什么风暴才触发**：删除风暴同时制造 (a) 大量 path 解锁窗口（事务重启、lazy commit 转重启、锁降级），(b) 大量 `struct btree` 释放/再分配（need_discard/alloc/键缓存回填），两者叠满后序列号碰撞概率陡增。普通负载窗口和换内存的频率都低，几乎碰不上。

**结论**：上游核心 bug；我方 32-key 批量化（commit `1f47e67ff7653`）只是加剧了内存回收轮转，不是肇事者。建议把该校验提交给 Kent。

---

# 证据

## A. 误锁探针命中（决定性证据）

探针位于 `fs/btree/locking.c` `__bch2_btree_node_relock()`，`six_relock_type()` 之前：若 `b->c.btree_id != path->btree_id` 则 `pr_err` + `dump_stack()` + `goto fail`。模块构建指纹：`82e8a6ed7824802e96cfc8d58ef2d747843cd6d1`。

### A1. discards 子系统（t=2209.160767）
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
path 是 freespace_h(btree 12)，却把 need_discard(btree 13) 的 L0 节点锁了回来。

### A2. copygc 子系统（t=2211.331864）
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
extents(btree 0) 的 **L1（内部节点）** path 误锁 need_discard 的 L1 节点——内部节点都被换树，说明整个 path 已彻底漂移。

### A3/A4. 快照删除 v2（t=2727.381873、t=2734.779851）
```
bcachefs: relock on node of wrong btree (probe): path idx 1 btree 1 level 0 pos 0:2305843009213694841:4294958555 | node btree 4 lock_seq 642
Call Trace:
 bch2_btree_path_relock_norestart+0x6d/0x90 [bcachefs]
 __btree_path_up_until_good_node+0x3f/0x230 [bcachefs]
 bch2_btree_path_traverse_one+0x14f/0x11b0 [bcachefs]
 __bch2_trans_begin+0x35/0x190 [bcachefs]
 bch2_trans_begin+0xe/0x20 [bcachefs]
 delete_dead_snapshot_keys_batched+0x60c/0x8d0 [bcachefs]      ← 本地批量化代码
 delete_dead_snapshot_keys_v2+0x3a6/0x6b0 [bcachefs]
 delete_dead_snapshots_locked+0x29/0x80 [bcachefs]
 __bch2_delete_dead_snapshots+0x1e0/0x2f0 [bcachefs]
 bch2_async_recovery_passes_work+0x1d/0x30 [bcachefs]
 process_one_work+0x1c3/0x380
```
inodes(btree 1) path 误锁 alloc(btree 4) 节点（lock_seq 分别 642/120——都是新分配节点的小序号）。

## B. 下游症状（与误锁一一对应）

- `fs/bcachefs/btree/bset.c:1484`（`bch2_btree_node_iter_peek/peek_all` 搜索越界）在 demote 后反复 WARN：按错误 btree 的 key format 在节点里二分，pos 落在节点边界之外。demote 时 clamp 到节点范围，属于症状级缓解。
- `iter.c:930` “path outside node (probe)”：事务重启后发现 path pos 与叶节点边界不符（如 inodes pos `0:2305843009213694841...` 对 extents 叶节点）——即挂错树后的直接表现。
- `kernel BUG at fs/bcachefs/btree/bkey_cmp.h:118`（曾直接重启机器，因 panic_on_oops=1）：`__bkey_cmp_bits` 按错误节点的 `nr_key_bits`/format 比较打包 key，读越界/断言失败。同处两个 EBUG_ON 已 demote 为 WARN + 降级路径。
- btree 拓扑校验错误、fs 只读等更晚期的连锁反应。

## C. 机制排除过程（为什么不是别的原因）

1. **不是“lazy commit 漏检重启”**：`commit.c:1544` 明确 `if (lazy && !ret) ret = btree_trans_restart(trans, BCH_ERR_transaction_restart_commit);`，且批处理循环对重启的处理（回退 begin_may_drop_updates + set_pos(batch_start)）与契约一致。
2. **不是 path->l[].b 写入方越权写**：DEBUG=y 下合法写入方 `bch2_btree_path_level_init`（iter.c:797）带 `EBUG_ON(!btree_path_pos_in_node(path, b))` 的活 BUG_ON，从未触发；ERR_PTR 写入方语义正常；memcpy 仅在 path 复制时发生。唯一绕过检查的入口就是 relock 拿缓存指针直接锁。
3. **自愈验证**：探针把误锁转为 relock 失败 → `__btree_path_up_until_good_node` 从根重走 → 风暴继续（migrated 3830432 → 7293184），全程无 RO/BUG/宕机。

---

# 详细分析

## 1. 出错调用链（完整因果链）

```
[风暴前提] 大规模快照删除:
  bch2_async_recovery_passes_work
   → __bch2_delete_dead_snapshots
    → delete_dead_snapshot_keys_v2        (遍历 inodes btree, 上游代码)
     → delete_dead_snapshot_keys_batched  (每 32 key 一事务, 本地 commit 1f47e67ff7653)
      → 每 32 key commit + bch2_trans_begin → 大量 path 解锁窗口
  同时 discards/copygc 线程高频填 need_discard/alloc/freespace 节点
  同时大量旧节点被 bch2_btree_node_free → struct btree 归还 slab

[事故窗口] 某 path 的 path->l[level].b 缓存的 struct btree 被释放:
  1) 节点释放: bch2_btree_node_free → 六锁释放 → struct btree 进 slab
  2) 内存复用: 同一 slab 挑中该内存, bch2_btree_node_alloc
     分配给另一个 btree 的新节点 (如 need_discard/alloc)
     b->c.btree_id = 新树 ID; b->c.lock.seq 从内存垃圾/残留初始化——
     序列号不大 (观测 9/120/186/642), 说明是从残留状态起步
  3) 误锁: 事务重启/重锁路径
       bch2_trans_begin
        → bch2_btree_path_traverse_one
         → __btree_path_up_until_good_node
          → bch2_btree_path_relock_norestart
           → __bch2_btree_node_relock(trans, path, level)
              six_relock_type(&b->c.lock, want, path->l[level].lock_seq)
              // 只看 seq 匹配! 不知道 b 已经是别的树的节点
              // seq 恰好追平 path 记录的 lock_seq → 加锁成功
            → path 挂上错误 btree 的节点 (无任何 btree_id 校验)
  4) 连锁: 后续 peek/advance 用错误节点的 format/nr_key_bits 解包
     → bset 搜索越界 WARN (bset.c:1484)
     → bkey_cmp BUG (bkey_cmp.h:118)
     → 拓扑校验失败 / RO
```

另一种入口（A1）：`btree_key_cache_fill` 触发 `bch2_btree_node_fill`（异步读）→ 返回时 `__bch2_trans_relock` 重锁全部 path，同样踩中。

## 2. 代码归属

| 代码 | 归属 |
|---|---|
| `__bch2_btree_node_relock` / 六锁重锁机制 / `six_relock_type` | 上游（Kent），rebase 基点 `abdbd61f320a8` |
| `delete_dead_snapshot_keys_v1/_v2/range` 驱动、`delete_dead_snapshots_process_key`、`bch2_delete_dead_snapshot_key` | 上游（Kent） |
| `delete_dead_snapshot_keys_batched`（32-key 批处理，+154/-18 行） | 本地 `1f47e67ff7653`（bhzhu203） |
| 触发面 | copygc / discards 栈零本地代码 → 与批量化无因果关系，批量化仅加剧内存轮转 |

## 3. 为什么序列号会“碰撞”

`path->l[level].lock_seq` 记录的是当初持锁时该节点的 seq。`six_relock_type` 的设计前提是“同一节点”：若节点未被释放过，seq 只会前进，旧 seq 不可重入；节点被释放后 seq 归零重来，而 struct btree 复用又把新树的节点放到同一地址——path 里残留的 `(指针, lock_seq)` 对在新节点 seq 追到该值时即“验明正身”失败却通过了。上游在 `__bch2_btree_node_relock` 里没有（1）btree_id 校验，（2）b 后代一致性校验，属于身份验证缺口。

## 4. 复现方法

1. 环境：7.1.5-uksm 自建内核（CONFIG_BCACHEFS_DEBUG=y、LOCK_TIME_STATS=y、PATH_TRACEPOINTS=y、panic_on_oops=0），bcachefs 单盘 /dev/vdb 挂 /data，netconsole 远端收集。
2. 造势：创建多层快照并灌入数百万 key（本例 extents 迁移总量 >700 万 key），随后一次性删除大批快照（删除风暴）。
3. 观测：同时跑 `kdump`/netconsole；风暴期间 discards、copygc、recovery pass 并发。误锁探针在 5–10 分钟窗口内以多次/场的频率命中。
4. 无探针时的表现：分钟级出现 bset.c:1484 WARN、iter.c:930 pos 越界、bkey_cmp.h:118 BUG（重启或 panic_on_oops=1 时直接死机）、拓扑错误/RO。

## 5. 修复

### 立即修复（建议提交上游，~6 行）
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
（校验放在 `six_relock_type` 成功之后亦可；失败走既有 `fail` 路径，代价只是一次根重走。）

### 本地已部署的缓解（过渡期防线）
- `__bch2_btree_node_relock` 内置 btree_id 探针：pr_err + dump_stack + WARN_ONCE + goto fail（常驻）。
- bset.c:1484 搜索越界 clamp 修复（症状级）。
- bkey_cmp.h 两个 EBUG_ON demote 为 WARN + 降级比较路径。
- `/etc/sysctl.d/99-bcachefs-debug.conf`: `kernel.panic_on_oops=0`。
- 已验证：探针命中 → 根重走自愈 → 风暴继续至完成。

### 遗留工作
- 三处 demoted EBUG_ON（bset.c、bkey_cmp.h×2/3 处）在上游修复合入后应恢复。
- 若上游接受 btree_id 校验，本地探针打印可移除，保留纯校验。
