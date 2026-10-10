// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2019 MediaTek Inc.
 */

#include <linux/slab.h>
#include <linux/proc_fs.h>
#include <linux/reboot.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/string.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/kernel.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <asm/memory.h>
#include <linux/of_fdt.h>
#include <linux/kmsg_dump.h>

#include "log_store_kernel.h"
#include "upmu_common.h"

static struct sram_log_header *sram_header;
static int sram_log_store_status = BUFF_NOT_READY;
static int dram_log_store_status = BUFF_NOT_READY;
static char *pbuff;
static struct pl_lk_log *dram_curlog_header;
static struct dram_buf_header *sram_dram_buff;
static bool early_log_disable;
struct proc_dir_entry *entry;
static u32 last_boot_phase;


#define EXPDB_PATH "/dev/block/by-name/expdb"

#define LOG_BLOCK_SIZE (512)
#define EXPDB_LOG_SIZE (2*1024*1024)
#define KERNEL_LOG_SNAPSHOT_SIZE (256*1024)

static DEFINE_MUTEX(log_store_emmc_lock);

#if 0
def CONFIG_MTK_PMIC_COMMON
u32 set_pmic_boot_phase(u32 boot_phase)
{
	u32 ret;

	boot_phase = boot_phase & BOOT_PHASE_MASK;
	ret = pmic_config_interface(0xA0E, boot_phase, BOOT_PHASE_MASK,
		PMIC_BOOT_PHASE_SHIFT);
	return ret;
}


u32 get_pmic_boot_phase(void)
{
	u32 value = 0, ret;

	ret = pmic_read_interface(0xA0E, &value, BOOT_PHASE_MASK,
		PMIC_LAST_BOOT_PHASE_SHIFT);
	if (ret == 0)
		last_boot_phase = value;
	return value;
}
#endif

/* 6.6 dropped get_fs()/set_fs() and the ksys_* in-kernel syscall
 * shims; reading/writing the expdb partition has to go through the
 * file_operations API directly.  Wrap filp_open + kernel_read/write
 * so the four EMMC IO functions below stay readable.
 */
static struct file *expdb_open(void)
{
	struct file *fp = filp_open(EXPDB_PATH, O_RDWR, 0);

	if (IS_ERR(fp)) {
		pr_notice("log_store can't open expdb: %ld\n", PTR_ERR(fp));
		return NULL;
	}
	return fp;
}

static loff_t expdb_size(struct file *fp)
{
	return vfs_llseek(fp, 0, SEEK_END);
}

static ssize_t expdb_read_at(struct file *fp, loff_t pos, void *buf, size_t n)
{
	return kernel_read(fp, buf, n, &pos);
}

static ssize_t expdb_write_at(struct file *fp, loff_t pos,
		const void *buf, size_t n)
{
	return kernel_write(fp, buf, n, &pos);
}

static void expdb_close(struct file *fp)
{
	filp_close(fp, NULL);
}

/* set the flag whether store log to emmc in next boot phase in pl */
void store_log_to_emmc_enable(bool value)
{

	if (!sram_dram_buff) {
		pr_notice("%s: sram dram buff is NULL.\n", __func__);
		return;
	}

	if (value) {
		sram_dram_buff->flag |= NEED_SAVE_TO_EMMC;
	} else {
		sram_dram_buff->flag &= ~NEED_SAVE_TO_EMMC;
		sram_header->reboot_count = 0;
		sram_header->save_to_emmc = 0;
	}

	pr_notice(
		"log_store: sram_dram_buff flag 0x%x, reboot count %d, %d.\n",
		sram_dram_buff->flag, sram_header->reboot_count,
		sram_header->save_to_emmc);
}

