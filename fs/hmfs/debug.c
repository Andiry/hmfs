#include <linux/fs.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/pagemap.h>
#include <linux/string.h>
#include "hmfs_fs.h"
#include "segment.h"

#define GET_BY_ADDR(sbi, type, addr) ( (type)ADDR((sbi), (addr)) )

#define MAX_CMD_LEN	((MAX_ARG_LEN + 2) * MAX_ARG_NUM)
#define MAX_ARG_LEN (12)
#define MAX_ARG_NUM (5)

#define USAGE		"============= GENERAL USAGE ============\n"\
			" type the these cmd to get detail usage.\n"\
			"    cp    --   show checkpoint info.\n" \
			"    ssa   --   show SSA info.\n" \
			"    sit   --   show SIT info.\n" \
			"    nat   --   show nat info.\n" \
			"    data  --   show nat info.\n" \
			"    help  --   show this usage.\n" \
			"=========================================\n"

#define USAGE_CP	"cp"

#define USAGE_SSA	"=============== SSA USAGE ==============\n"\
      			" `ssa <idx1> <idx2>`\n"\
			"   -- block summary in blk[idx1, idx2]\n"\
      			" `ssa <segno>`\n"\
			"   -- block summary in  segment[segno]\n"\
			"=========================================\n"

#define USAGE_SIT	"=============== SIT USAGE ==============\n" \
			" no parameters needed now. it should be\n"\
			" fase to scan all segments\n"\
			"=========================================\n"

#define USAGE_NAT "nat"
#define USAGE_DATA "data"

static LIST_HEAD(hmfs_stat_list);
static struct dentry *debugfs_root;
static DEFINE_MUTEX(hmfs_stat_mutex);

struct buffer {
	struct hmfs_sb_info *sbi;
	int size;
	int capacity;
	char *buf;
};
static struct buffer info_buffer;
static int hmfs_dispatch_cmd(const char *cmd, int len);

void update_nat_stat(struct hmfs_sb_info *sbi, int flush_count)
{
	struct hmfs_stat_info *stat_i = STAT_I(sbi);

	spin_lock(&stat_i->stat_lock);
	stat_i->flush_nat_sum += flush_count;
	stat_i->flush_nat_time++;
	stat_i->nr_flush_nat_per_block[flush_count / 50]++;
	spin_unlock(&stat_i->stat_lock);
}

static int stat_show(struct seq_file *s, void *v)
{
	struct hmfs_stat_info *si;
	struct hmfs_cm_info *cm_i = NULL;
	struct list_head *head, *this;
	struct orphan_inode_entry *orphan = NULL;
	unsigned long max_file_size = hmfs_max_file_size();
	int i;

	mutex_lock(&hmfs_stat_mutex);
	list_for_each_entry(si, &hmfs_stat_list, stat_list) {
		cm_i = CM_I(si->sbi);

		seq_printf(s, "=============General Infomation=============\n");
		seq_printf(s, "physical address:%lu\n",
			   (unsigned long)si->sbi->phys_addr);
		seq_printf(s, "virtual address:%p\n", si->sbi->virt_addr);
		seq_printf(s, "initial size:%lu\n",
			   (unsigned long)si->sbi->initsize);
		seq_printf(s, "page count:%lu\n",
			   (unsigned long)cm_i->user_block_count);
		seq_printf(s, "segment count:%lu\n",
			   (unsigned long)si->sbi->segment_count);
		seq_printf(s, "valid_block_count:%lu\n",
			   (unsigned long)cm_i->valid_block_count);
		seq_printf(s, "alloc_block_count:%lu\n",
			   (unsigned long)cm_i->alloc_block_count);
		seq_printf(s, "valid_node_count:%lu\n", cm_i->valid_node_count);
		seq_printf(s, "valid_inode_count:%lu\n",
			   cm_i->valid_inode_count);
		seq_printf(s, "SSA start address:%lu\n",
			   (unsigned long)((char *)si->sbi->ssa_entries -
					   (char *)si->sbi->virt_addr));
		seq_printf(s, "SIT start address:%lu\n",
			   (unsigned long)((char *)si->sbi->sit_entries -
					   (char *)si->sbi->virt_addr));
		seq_printf(s, "main area range:%lu - %lu\n",
			   (unsigned long)si->sbi->main_addr_start,
			   (unsigned long)si->sbi->main_addr_end);
		seq_printf(s, "max file size:%luk %luM %luG\n",
			   max_file_size / 1024, max_file_size / 1024 / 1024,
			   max_file_size / 1024 / 1024 / 1024);
		if (si->flush_nat_time)
			seq_printf(s, "flush_nat_per_block:%lu\n", 
					si->flush_nat_sum / si->flush_nat_time);
		for (i = 0; i < 10; i++)
			seq_printf(s, "nr_flush_nat_per_block[%d-%d):%d\n", i * 50,
					i * 50 + 50, si->nr_flush_nat_per_block[i]);

		head = &cm_i->orphan_inode_list;
		seq_printf(s, "orphan inode:\n");
		list_for_each(this, head) {
			orphan =
			    list_entry(this, struct orphan_inode_entry, list);
			seq_printf(s, "%lu ", (unsigned long)orphan->ino);
		}
		seq_printf(s, "\n");
	}
	mutex_unlock(&hmfs_stat_mutex);
	return 0;
}

