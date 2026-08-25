/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _BCACHEFS_JOURNAL_STALL_WATCHDOG_H
#define _BCACHEFS_JOURNAL_STALL_WATCHDOG_H

struct bch_fs;

int bch2_stall_watchdog_start(struct bch_fs *);
void bch2_stall_watchdog_stop(struct bch_fs *);
void bch2_stall_dump_stacks(struct bch_fs *);

#endif /* _BCACHEFS_JOURNAL_STALL_WATCHDOG_H */