void set_boot_phase(u32 step)
{
	struct file *fp;
	loff_t file_size;
	loff_t hdr_off;
	struct log_emmc_header pEmmc;

#if 0
def CONFIG_MTK_PMIC_COMMON
	if (sram_header->reserve[SRAM_PMIC_BOOT_PHASE] == FLAG_ENABLE) {
		set_pmic_boot_phase(step);
		if (last_boot_phase == 0)
			get_pmic_boot_phase();
	}
#endif

	if ((sram_dram_buff->flag & NEED_SAVE_TO_EMMC) == NEED_SAVE_TO_EMMC) {
		pr_notice("log_store: set boot phase, last boot phase is %d.\n",
		last_boot_phase);
		pr_notice("log_store: not boot up, don't store log to expdb  ");
		return;
	}

	fp = expdb_open();
	if (!fp)
		return;

	memset(&pEmmc, 0, sizeof(pEmmc));
	file_size = expdb_size(fp);
	hdr_off = file_size - sram_header->reserve[1];
	expdb_read_at(fp, hdr_off, &pEmmc, sizeof(pEmmc));
	if (pEmmc.sig != LOG_EMMC_SIG) {
		pr_notice("log_store emmc header error, format it.\n");
		memset(&pEmmc, 0, sizeof(pEmmc));
		pEmmc.sig = LOG_EMMC_SIG;
	} else if (last_boot_phase == 0)
		// get last boot phase
		last_boot_phase = (pEmmc.reserve_flag[BOOT_STEP] >>
			LAST_BOOT_PHASE_SHIFT) & BOOT_PHASE_MASK;

	// clear now boot phase
	pEmmc.reserve_flag[BOOT_STEP] &= (BOOT_PHASE_MASK <<
		LAST_BOOT_PHASE_SHIFT);
	// set boot phase
	pEmmc.reserve_flag[BOOT_STEP] |= (step << NOW_BOOT_PHASE_SHIFT);
	expdb_write_at(fp, hdr_off, &pEmmc, sizeof(pEmmc));
	expdb_close(fp);
	pr_notice("log_store: set boot phase, last boot phase is %d.\n",
		last_boot_phase);
}

u32 get_last_boot_phase(void)
{
	return last_boot_phase;
}

void log_store_bootup(void)
{
	/* Boot up finish, don't save log to emmc in next boot.*/
	store_log_to_emmc_enable(false);
	set_boot_phase(BOOT_PHASE_ANDROID);
}

