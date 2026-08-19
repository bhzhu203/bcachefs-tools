// SPDX-License-Identifier: GPL-2.0
/*
 * Write the kernel log to a file on the root filesystem at oops/panic time.
 *
 * Netconsole loses the oops tail on this platform (the kexec into the
 * capture kernel happens before virtio-net can drain its TX ring), and
 * kdump needs a fully working capture kernel. The oops itself usually
 * runs in process context with interrupts enabled, so writing to the
 * ext4 root filesystem there is legal, and the file survives the reboot
 * that follows, carrying the full oops text the consoles never emitted.
 */

#include <linux/file.h>
#include <linux/fs.h>
#include <linux/hardirq.h>
#include <linux/irqflags.h>
#include <linux/kernel.h>
#include <linux/kmsg_dump.h>
#include <linux/ktime.h>
#include <linux/moduleparam.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#include "oops_log.h"

#define BCH_OOPS_CHUNK		SZ_256K

static bool bch_oops_log_enabled;
module_param_named(oops_log_dump, bch_oops_log_enabled, bool, 0644);
MODULE_PARM_DESC(oops_log_dump,
		 "Write the kernel log to /var/log/bcachefs-oops-<ts>.txt at oops/panic time (default off)");

static struct kmsg_dumper bch_oops_dumper;
static char *bch_oops_buf;

static void bch_oops_dump(struct kmsg_dumper *d, struct kmsg_dump_detail *detail)
{
	struct kmsg_dump_iter iter;
	struct file *f;
	char path[64];
	size_t len;
	loff_t pos = 0;
	unsigned int saved_flags;
	int hdr;

	if (!READ_ONCE(bch_oops_log_enabled))
		return;

	if (detail->reason != KMSG_DUMP_OOPS &&
	    detail->reason != KMSG_DUMP_PANIC)
		return;

	/*
	 * File IO can sleep: only attempt it from process context with
	 * interrupts enabled and no atomic locks held; otherwise leave
	 * capture to kdump and the serial console.
	 */
	if (!bch_oops_buf || in_interrupt() || irqs_disabled() || in_atomic())
		return;

	snprintf(path, sizeof(path), "/var/log/bcachefs-oops-%llu.txt",
		 (u64)ktime_get_real_seconds());

	f = filp_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (IS_ERR(f))
		return;

	/* never write into bcachefs itself from a bcachefs oops */
	if (strcmp(f->f_inode->i_sb->s_type->name, "bcachefs") == 0) {
		filp_close(f, NULL);
		return;
	}

	saved_flags = current->flags;
	current->flags |= PF_MEMALLOC;

	hdr = snprintf(bch_oops_buf, BCH_OOPS_CHUNK,
		       "bcachefs oops capture: reason=%s ts=%llu\n",
		       kmsg_dump_reason_str(detail->reason),
		       (u64)ktime_get_real_seconds());
	if (hdr > 0)
		kernel_write(f, bch_oops_buf, hdr, &pos);

	kmsg_dump_rewind(&iter);
	while (kmsg_dump_get_buffer(&iter, false, bch_oops_buf,
				    BCH_OOPS_CHUNK, &len) && len)
		kernel_write(f, bch_oops_buf, len, &pos);

	vfs_fsync(f, 0);
	current->flags = saved_flags;
	filp_close(f, NULL);
}

int __init bch2_oops_log_init(void)
{
	bch_oops_buf = kmalloc(BCH_OOPS_CHUNK, GFP_KERNEL);
	if (!bch_oops_buf)
		return -ENOMEM;

	bch_oops_dumper.max_reason = KMSG_DUMP_OOPS;
	bch_oops_dumper.dump = bch_oops_dump;
	return kmsg_dump_register(&bch_oops_dumper);
}

void bch2_oops_log_exit(void)
{
	kmsg_dump_unregister(&bch_oops_dumper);
	kfree(bch_oops_buf);
	bch_oops_buf = NULL;
}