static int stat_open(struct inode *inode, struct file *file)
{
	return single_open(file, stat_show, inode->i_private);
}

static const struct file_operations stat_fops = {
	.open = stat_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static int info_open(struct inode *inode, struct file *file)
{
	return 0;
}

static ssize_t info_read(struct file *file, char __user * buffer, size_t count,
			 loff_t * ppos)
{
	if (*ppos >= info_buffer.size)
		return 0;
	if (count + *ppos > info_buffer.size)
		count = info_buffer.size - *ppos;

	if (copy_to_user(buffer, info_buffer.buf, count)) {
		return -EFAULT;
	}

	*ppos += count;
	return count;
}

//'buffer' being added "\n" at the tail automatically.
static ssize_t info_write(struct file *file, const char __user * buffer,
			  size_t count, loff_t * ppos)
{
	char cmd[MAX_CMD_LEN + 1] = { 0 };

	if (*ppos >= MAX_CMD_LEN + 1) {
		return 0;
	}
	if (*ppos + count > MAX_CMD_LEN + 1)
		return -EFAULT;	//cmd buffer overflow

	if (copy_from_user(cmd, buffer, count))
		return -EFAULT;
	hmfs_dispatch_cmd(cmd, count);

	*ppos += count;
	return count;
}

struct file_operations info_fops = {
	.owner = THIS_MODULE,
	.open = info_open,
	.read = info_read,
	.write = info_write,
};

int hmfs_build_info(struct hmfs_sb_info *sbi, size_t c)
{
	info_buffer.sbi = sbi;
	info_buffer.size = 0;
	info_buffer.capacity = c;
	info_buffer.buf = kzalloc(sizeof(char) * c, GFP_KERNEL);
	if (!info_buffer.buf)
		return -ENOMEM;
	return 0;
}

void hmfs_destroy_info(void)
{
	info_buffer.sbi = NULL;
	info_buffer.size = 0;
	info_buffer.capacity = 0;
	kfree(info_buffer.buf);
	info_buffer.buf = NULL;
}

int hmfs_build_stats(struct hmfs_sb_info *sbi)
{
	struct hmfs_stat_info *si;

	sbi->stat_info = kzalloc(sizeof(struct hmfs_stat_info), GFP_KERNEL);
	if (!sbi->stat_info)
		return -ENOMEM;

	memset(sbi->stat_info, 0, sizeof(struct hmfs_stat_info));
	si = sbi->stat_info;
	si->sbi = sbi;
	spin_lock_init(&si->stat_lock);
	mutex_lock(&hmfs_stat_mutex);
	list_add_tail(&si->stat_list, &hmfs_stat_list);
	mutex_unlock(&hmfs_stat_mutex);

	return hmfs_build_info(sbi, (1 << 20));	//TODO
}

void hmfs_destroy_stats(struct hmfs_sb_info *sbi)
{
	struct hmfs_stat_info *si = sbi->stat_info;

	mutex_lock(&hmfs_stat_mutex);
	list_del(&si->stat_list);
	mutex_unlock(&hmfs_stat_mutex);

	kfree(si);
}

void hmfs_create_root_stat(void)
{
	debugfs_root = debugfs_create_dir("hmfs", NULL);
	if (debugfs_root) {
		debugfs_create_file("status", S_IRUGO, debugfs_root, NULL,
				    &stat_fops);
		debugfs_create_file("info", S_IRUGO, debugfs_root,
				    NULL, &info_fops);
	}
}

void hmfs_destroy_root_stat(void)
{
	debugfs_remove_recursive(debugfs_root);
	debugfs_root = NULL;
}

/*
 * vprint write to file buffer -- dump info to the file
 * 	@buffer : file buffer
 * 	@mode : 1 means appending, 0 will erase all data in the buffer.
 * 	@return : number of bytes written to file buffer
 */
int hmfs_print(int mode, const char *fmt, ...)
{
	size_t start, len;
	va_list args;

	if (0 == mode)
		info_buffer.size = 0;
	start = info_buffer.size;
	len = info_buffer.capacity - info_buffer.size;

	va_start(args, fmt);
	len = vsnprintf(info_buffer.buf + start, len, fmt, args);
	va_end(args);

	info_buffer.size += len;
	return len;
}

//return how many bytes written to file buffer
static int print_cp_one(struct hmfs_checkpoint *cp, int detail)
{
	size_t len = 0;
	int i;

	if (!cp)
		return 0;
	len += hmfs_print(1, "version: %u\n", le32_to_cpu(cp->checkpoint_ver));

	if (detail) {
		len += hmfs_print(1, "------detail info------\n");
		len += hmfs_print(1, "checkpoint_ver: %u\n",
				  le32_to_cpu(cp->checkpoint_ver));
		len += hmfs_print(1, "alloc_block_count: %u\n",
				  le64_to_cpu(cp->alloc_block_count));
		len += hmfs_print(1, "valid_block_count: %u\n",
				  le64_to_cpu(cp->valid_block_count));
		len += hmfs_print(1, "free_segment_count: %u\n",
				  le64_to_cpu(cp->free_segment_count));
		len += hmfs_print(1, "cur_node_segno: %u\n",
				  le32_to_cpu(cp->cur_node_segno));
		len += hmfs_print(1, "cur_node_blkoff: %u\n",
				  le16_to_cpu(cp->cur_node_blkoff));
		len += hmfs_print(1, "cur_data_segno: %u\n",
				  le32_to_cpu(cp->cur_data_segno));
		len += hmfs_print(1, "cur_data_blkoff: %u\n",
				  le16_to_cpu(cp->cur_data_blkoff));
		len += hmfs_print(1, "prev_cp_addr: %x\n",
				  le64_to_cpu(cp->prev_cp_addr));
		len += hmfs_print(1, "next_cp_addr: %x\n",
				  le64_to_cpu(cp->checkpoint_ver));
		len += hmfs_print(1, "valid_inode_count: %u\n",
				  le32_to_cpu(cp->valid_inode_count));
		len += hmfs_print(1, "valid_node_count: %u\n",
				  le32_to_cpu(cp->valid_node_count));
		len += hmfs_print(1, "nat_addr: %x\n", 
						le64_to_cpu(cp->nat_addr));

		for (i = 0; i < NUM_ORPHAN_BLOCKS; ++i) {
			if (cp->orphan_addrs[i]) {
				len += hmfs_print(1, "orphan_addr[%d]: %lu\n", i,
								le64_to_cpu(cp->orphan_addrs[i]));
			} else
				break;
		}
		len += hmfs_print(1, "next_scan_nid: %u\n",
			       le32_to_cpu(cp->next_scan_nid));
		len += hmfs_print(1, "elapsed_time: %u\n",
			       le32_to_cpu(cp->elapsed_time));
		len += hmfs_print(1, "\n\n");
	}
	return len;
}

static int print_cp_nth(struct hmfs_sb_info *sbi, int n, int detail)
{
	size_t i = 0;
	struct hmfs_cm_info *cmi = CM_I(sbi);
	struct checkpoint_info *cpi;
	struct hmfs_checkpoint *hmfs_cp = NULL;
	block_t next_addr;

	cpi = cmi->last_cp_i;
	hmfs_cp = cpi->cp;

	while (i++ < n) {
		next_addr = le64_to_cpu(hmfs_cp->next_cp_addr);
		hmfs_cp = ADDR(sbi, next_addr);
	}
	return print_cp_one(hmfs_cp, detail);
}

static int print_cp_all(struct hmfs_sb_info *sbi, int detail)
{
	size_t len = 0;
	struct hmfs_cm_info *cmi = CM_I(sbi);
	struct checkpoint_info *cpi;
	struct hmfs_checkpoint *hmfs_cp = NULL;
	block_t next_addr;

	cpi = cmi->last_cp_i;
	hmfs_cp = cpi->cp;

	do {
		next_addr = le64_to_cpu(hmfs_cp->next_cp_addr);
		hmfs_cp = ADDR(sbi, next_addr);
		len += print_cp_one(hmfs_cp, detail);	//member cp can't be used except for current checkpint
	} while (hmfs_cp != cpi->cp);
	return len;
}

/*
 Usage: 
     cp c    [<d>]  -- dump current checkpoint info.
     cp <n>  [<d>]  -- dump the n-th checkpoint info on NVM, 0 is the last one.
     cp a    [<d>]  -- dump whole checkpoint list on NVM.
     cp             -- print this usage.
     set option 'd' 0 will not give the detail info, default is 1
 */
static int hmfs_print_cp(int args, char argv[][MAX_ARG_LEN + 1])
{
	struct hmfs_sb_info *sbi = info_buffer.sbi;
	const char *opt = argv[1];
	size_t len = 0;
	int detail = 1;

	if (args >= 3 && '0' == argv[2][0])
		detail = 0;
	if ('c' == opt[0]) {
		hmfs_print(1, "======Current checkpoint info======\n");
		len = print_cp_one(NULL, detail);
	} else if ('a' == opt[0]) {
		hmfs_print(1, "======Total checkpoints info======\n");
		len = print_cp_all(sbi, detail);
	} else {
		unsigned long long n = simple_strtoull(opt, NULL, 0);
		hmfs_print(1, "======%luth checkpoint info======\n", n);
		len = print_cp_nth(sbi, n, detail);
	}
	return len;
}

/*
 * print_ssa_one -- dump a segment summary entry to file buffer.
 *	@blk_idx : the index of summary block.
 */
static size_t print_ssa_one(struct hmfs_sb_info *sbi, block_t blk_addr)
{
	size_t len = 0;
	struct hmfs_summary *sum_entry;

	if (blk_addr < sbi->main_addr_start || blk_addr >= sbi->main_addr_end) {
		//invalid block addr
		return -1;
	}

	sum_entry = get_summary_by_addr(sbi, blk_addr);

	len +=
	    hmfs_print(1, "-- [%016x] --\n", blk_addr >> HMFS_PAGE_SIZE_BITS);
	len += hmfs_print(1, "  nid: %u\n", le32_to_cpu(sum_entry->nid));
	len += hmfs_print(1, "  start_version: %u\n",
			   le32_to_cpu(sum_entry->start_version));
	len += hmfs_print(1, "  ofs_in_node: %u\n", get_summary_offset(sum_entry));
	len += hmfs_print(1, "  type: %u\n", get_summary_type(sum_entry));
	len += hmfs_print(1, "  v bit: %u\n", !!get_summary_valid_bit(sum_entry));
	len += hmfs_print(1, "\n");

	return len;
}

static int print_ssa_range(struct hmfs_sb_info *sbi, block_t idx_from,
			   block_t idx_to)
{
	int len = 0, i = 0, res = -1;

	//struct hmfs_summary_block* sum_blk = get_summary_block(sbi, blkidx);
	for (i = idx_from; i <= idx_to; i++) {
		res = print_ssa_one(sbi, i << HMFS_PAGE_SIZE_BITS);
		if (res == -1) {
			return -1;
		}
		len += res;
	}
	return len;
}

static size_t print_ssa_per_seg(struct hmfs_sb_info *sbi, block_t segno)
{
	block_t idx_from = segno << HMFS_PAGE_PER_SEG_BITS;
	return print_ssa_range(sbi, idx_from, idx_from + HMFS_PAGE_PER_SEG - 1);
}

/*
  Usage:
      ssa <idx1> <idx2>	-- dump summary of [idx1, idx2]th block 
      ssa <segno>	-- dump summary of all blocks in [segno]th segment
 */
static int hmfs_print_ssa(int args, char argv[][MAX_ARG_LEN + 1])
{
	int len = 0, cnt = -1;
	block_t idx_from = 0, idx_to = 0;
	struct hmfs_sb_info *sbi = info_buffer.sbi;

	hmfs_print(0, "======= SSA INFO =======\n");
	if (2 == args) {
		idx_from = (block_t) simple_strtoull(argv[1], NULL, 0);
		cnt = print_ssa_per_seg(sbi, idx_from);
	} else if (3 == args) {
		idx_from = (block_t) simple_strtoull(argv[1], NULL, 0);
		idx_to = (block_t) simple_strtoull(argv[2], NULL, 0);
		cnt = print_ssa_range(sbi, idx_from, idx_to);
	}
	if (cnt < 0) {
		hmfs_print(0, " **error** invalid index: %llu\n", idx_from);
		return 0;
	}
	len += cnt;
	return len;
}

static inline int get_vblocks_from_sit(struct hmfs_sb_info *sbi, seg_t segno)
{
	return __le16_to_cpu(get_sit_entry(sbi, segno)->vblocks);
}

static inline int print_error_segment(seg_t segno, int sit_blk_cnt,
				       int ssa_blk_cnt)
{
	return hmfs_print(1, "segment #%d *ERROR*, cnt in SIT: %d\
			  cnt in SSA: %d\n", segno, sit_blk_cnt, ssa_blk_cnt);
}

static int hmfs_print_sit(int args, char argv[][MAX_ARG_LEN + 1])
{
	struct hmfs_sb_info *sbi = info_buffer.sbi;
	int sit_blk_cnt, len=0;
	int ssa_blk_cnt;
	int blk_id = 0;
	seg_t segno = 0;

	struct hmfs_summary_block *ssa_blk;
	struct hmfs_summary *ssa_entry;


	for (segno = 0; segno < TOTAL_SEGS(sbi); ++segno) {
		ssa_blk = get_summary_block(sbi, segno);
		ssa_entry = ssa_blk->entries;
		ssa_blk_cnt = 0;
		for (blk_id = 0; blk_id < HMFS_PAGE_PER_SEG; ++blk_id) {
			if (get_summary_valid_bit(ssa_entry))//seems that le16 is ok
				++ssa_blk_cnt;
			ssa_entry++;
		}

		sit_blk_cnt = get_vblocks_from_sit(sbi, segno);
		if (ssa_blk_cnt != sit_blk_cnt){
			len = print_error_segment(segno, sit_blk_cnt, ssa_blk_cnt);
			break;
		}
	}
	if (segno == TOTAL_SEGS(sbi)){
		len = hmfs_print(1, "no error found in SIT check!\n");
	}

	return len;
}

static int hmfs_print_nat(int args, char argv[][MAX_ARG_LEN + 1])
{
	return 0;
}

static int hmfs_print_data(int args, char argv[][MAX_ARG_LEN + 1])
{
	return 0;
}

static int hmfs_check_ssa(struct hmfs_sb_info *sbi, block_t cp_addr,
			  block_t blk_addr, size_t h, size_t offset,
			  block_t nid)
{
	struct hmfs_checkpoint* cp;
	struct hmfs_summary* summary;
	block_t raw_nid, raw_height;
	int ret_val = 0;

	cp = (struct hmfs_checkpoint *)ADDR(sbi, cp_addr);
	summary = get_summary_by_addr(sbi, blk_addr);

	//check summary type
	if ((0 != h && SUM_TYPE_NATN != get_summary_type(summary))
	    || (0 == h && SUM_TYPE_NATD != get_summary_type(summary))) {
		hmfs_print(1, "**error** summary type error: ");
		hmfs_print(1,
			   "type of nat node at %#x should be %d, but get %d \n",
			   blk_addr, h ? SUM_TYPE_NATN : SUM_TYPE_NATD,
			   get_summary_type(summary));
		ret_val = -1;
	}
	//check offset && nid
	if (h != sbi->nat_height) {
		raw_height = get_summary_nid(summary) >> 27;
		raw_nid = (get_summary_nid(summary) & 0x7ffffff);
		if (offset != get_summary_offset(summary)) {
			hmfs_print(1, "**error** summary offset error: ");
			hmfs_print(1,
				   "offset nat node at %#x should be %d, but get %d \n",
				   blk_addr, offset,
				   get_summary_offset(summary));
			ret_val = -1;
		}
		if (h + 1 != raw_height) {
			hmfs_print(1, "**error** summary height error: ");
			hmfs_print(1,
				   "offset nat node at %#x should be %d, but get %llu \n",
				   blk_addr, h + 1, raw_height);
			ret_val = -1;
		}
		if (nid != raw_nid) {
			hmfs_print(1, "**error** summary block order error: ");
			hmfs_print(1,
				   "offset nat node at %#x should be %d, but get %llu \n",
				   blk_addr, h, raw_nid);
			ret_val = -1;
		}
	}

	return ret_val;
}

static int traverse_nat(struct hmfs_sb_info *sbi, block_t cp_addr,
			block_t root_addr, size_t h, block_t nid)
{
	int err = 0;
	size_t i;
	struct hmfs_nat_node *root;
	size_t offset = nid >> (h * LOG2_NAT_ADDRS_PER_NODE);

	if (!root_addr)
		return 0;

	err = hmfs_check_ssa(sbi, cp_addr, root_addr, h, offset, nid);
	if (0 != err) {
		hmfs_print(1, "\n----- ERROR BLK INFO -----\n");
		print_ssa_one(sbi, root_addr);
		hmfs_print(1, "--------------------------\n");
		return err;
	}

	if (0 == h) {		//get the nat entry
		//TODO: make node summary check
		return err;
	}

	root = (struct hmfs_nat_node *)ADDR(sbi, root_addr);
	for (i = 0; i < NAT_ADDR_PER_NODE; i++) {
		block_t child_addr = le64_to_cpu(root->addr[i]);
		if (child_addr == NULL_ADDR) {
			continue;
		}
		hmfs_print(1, ">>>>>>>>>>> %p -> %p, height is %d\n", root_addr,
			   child_addr, h);
		err =
		    traverse_nat(sbi, cp_addr, child_addr, h - 1,
				 nid +
				 (i << ((h - 1) * LOG2_NAT_ADDRS_PER_NODE)));
		if (0 != err)	//stop if found error
			break;
	}
	return err;
}

/*
 *description: 
 *	check consistency of meta info on NVM.
 * @return: return the error code; 0, no error.
 */
static int hmfs_consis(void)
{
	int err = 0;
	struct hmfs_sb_info *sbi = info_buffer.sbi;
	block_t cp_head_addr, cp_addr;
	struct hmfs_super_block *sb = HMFS_RAW_SUPER(sbi);
	struct hmfs_cm_info *cmi = sbi->cm_info;
	hmfs_print(1, "cmi->valid_inode: %d\n", cmi->valid_inode_count);

	//check summary
	hmfs_print(1, "======= check summary ======\n");
	cp_head_addr = le64_to_cpu(sb->cp_page_addr);
	cp_head_addr =
	    le64_to_cpu(((struct hmfs_checkpoint *)ADDR(sbi, cp_head_addr))->
			prev_cp_addr);
	for (cp_addr = cp_head_addr;;) {
		struct hmfs_checkpoint *cp;
		cp = (struct hmfs_checkpoint *)ADDR(sbi, cp_addr);
		hmfs_print(1, "checkpoint address: %#x\n", cp_addr);
		hmfs_print(1, "valid inode count: %d\n",
			   le32_to_cpu(cp->valid_inode_count));
		hmfs_print(1, "valid node count: %d\n",
			   le32_to_cpu(cp->valid_node_count));
		err =
		    traverse_nat(sbi, cp_addr, le64_to_cpu(cp->nat_addr),
				 sbi->nat_height, 0);
		//if (0 != err)
		//      return err;
		cp_addr =
		    le64_to_cpu(((struct hmfs_checkpoint *)ADDR(sbi, cp_addr))->
				prev_cp_addr);
		if (cp_addr == cp_head_addr)
			break;
	}
	hmfs_print(1, "=== check summary done ===\n");

	//TODO: other consistency checking

	return err;
}

#define IS_BLANK(ch) (' ' == (ch) || '\t' == (ch) || '\n' == (ch))

//return: < 0, error; else, args;
static int hmfs_parse_cmd(const char *cmd, size_t len,
			  char argv[][MAX_ARG_LEN + 1])
{
	int args;
	size_t i, j, tokenl;
	for (i = 0, j = 0, args = 0; i < len;) {
		if (args >= MAX_ARG_NUM)
			return args;
		while (i < len && IS_BLANK(cmd[i])) {
			++i;
		}
		j = i;
		while (i < len && !IS_BLANK(cmd[i]))
			++i;
		if (i - j > MAX_ARG_LEN)
			tokenl = MAX_ARG_LEN;
		else
			tokenl = i - j;
		if (0 == tokenl)
			break;

		strncpy(argv[args], cmd + j, tokenl);
		argv[args][tokenl] = 0;
		++args;
	}

	return args;
}

/*
 * DESCRIPTION:
 * 	When we trying to write a debugfs file, it is trated command.
 * 	We parse command and exec some functions to set the output buffer.
 * 	Then we can get the infomation we want.
 * 	P.S. lseek() doesn't work.
 *
 * BASH EXAMPLE:
 * 	`$ echo <cmd> > <file> && cat <file>`
 *
 * RETURN VALUE:
 * 	success with the length of written file buffer, else -EFAULT;
 */
static int hmfs_dispatch_cmd(const char *cmd, int len)
{

	int args, res = 0;
	char argv[MAX_ARG_NUM][MAX_ARG_LEN + 1];
	args = hmfs_parse_cmd(cmd, len, argv);
	if (args <= 0) {
		//print usage guide
		hmfs_print(0, USAGE);
		return -EFAULT;
	}

	hmfs_print(0, "");	//clear the buffer
	if (0 == strncasecmp(argv[0], "cp", 2)) {
		if (args == 1) {
			hmfs_print(0, USAGE_CP);
			return 0;
		}
		res = hmfs_print_cp(args, argv);
	} else if (0 == strncasecmp(argv[0], "ssa", 3)) {
		if (args == 1) {
			hmfs_print(0, USAGE_SSA);
			return 0;
		}
		res = hmfs_print_ssa(args, argv);
	} else if (0 == strncasecmp(argv[0], "sit", 3)) {
		if (args == 1) {
			hmfs_print(0, USAGE_SIT);
			res = hmfs_print_sit(args, argv);
		}
		//res = hmfs_print_sit(args, argv);
	} else if (0 == strncasecmp(argv[0], "nat", 3)) {
		if (args == 1) {
			hmfs_print(0, USAGE_NAT);
			return 0;
		}
		res = hmfs_print_nat(args, argv);
	} else if (0 == strncasecmp(argv[0], "data", 4)) {
		if (args <= 1) {
			hmfs_print(0, USAGE_DATA);
			return 0;
		}
		res = hmfs_print_data(args, argv);
	} else if (0 == strncasecmp(argv[0], "consis", 6)) {
		res = hmfs_consis();
	} else {
		hmfs_print(0, USAGE);
		return -EFAULT;
	}

	return res;

}

inline void hmfs_call_trace(void)
{
	tprint("<%s> Caller0 is %pS\n", __FUNCTION__,
	       __builtin_return_address(0));
	tprint("<%s> Caller1 is %pS\n", __FUNCTION__,
	       __builtin_return_address(1));
	tprint("<%s> Caller2 is %pS\n", __FUNCTION__,
	       __builtin_return_address(2));
}