void log_store_to_emmc(void)
{
	struct kmsg_dump_iter iter = { };
	struct emmc_log kernel_log_config = { };
	struct log_emmc_header pEmmc;
	struct file *fp = NULL;
	char *buff = NULL;
	size_t footer_size, data_size;
	size_t len = 0;
	loff_t file_size, data_off, hdr_off, idx_off;
	ssize_t size;
	u32 index;

	mutex_lock(&log_store_emmc_lock);

	if (!sram_header)
		goto out;

	footer_size = READ_ONCE(sram_header->reserve[SRAM_BLOCK_SIZE]);
	if (footer_size < sizeof(pEmmc) +
			  HEADER_INDEX_MAX * sizeof(struct emmc_log) ||
	    footer_size > EXPDB_LOG_SIZE) {
		pr_notice("log_store invalid expdb footer size:0x%zx.\n",
			  footer_size);
		goto out;
	}
	data_size = EXPDB_LOG_SIZE - footer_size;

	buff = vzalloc(KERNEL_LOG_SNAPSHOT_SIZE);
	if (!buff)
		goto out;

	kmsg_dump_rewind(&iter);
	if (!kmsg_dump_get_buffer(&iter, true, buff,
				  KERNEL_LOG_SNAPSHOT_SIZE, &len) || !len)
		goto out;

	fp = expdb_open();
	if (!fp)
		goto out;

	memset(&pEmmc, 0, sizeof(pEmmc));
	file_size = expdb_size(fp);
	if (file_size < EXPDB_LOG_SIZE || (loff_t)footer_size > file_size) {
		pr_notice("log_store invalid expdb size:%lld.\n", file_size);
		goto out;
	}

	data_off = file_size - EXPDB_LOG_SIZE;
	hdr_off = file_size - footer_size;
	size = expdb_read_at(fp, hdr_off, &pEmmc, sizeof(pEmmc));
	if (size != sizeof(pEmmc)) {
		pr_notice("log_store read expdb header failed:%zd.\n", size);
		goto out;
	}
	if (pEmmc.sig != LOG_EMMC_SIG) {
		pr_notice("log_store emmc header error:0x%x.\n", pEmmc.sig);
		goto out;
	}

	if (len > data_size) {
		pr_notice("log_store kernel snapshot too large:%zu.\n", len);
		goto out;
	}
	if (pEmmc.offset >= data_size || pEmmc.offset > data_size - len)
		pEmmc.offset = 0;

	kernel_log_config.start = pEmmc.offset;
	size = expdb_write_at(fp, data_off + pEmmc.offset, buff, len);
	if (size != len) {
		pr_notice("log_store write kernel snapshot failed:%zd.\n", size);
		goto out;
	}
	pEmmc.offset += len;
	kernel_log_config.end = pEmmc.offset;
	kernel_log_config.type = LOG_LAST_KERNEL;
	index = pEmmc.reserve_flag[LOG_INDEX] % HEADER_INDEX_MAX;
	idx_off = hdr_off + sizeof(struct log_emmc_header) +
		index * sizeof(struct emmc_log);
	if (idx_off > file_size - sizeof(struct emmc_log))
		goto out;

	size = expdb_write_at(fp, idx_off, &kernel_log_config,
			      sizeof(kernel_log_config));
	if (size != sizeof(kernel_log_config)) {
		pr_notice("log_store write snapshot index failed:%zd.\n", size);
		goto out;
	}
	pEmmc.reserve_flag[LOG_INDEX] = (index + 1) % HEADER_INDEX_MAX;
	size = expdb_write_at(fp, hdr_off, &pEmmc, sizeof(pEmmc));
	if (size != sizeof(pEmmc)) {
		pr_notice("log_store write expdb header failed:%zd.\n", size);
		goto out;
	}
	pr_notice("log_store wrote %zu-byte kernel snapshot.\n", len);

out:
	if (fp)
		expdb_close(fp);
	vfree(buff);
	mutex_unlock(&log_store_emmc_lock);
}

static int log_store_reboot(struct notifier_block *nb, unsigned long action,
			    void *data)
{
	if (action == SYS_RESTART)
		log_store_to_emmc();

	return NOTIFY_DONE;
}

static struct notifier_block log_store_reboot_nb = {
	.notifier_call = log_store_reboot,
};

int set_emmc_config(int type, int value)
{
	struct file *fp;
	struct log_emmc_header pEmmc;
	loff_t file_size, hdr_off;

	if (type >= EMMC_STORE_FLAG_TYPE_NR || type < 0) {
		pr_notice("invalid config type: %d.\n", type);
		return -1;
	}

	fp = expdb_open();
	if (!fp)
		return -1;

	memset(&pEmmc, 0, sizeof(pEmmc));
	file_size = expdb_size(fp);
	hdr_off = file_size - sram_header->reserve[1];
	expdb_read_at(fp, hdr_off, &pEmmc, sizeof(pEmmc));
	if (pEmmc.sig != LOG_EMMC_SIG) {
		pr_notice("log_store emmc header error.\n");
		expdb_close(fp);
		return -1;
	}

	if (type == UART_LOG || type == PRINTK_RATELIMIT ||
		type == KEDUMP_CTL) {
		if (value)
			pEmmc.reserve_flag[type] = FLAG_ENABLE;
		else
			pEmmc.reserve_flag[type] = FLAG_DISABLE;
	} else {
		pEmmc.reserve_flag[type] = value;
	}
	expdb_write_at(fp, file_size - LOG_BLOCK_SIZE, &pEmmc, sizeof(pEmmc));
	expdb_close(fp);
	pr_notice("type:%d, value:%d.\n", type, value);
	return 0;
}

int read_emmc_config(struct log_emmc_header *log_header)
{
	struct file *fp;
	loff_t file_size, hdr_off;

	fp = expdb_open();
	if (!fp)
		return -1;

	file_size = expdb_size(fp);
	hdr_off = file_size - sram_header->reserve[1];
	expdb_read_at(fp, hdr_off, log_header, sizeof(struct log_emmc_header));
	if (log_header->sig != LOG_EMMC_SIG) {
		pr_notice("log_store emmc header error.\n");
		expdb_close(fp);
		return -1;
	}
	expdb_close(fp);
	return 0;
}

static void *remap_lowmem(phys_addr_t start, phys_addr_t size)
{
	struct page **pages;
	phys_addr_t page_start;
	unsigned int page_count;
	pgprot_t prot;
	unsigned int i;
	void *vaddr;

	page_start = start - offset_in_page(start);
	page_count = DIV_ROUND_UP(size + offset_in_page(start), PAGE_SIZE);

	prot = pgprot_noncached(PAGE_KERNEL);

	pages = kmalloc_array(page_count, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return NULL;

	for (i = 0; i < page_count; i++) {
		phys_addr_t addr = page_start + i * PAGE_SIZE;

		pages[i] = pfn_to_page(addr >> PAGE_SHIFT);
	}
	vaddr = vmap(pages, page_count, VM_MAP, prot);
	kfree(pages);
	if (!vaddr) {
		pr_notice("%s: Failed to map %u pages\n", __func__, page_count);
		return NULL;
	}

	return vaddr + offset_in_page(start);
}

static int pl_lk_log_show(struct seq_file *m, void *v)
{
	if (dram_curlog_header == NULL || pbuff == NULL) {
		seq_puts(m, "log buff is null.\n");
		return 0;
	}

	seq_printf(m, "show buff sig 0x%x, size 0x%x,pl size 0x%x, lk size 0x%x, last_boot step 0x%x!\n",
			dram_curlog_header->sig, dram_curlog_header->buff_size,
			dram_curlog_header->sz_pl, dram_curlog_header->sz_lk, last_boot_phase);

	if (dram_log_store_status == BUFF_READY)
		if (dram_curlog_header->buff_size >= (dram_curlog_header->off_pl
		+ dram_curlog_header->sz_pl
		+ dram_curlog_header->sz_lk))
			seq_write(m, pbuff+dram_curlog_header->off_pl,
				dram_curlog_header->sz_lk
				+ dram_curlog_header->sz_pl);

	return 0;
}


static int pl_lk_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, pl_lk_log_show, inode->i_private);
}

static ssize_t pl_lk_file_write(struct file *filp,
	const char *ubuf, size_t cnt, loff_t *data)
{
	char buf[64];
	long val;
	int ret;

	if (cnt >= sizeof(buf))
		return -EINVAL;

	if (copy_from_user(&buf, ubuf, cnt))
		return -EFAULT;

	buf[cnt] = 0;

	ret = kstrtoul(buf, 10, (unsigned long *)&val);

	if (ret < 0)
		return ret;

	switch (val) {
	case 0:
		log_store_bootup();
		break;

	default:
		break;
	}
	return cnt;
}

/* 6.6 proc_create() takes struct proc_ops, not file_operations. */
static const struct proc_ops pl_lk_file_ops = {
	.proc_open = pl_lk_file_open,
	.proc_write = pl_lk_file_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};


static int __init log_store_late_init(void)
{
	set_boot_phase(BOOT_PHASE_KERNEL);
	if (sram_dram_buff == NULL) {
		pr_notice("log_store: sram header DRAM buff is null.\n");
		dram_log_store_status = BUFF_ALLOC_ERROR;
		return -1;
	}

	if (!sram_dram_buff->buf_addr || !sram_dram_buff->buf_size) {
		pr_notice("log_store: DRAM buff is null.\n");
		dram_log_store_status = BUFF_ALLOC_ERROR;
		return -1;
	}
	pr_notice("log store:sram_dram_buff addr 0x%x, size 0x%x.\n",
		sram_dram_buff->buf_addr, sram_dram_buff->buf_size);

	pbuff = remap_lowmem(sram_dram_buff->buf_addr,
		sram_dram_buff->buf_size);
	pr_notice("[PHY layout]log_store_mem:0x%08llx-0x%08llx (0x%llx)\n",
			(unsigned long long)sram_dram_buff->buf_addr,
			(unsigned long long)sram_dram_buff->buf_addr
			+ sram_dram_buff->buf_size - 1,
			(unsigned long long)sram_dram_buff->buf_size);
	if (!pbuff) {
		pr_notice("log_store: ioremap_wc failed.\n");
		dram_log_store_status = BUFF_ERROR;
		return -1;
	}

/* check buff flag */
	if (dram_curlog_header->sig != LOG_STORE_SIG) {
		pr_notice("log store: log sig: 0x%x.\n",
			dram_curlog_header->sig);
		dram_log_store_status = BUFF_ERROR;
		return 0;
	}

	dram_log_store_status = BUFF_READY;
	pr_notice("buff %p, sig %x size %x pl %x, sz %x lk %x, sz %x p %x, l %x\n",
		pbuff, dram_curlog_header->sig,
		dram_curlog_header->buff_size,
		dram_curlog_header->off_pl, dram_curlog_header->sz_pl,
		dram_curlog_header->off_lk, dram_curlog_header->sz_lk,
		dram_curlog_header->pl_flag, dram_curlog_header->lk_flag);

	entry = proc_create("pl_lk", 0664, NULL, &pl_lk_file_ops);
	if (!entry) {
		pr_notice("log_store: failed to create proc entry\n");
		return 1;
	}

	/* Preserve a boot snapshot even when a later watchdog loses DRAM. */
	log_store_to_emmc();
	if (register_reboot_notifier(&log_store_reboot_nb))
		pr_warn("log_store failed to register reboot notifier.\n");

	return 0;
}

#ifndef MODULE
/* need mapping virtual address to phy address */
static void store_printk_buff(void)
{
	phys_addr_t log_buf;
	char *buff;
	int size;

	if (!sram_dram_buff) {
		pr_notice("log_store: sram_dram_buff is null.\n");
		return;
	}
	buff = log_buf_addr_get();
	log_buf = virt_to_phys(buff);
	size = log_buf_len_get();
	/* support 32/64 bits */
#ifdef CONFIG_PHYS_ADDR_T_64BIT
	if ((log_buf >> 32) == 0)
		sram_dram_buff->klog_addr = (u32)(log_buf & 0xffffffff);
	else
		sram_dram_buff->klog_addr = 0;
#else
	sram_dram_buff->klog_addr = log_buf;
#endif
	sram_dram_buff->klog_size = size;
	if (!early_log_disable)
		sram_dram_buff->flag |= BUFF_EARLY_PRINTK;
	pr_notice("log_store printk_buff addr:0x%x,sz:0x%x,buff-flag:0x%x.\n",
		sram_dram_buff->klog_addr,
		sram_dram_buff->klog_size,
		sram_dram_buff->flag);
}
#endif

void disable_early_log(void)
{
	pr_notice("log_store: %s.\n", __func__);
	early_log_disable = true;
	if (!sram_dram_buff) {
		pr_notice("log_store: sram_dram_buff is null.\n");
		return;
	}

	sram_dram_buff->flag &= ~BUFF_EARLY_PRINTK;
}


struct mem_desc_ls {
	unsigned int addr;
	unsigned int size;
};
/* store log_store information to */
static int __init log_store_early_init(void)
{

#if IS_ENABLED(CONFIG_MTK_DRAM_LOG_STORE)
	struct mem_desc_ls sram_ls = {
		.addr = CONFIG_MTK_DRAM_LOG_STORE_ADDR,
		.size = CONFIG_MTK_DRAM_LOG_STORE_SIZE,
	};
	struct mem_desc_ls dt_sram_ls;
	struct device_node *np_chosen;
	const void *prop;
	int len;

	np_chosen = of_find_node_by_path("/chosen");
	if (!np_chosen)
		np_chosen = of_find_node_by_path("/chosen@0");

	if (np_chosen) {
		/*
		 * MediaTek LK injects this property as a native-endian
		 * mem_desc_ls, not as standard big-endian DT cells.  Keep the
		 * vendor ABI used by both the 4.19 kernel and Samsung's 6.6
		 * implementation.
		 */
		prop = of_get_property(np_chosen, "log_store", &len);
		if (prop && len >= sizeof(dt_sram_ls)) {
			memcpy(&dt_sram_ls, prop, sizeof(dt_sram_ls));
			if (dt_sram_ls.addr == CONFIG_MTK_DRAM_LOG_STORE_ADDR &&
			    dt_sram_ls.size == CONFIG_MTK_DRAM_LOG_STORE_SIZE)
				sram_ls = dt_sram_ls;
			else
				pr_warn("log_store: invalid DT descriptor 0x%x@0x%x, using configured 0x%x@0x%x\n",
					dt_sram_ls.size, dt_sram_ls.addr,
					sram_ls.size, sram_ls.addr);
		} else {
			pr_warn("log_store: invalid or missing DT property, using configured descriptor\n");
		}
		if (prop && len >= sizeof(dt_sram_ls) &&
		    dt_sram_ls.addr == sram_ls.addr &&
		    dt_sram_ls.size == sram_ls.size) {
			pr_info("log_store: get ok, sram addr:0x%x, size:0x%x\n",
					sram_ls.addr, sram_ls.size);
		}
		of_node_put(np_chosen);
	} else {
		pr_warn("log_store: no chosen node, using configured descriptor\n");
	}

	sram_header = ioremap_wc(sram_ls.addr,
		sram_ls.size);
	if (!sram_header) {
		pr_err("log_store: failed to map 0x%x@0x%x\n",
		       sram_ls.size, sram_ls.addr);
		return -ENOMEM;
	}
	dram_curlog_header = &(sram_header->dram_curlog_header);
#else
	pr_notice("log_store: not Found CONFIG_MTK_DRAM_LOG_STORE!\n");
	return -1;
#endif
	pr_notice("log_store: sram header address 0x%p.\n",
		sram_header);
	if (sram_header->sig != SRAM_HEADER_SIG) {
		pr_notice("log_store: sram header sig 0x%x.\n",
			sram_header->sig);
		sram_log_store_status = BUFF_ERROR;
		sram_header = NULL;
		return -1;
	}

	sram_dram_buff = &(sram_header->dram_buf);
	if (sram_dram_buff->sig != DRAM_HEADER_SIG) {
		pr_notice("log_store: sram header DRAM sig error");
		sram_log_store_status = BUFF_ERROR;
		sram_dram_buff = NULL;
		return -1;
	}

#ifndef MODULE
	/* store printk log buff information to DRAM */
	store_printk_buff();
#endif
	if (sram_header->reserve[1] == 0 ||
		sram_header->reserve[1] > EXPDB_LOG_SIZE)
		sram_header->reserve[1] = LOG_BLOCK_SIZE;

	pr_notice("sig 0x%x flag 0x%x add 0x%x size 0x%x offsize 0x%x point 0x%x\n",
		sram_dram_buff->sig, sram_dram_buff->flag,
		sram_dram_buff->buf_addr, sram_dram_buff->buf_size,
		sram_dram_buff->buf_offsize, sram_dram_buff->buf_point);

#ifdef MODULE
	log_store_late_init();
#endif

	return 0;
}

#ifdef MODULE
static void __exit log_store_exit(void)
{
	unregister_reboot_notifier(&log_store_reboot_nb);
	if (entry)
		proc_remove(entry);
}

module_init(log_store_early_init);
module_exit(log_store_exit);
#else
early_initcall(log_store_early_init);
late_initcall(log_store_late_init);
#endif

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
