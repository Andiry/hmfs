#include "hmfs.h"
#include "hmfs_fs.h"
#include "segment.h"
#include <linux/crc16.h>
#include <linux/pagevec.h>

static struct kmem_cache *orphan_entry_slab;

static struct kmem_cache *cp_info_entry_slab;

static struct kmem_cache *map_inode_entry_slab;

static ver_t next_checkpoint_ver(ver_t version)
{
	//TODO
	return version + 1;
}

static void init_orphan_manager(struct hmfs_cm_info *cm_i)
{
	/* init orphan manager */
	mutex_init(&cm_i->orphan_inode_mutex);
	INIT_LIST_HEAD(&cm_i->orphan_inode_list);
	cm_i->n_orphans = 0;
}

void add_orphan_inode(struct hmfs_sb_info *sbi, nid_t ino)
{
	struct list_head *head, *this;
	struct orphan_inode_entry *new = NULL, *orphan = NULL;
	struct hmfs_cm_info *cm_i = CM_I(sbi);

	mutex_lock(&cm_i->orphan_inode_mutex);
	head = &cm_i->orphan_inode_list;
	list_for_each(this, head) {
		orphan = list_entry(this, struct orphan_inode_entry, list);
		if (orphan->ino == ino)
			goto out;
		if (orphan->ino > ino)
			break;
		orphan = NULL;
	}
retry:
	new = kmem_cache_alloc(orphan_entry_slab, GFP_ATOMIC);
	if (!new) {
		cond_resched();
		goto retry;
	}
	new->ino = ino;

	if (orphan)
		list_add(&new->list, this->prev);
	else
		list_add_tail(&new->list, head);
	cm_i->n_orphans++;
out:	
	mutex_unlock(&cm_i->orphan_inode_mutex);
}

void remove_orphan_inode(struct hmfs_sb_info *sbi, nid_t ino)
{
	struct list_head *this, *next, *head;
	struct orphan_inode_entry *orphan;
	struct hmfs_cm_info *cm_i = CM_I(sbi);

	mutex_lock(&cm_i->orphan_inode_mutex);
	head = &cm_i->orphan_inode_list;
	list_for_each_safe(this, next, head) {
		orphan = list_entry(this, struct orphan_inode_entry, list);
		if (orphan->ino == ino) {
			list_del(&orphan->list);
			INIT_LIST_HEAD(&orphan->list);
			kmem_cache_free(orphan_entry_slab, orphan);
			cm_i->n_orphans--;
			break;
		}
	}
	mutex_unlock(&cm_i->orphan_inode_mutex);
}

int check_orphan_space(struct hmfs_sb_info *sbi)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	int err = 0;

	mutex_lock(&cm_i->orphan_inode_mutex);
	if (cm_i->n_orphans >= HMFS_MAX_ORPHAN_NUM)
		err = -ENOSPC;
	BUG_ON(cm_i->n_orphans > HMFS_MAX_ORPHAN_NUM);
	mutex_unlock(&cm_i->orphan_inode_mutex);
	return err;
}

static int __add_dirty_map_inode(struct inode *inode, struct map_inode_entry *new)
{
	struct hmfs_sb_info *sbi = HMFS_I_SB(inode);
	struct list_head *head = &sbi->dirty_map_inodes;
	struct list_head *this;
	struct map_inode_entry *entry;

	list_for_each(this, head) {
		entry = list_entry(this, struct map_inode_entry, list);
		if (entry->inode == inode)
			return -EEXIST;
	}
	list_add_tail(&new->list, head);
	return 0;
}

void add_dirty_map_inode(struct inode *inode)
{
	struct hmfs_sb_info *sbi = HMFS_I_SB(inode);
	struct map_inode_entry *new;
retry:
	new = kmem_cache_alloc(map_inode_entry_slab, GFP_NOFS);
	if (!new) {
		cond_resched();
		goto retry;
	}
	new->inode = inode;
	INIT_LIST_HEAD(&new->list);

	spin_lock(&sbi->dirty_map_inodes_lock);
	if (__add_dirty_map_inode(inode, new))
		kmem_cache_free(map_inode_entry_slab, new);
	spin_unlock(&sbi->dirty_map_inodes_lock);
}

void remove_dirty_map_inode(struct inode *inode)
{
	struct hmfs_sb_info *sbi = HMFS_I_SB(inode);
	struct list_head *head = &sbi->dirty_map_inodes;
	struct list_head *this;
	struct map_inode_entry *entry;

	spin_lock(&sbi->dirty_map_inodes_lock);

	list_for_each(this, head) {
		entry = list_entry(this, struct map_inode_entry, list);
		if (entry->inode == inode) {
			list_del(&entry->list);
			INIT_LIST_HEAD(&entry->list);
			kmem_cache_free(map_inode_entry_slab, entry);
			break;
		}
	}

	spin_unlock(&sbi->dirty_map_inodes_lock);
}

static void sync_checkpoint_info(struct hmfs_sb_info *sbi,
				struct hmfs_checkpoint *hmfs_cp,
				struct checkpoint_info *cp)
{
	cp->version = le32_to_cpu(hmfs_cp->checkpoint_ver);
	cp->nat_root = ADDR(sbi, le64_to_cpu(hmfs_cp->nat_addr));
	cp->cp = hmfs_cp;
}

static void move_to_next_checkpoint(struct hmfs_sb_info *sbi,
				struct hmfs_checkpoint *prev_checkpoint)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);

	mutex_lock(&cm_i->cp_tree_lock);

	sync_checkpoint_info(sbi, prev_checkpoint, cm_i->cur_cp_i);
	radix_tree_insert(&cm_i->cp_tree_root, cm_i->new_version,
			cm_i->cur_cp_i);
	list_add(&cm_i->cur_cp_i->list, &cm_i->last_cp_i->list);
	cm_i->new_version = next_checkpoint_ver(cm_i->new_version);
	cm_i->last_cp_i = cm_i->cur_cp_i;
retry:
	cm_i->cur_cp_i = kmem_cache_alloc(cp_info_entry_slab, GFP_KERNEL);

	if (!cm_i->cur_cp_i) {
		cond_resched();
		goto retry;
	}

	cm_i->cur_cp_i->version = cm_i->new_version;
	cm_i->cur_cp_i->nat_root = NULL;
	cm_i->cur_cp_i->cp = NULL;

	mutex_unlock(&cm_i->cp_tree_lock);
}

struct checkpoint_info *get_next_checkpoint_info(struct hmfs_sb_info *sbi,
				struct checkpoint_info *cp_i)
{
	ver_t next_version;
	struct hmfs_checkpoint *this_cp, *next_cp;
	block_t next_addr;
	struct checkpoint_info *next_cp_i;
	struct hmfs_cm_info *cm_i = CM_I(sbi);

	this_cp = cp_i->cp;
	next_addr = le64_to_cpu(this_cp->next_cp_addr);
	next_cp = ADDR(sbi, next_addr);
	next_version = le32_to_cpu(next_cp->checkpoint_ver);
	next_cp_i = radix_tree_lookup(&cm_i->cp_tree_root, next_version);

	if (!next_cp_i) {
retry:
		next_cp_i = kmem_cache_alloc(cp_info_entry_slab, GFP_KERNEL);
		if (!next_cp_i) {
			cond_resched();
			goto retry;
		}

		sync_checkpoint_info(sbi, next_cp, next_cp_i);
		//TODO: sort cp_i according to version
		list_add(&next_cp_i->list, &cm_i->last_cp_i->list);
		radix_tree_insert(&cm_i->cp_tree_root, next_cp_i->version,
				next_cp_i);
	}
	return next_cp_i;
}

/*
 * no_fail: If checkpoint with version is miss, return the checkpoint
 * whose version is slightly greater than version
 */
struct checkpoint_info *get_checkpoint_info(struct hmfs_sb_info *sbi,
				ver_t version, bool no_fail)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	struct checkpoint_info *cp_i, *entry;
	struct list_head *this, *head;
	struct hmfs_checkpoint *hmfs_cp;
	block_t next_addr;

	if (version == cm_i->new_version)
		return cm_i->cur_cp_i;

	mutex_lock(&cm_i->cp_tree_lock);
	cp_i = radix_tree_lookup(&cm_i->cp_tree_root, version);
	if (!cp_i) {
		cp_i = cm_i->last_cp_i;
		hmfs_bug_on(sbi, version > cp_i->version);

		head = &cp_i->list;
		/* Search a checkpoint_info whose version is closest to given version */
		cp_i = NULL;
		list_for_each(this, head) {
			entry = list_entry(this, struct checkpoint_info, list);
			if (entry->version < version) {
				if (cp_i == NULL || entry->version > cp_i->version)
					cp_i = entry;
			}
		}

		if (cp_i == NULL)
			cp_i = cm_i->last_cp_i;

		do {
			next_addr = le64_to_cpu(cp_i->cp->next_cp_addr);

			hmfs_cp = ADDR(sbi, next_addr);
retry:
			entry = kmem_cache_alloc(cp_info_entry_slab, GFP_KERNEL);

			if (!entry) {
				cond_resched();
				goto retry;
			}

			sync_checkpoint_info(sbi, hmfs_cp, entry);

			list_add(&entry->list, &cm_i->last_cp_i->list);
			radix_tree_insert(&cm_i->cp_tree_root, entry->version,
					entry);
			cp_i = entry;
			if (cp_i->version == version || (no_fail && cp_i->version > version))
				break;
			if (cp_i->version > version) {
				cp_i = NULL;
				break;
			}
		} while (1);

	}
	mutex_unlock(&cm_i->cp_tree_lock);
	return cp_i;
}

static struct hmfs_checkpoint *get_mnt_checkpoint(struct hmfs_sb_info *sbi,
				struct hmfs_checkpoint *cp,	ver_t version)
{
	struct hmfs_checkpoint *entry = cp;
	ver_t current_version;
	block_t addr;

	do {
		addr = le64_to_cpu(entry->next_cp_addr);
		entry = ADDR(sbi, addr);
		current_version = le32_to_cpu(entry->checkpoint_ver);
	} while(current_version != version && entry != cp);

	if (current_version == version)
		return entry;
	return NULL;
}

static void recovery_cp_gc(struct hmfs_sb_info *sbi, 
				struct hmfs_checkpoint *hmfs_cp)
{
	block_t rs_cp_addr, nx_cp_addr;	
	struct hmfs_checkpoint *rs_cp, *nx_cp;
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	int checksum;

	/* If HMFS_CP_GC set, we have taken store_checkpoint_addr */
	rs_cp_addr = le64_to_cpu(hmfs_cp->state_arg_2);
	rs_cp = ADDR(sbi, rs_cp_addr);
	nx_cp_addr = le64_to_cpu(rs_cp->next_cp_addr);
	nx_cp = ADDR(sbi, nx_cp_addr);

	hmfs_bug_on(sbi, le64_to_cpu(rs_cp->prev_cp_addr) != L_ADDR(sbi, hmfs_cp));
	hmfs_bug_on(sbi, le32_to_cpu(hmfs_cp->checkpoint_ver) < 
					le32_to_cpu(rs_cp->checkpoint_ver));
	hmfs_bug_on(sbi, le32_to_cpu(nx_cp->checkpoint_ver) < 
					le32_to_cpu(hmfs_cp->checkpoint_ver));

	/* Flush SIT and SSA in checkpoint log area */
	recovery_sit_entries(sbi, hmfs_cp, true);

	hmfs_cp->next_cp_addr = cpu_to_le64(rs_cp_addr);
	nx_cp->prev_cp_addr = cpu_to_le64(rs_cp_addr);
	raw_super->cp_page_addr = cpu_to_le64(rs_cp_addr);
	
	checksum = hmfs_make_checksum(rs_cp);
	set_struct(rs_cp, checksum, checksum);

	checksum = hmfs_make_checksum(raw_super);
	set_struct(raw_super, checksum, checksum);

	raw_super = next_super_block(raw_super);
	hmfs_memcpy(raw_super, HMFS_RAW_SUPER(sbi), sizeof(struct hmfs_super_block));

	set_fs_state(hmfs_cp, HMFS_NONE);

	move_to_next_checkpoint(sbi, rs_cp);
}

void check_checkpoint_state(struct hmfs_sb_info *sbi)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	struct hmfs_checkpoint *hmfs_cp = cm_i->last_cp_i->cp;
	u8 state;

	sbi->recovery_doing = 1;
	state = hmfs_cp->state;
	switch(state) {
	case HMFS_NONE:
		break;
	case HMFS_GC_DATA:
	case HMFS_GC_NODE:
		recovery_gc_crash(sbi, hmfs_cp);
		break;
	case HMFS_CP_GC:
		recovery_cp_gc(sbi, hmfs_cp);
		break;
	case HMFS_ADD_CP:
		redo_checkpoint(sbi, hmfs_cp);
		break;
	}
	sbi->recovery_doing = 0;
}

int init_checkpoint_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_cm_info *cm_i;
	struct checkpoint_info *cp_i;
	struct hmfs_super_block *super = ADDR(sbi, 0);
	struct hmfs_checkpoint *hmfs_cp;
	block_t cp_addr;

	/* Init checkpoint_info list */
	cp_addr = le64_to_cpu(super->cp_page_addr);
	hmfs_cp = ADDR(sbi, cp_addr);

	if (sbi->mnt_cp_version && sbi->mnt_cp_version != 
			le32_to_cpu(hmfs_cp->checkpoint_ver)) {
		hmfs_cp = get_mnt_checkpoint(sbi, hmfs_cp, sbi->mnt_cp_version);
		if (!hmfs_cp)
			return -EINVAL;
	}

	cm_i = kzalloc(sizeof(struct hmfs_cm_info), GFP_KERNEL);

	if (!cm_i) {
		goto out_cm_i;
	}

	/* allocate and init last checkpoint_info */
	cp_i = kmem_cache_alloc(cp_info_entry_slab, GFP_ATOMIC);
	if (!cp_i) {
		goto out_cp_i;
	}

	
	cm_i->valid_inode_count = le32_to_cpu(hmfs_cp->valid_inode_count);
	cm_i->valid_node_count = le32_to_cpu(hmfs_cp->valid_node_count);
	cm_i->valid_block_count = le32_to_cpu(hmfs_cp->valid_block_count);
	cm_i->user_block_count = le32_to_cpu(HMFS_RAW_SUPER(sbi)->user_block_count);
	cm_i->alloc_block_count = le32_to_cpu(hmfs_cp->alloc_block_count);
	sync_checkpoint_info(sbi, hmfs_cp, cp_i);
	cm_i->last_cp_i = cp_i;

	rwlock_init(&cm_i->journal_lock);
	spin_lock_init(&cm_i->stat_lock);
	INIT_LIST_HEAD(&cp_i->list);
	INIT_RADIX_TREE(&cm_i->cp_tree_root, GFP_ATOMIC);
	mutex_init(&cm_i->cp_tree_lock);
	mutex_init(&cm_i->cp_mutex);

	mutex_lock(&cm_i->cp_tree_lock);
	radix_tree_insert(&cm_i->cp_tree_root, cp_i->version, cp_i);
	mutex_unlock(&cm_i->cp_tree_lock);

	/* Allocate and Init current checkpoint_info */
	cp_i = kmem_cache_alloc(cp_info_entry_slab, GFP_KERNEL);
	INIT_LIST_HEAD(&cp_i->list);
	cm_i->new_version = next_checkpoint_ver(le32_to_cpu(hmfs_cp->checkpoint_ver));
	cp_i->version = cm_i->new_version;
	cp_i->nat_root = NULL;
	cp_i->cp = NULL;

	init_orphan_manager(cm_i);

	cm_i->cur_cp_i = cp_i;

	sbi->cm_info = cm_i;
	return 0;

out_cp_i:
	kfree(cm_i);
out_cm_i:
	return -ENOMEM;
}

static void destroy_checkpoint_info(struct hmfs_cm_info *cm_i)
{
	struct checkpoint_info *cp_i = cm_i->last_cp_i, *entry;
	struct list_head *head, *this, *tmp;

	head = &cp_i->list;
	list_for_each_safe(this, tmp, head) {
		entry = list_entry(this, struct checkpoint_info, list);
		list_del(this);
		INIT_LIST_HEAD(&entry->list);
		radix_tree_delete(&cm_i->cp_tree_root, entry->version);
		kmem_cache_free(cp_info_entry_slab, entry);
	}
	kmem_cache_free(cp_info_entry_slab, cp_i);
	radix_tree_delete(&cm_i->cp_tree_root, cp_i->version);
	kmem_cache_free(cp_info_entry_slab, cm_i->cur_cp_i);
}

int destroy_checkpoint_manager(struct hmfs_sb_info *sbi)
{
	struct hmfs_cm_info *cm_i = sbi->cm_info;

	mutex_lock(&cm_i->cp_tree_lock);
	destroy_checkpoint_info(cm_i);
	mutex_unlock(&cm_i->cp_tree_lock);

	kfree(cm_i);
	return 0;
}

int create_checkpoint_caches(void)
{
	orphan_entry_slab = hmfs_kmem_cache_create("hmfs_orphan_entry",
								sizeof(struct orphan_inode_entry), NULL);
	if (unlikely(!orphan_entry_slab))
		return -ENOMEM;

	cp_info_entry_slab = hmfs_kmem_cache_create("hmfs_checkpoint_info_entry",
								sizeof(struct checkpoint_info), NULL);
	if (cp_info_entry_slab == NULL) {
		goto free_orphan;
	}
	
	map_inode_entry_slab = hmfs_kmem_cache_create("hmfs_map_inode_entry",
								sizeof(struct map_inode_entry), NULL);
	if(map_inode_entry_slab == NULL)
		goto free_cp_info;
	return 0;

free_cp_info:
	kmem_cache_destroy(cp_info_entry_slab);
free_orphan:
	kmem_cache_destroy(orphan_entry_slab);
	
	return -ENOMEM;
}

void destroy_checkpoint_caches(void)
{
	kmem_cache_destroy(orphan_entry_slab);
	kmem_cache_destroy(cp_info_entry_slab);
	kmem_cache_destroy(map_inode_entry_slab);
}

static void sync_map_data_pages(struct hmfs_sb_info *sbi)
{
	struct list_head *head = &sbi->dirty_map_inodes, *this;
	struct map_inode_entry *entry;
	struct inode *inode;
	struct pagevec pvec;
	pgoff_t index = 0, end = LONG_MAX;
	int i, nr_pages;
	struct page *page;
	struct writeback_control wbc = {
		.for_reclaim = 0,
	};
	struct address_space *mapping = NULL;

	pagevec_init(&pvec, 0);

	list_for_each(this, head) {
		entry = list_entry(this, struct map_inode_entry, list);
		inode = igrab(entry->inode);
		if (!inode)
			continue;
		hmfs_bug_on(sbi, atomic_read(&HMFS_I(inode)->nr_dirty_map_pages));
		index = 0;
		
		mapping = inode->i_mapping;
		while (index <= end) {
			nr_pages = pagevec_lookup_tag(&pvec, mapping, &index,
							PAGECACHE_TAG_DIRTY, 
							min(end - index, (pgoff_t)PAGEVEC_SIZE - 1) + 1);
			if(nr_pages == 0)
				break;

			for (i = 0; i < nr_pages; i++) {
				page = pvec.pages[i];
				lock_page(page);
				hmfs_bug_on(sbi, page->mapping != mapping);
				hmfs_bug_on(sbi, !PageDirty(page));
				clear_page_dirty_for_io(page);
				
				if (hmfs_write_data_page(page, &wbc)) {	
					unlock_page(page);
					break;
				}
			}
			pagevec_release(&pvec);
		}
		hmfs_bug_on(sbi, !atomic_read(&HMFS_I(inode)->nr_dirty_map_pages));
	}
}

static void sync_dirty_inodes(struct hmfs_sb_info *sbi)
{
	struct list_head *head, *this, *next;
	struct hmfs_inode_info *inode_i;
	int ret;

	head = &sbi->dirty_inodes_list;
	list_for_each_safe(this, next, head) {
		inode_i = list_entry(this, struct hmfs_inode_info, list);
		ret = __hmfs_write_inode(&inode_i->vfs_inode);
	}
}

static void block_operations(struct hmfs_sb_info *sbi)
{
retry:
	mutex_lock_all(sbi);
	
	if (atomic_read(&sbi->nr_dirty_map_pages)) {
		mutex_unlock_all(sbi);
		sync_map_data_pages(sbi);
		goto retry;
	}
	
	if (!list_empty(&sbi->dirty_inodes_list)) {
		mutex_unlock_all(sbi);
		sync_dirty_inodes(sbi);
		goto retry;
	}
}

static void unblock_operations(struct hmfs_sb_info *sbi)
{
	mutex_unlock_all(sbi);
}

/*
 * We need to flush orphan inodes before allocating the checkpoint block of
 * this orphan inodes. In GC, we would collect block by order. If we allocate
 * checkpoint block before orphan blocks adn they are in the same segment,
 * we would move the checkpoint to the new segment first, and then if we move the
 * orphan blocks, we would write address to older checkpoint instead 
 * of new checkpoint
 */
static int flush_orphan_inodes(struct hmfs_sb_info *sbi, block_t *orphan_addrs)
{
	struct list_head *head, *this;
	struct orphan_inode_entry *entry;
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	struct hmfs_summary *summary;
	block_t orphan_addr = 0;
	__le32 *orphan_block = NULL;
	__le32 *end = NULL;
	int i = 0;
	int ret = 0;

	mutex_lock(&cm_i->orphan_inode_mutex);

	head = &cm_i->orphan_inode_list;
	list_for_each(this, head) {
		entry = list_entry(this, struct orphan_inode_entry, list);
		if (!orphan_addr) {
			orphan_block = alloc_new_node(sbi, 0, NULL, SUM_TYPE_CP);
			if (IS_ERR(orphan_block)) {
				ret = -ENOMEM;
				goto out;
			}
			orphan_addr = L_ADDR(sbi, orphan_block);
			summary = get_summary_by_addr(sbi, orphan_addr);
			make_summary_entry(summary, 0, cm_i->new_version, i,
					SUM_TYPE_ORPHAN);
			orphan_addrs[i++] = orphan_addr;
			orphan_block = ADDR(sbi, orphan_addr);
			/* Reseverd for checkpoint address */
			orphan_block = (__le32 *)JUMP(orphan_block, sizeof(__le64));
			end = (__le32 *)JUMP(orphan_block, HMFS_PAGE_SIZE);
		}
		*orphan_block = cpu_to_le32(entry->ino);
		orphan_block++;
		if (orphan_block == end) {
			orphan_addr = 0;
		}
	}
	hmfs_bug_on(sbi, i > NUM_ORPHAN_BLOCKS);
out:
	mutex_unlock(&cm_i->orphan_inode_mutex);
	return ret;
}

static void flush_orphan_inodes_finish(struct hmfs_sb_info *sbi, 
				block_t *orphan_addrs, block_t cp_addr) {
	int i;
	__le64 *orphan_block;
	struct hmfs_checkpoint *hmfs_cp = ADDR(sbi, cp_addr);

	for (i = 0; i < NUM_ORPHAN_BLOCKS; ++i, orphan_addrs++) {
		if (*orphan_addrs) {
			orphan_block = ADDR(sbi, *orphan_addrs);
			*orphan_block = cpu_to_le64(cp_addr);
			hmfs_cp->orphan_addrs[i] = cpu_to_le64(*orphan_addrs);
		} else
			break;
	}
}

static void recover_orphan_inode(struct hmfs_sb_info *sbi, nid_t ino)
{
	struct inode *inode = hmfs_iget(sbi->sb, ino);
	hmfs_bug_on(sbi, IS_ERR(inode));
	clear_nlink(inode);

	iput(inode);
}

/* Now we delete the orphan inodes */
int recover_orphan_inodes(struct hmfs_sb_info *sbi)
{
	int i;
	__le32 *orphan_block;
	__le32 *end;
	block_t orphan_addr;
	nid_t ino;
	struct hmfs_checkpoint *hmfs_cp = CM_I(sbi)->last_cp_i->cp;

	for (i = 0; i < NUM_ORPHAN_BLOCKS; ++i) {
		orphan_addr = le64_to_cpu(hmfs_cp->orphan_addrs[i]);
		if (!orphan_addr)
			return 0;
		orphan_block = ADDR(sbi, orphan_addr);
		end = (__le32 *)JUMP(orphan_block, HMFS_PAGE_SIZE);
		orphan_block = (__le32 *)JUMP(orphan_block, sizeof(__le64));
		while (orphan_block != end) {
			ino = le32_to_cpu(*orphan_block);
			recover_orphan_inode(sbi, ino);
			orphan_block++;
		}
	}

	return 0;
}

static int do_checkpoint(struct hmfs_sb_info *sbi, bool gc_cp)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	struct free_segmap_info *free_i = FREE_I(sbi);
	struct hmfs_nm_info *nm_i = NM_I(sbi);
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	struct hmfs_summary *summary;
	unsigned int cp_checksum, sb_checksum;
	ver_t store_version;
	block_t store_checkpoint_addr = 0;
	block_t nat_root_addr, orphan_addrs[2] = {0, 0};
	struct hmfs_nat_node *nat_root = NULL;
	struct hmfs_checkpoint *prev_checkpoint, *next_checkpoint;
	struct hmfs_checkpoint *store_checkpoint;
	struct curseg_info *curseg_i = SM_I(sbi)->curseg_array;
	int ret;

	prev_checkpoint = cm_i->last_cp_i->cp;
	next_checkpoint = ADDR(sbi, le64_to_cpu(prev_checkpoint->next_cp_addr));
	
	if (!gc_cp)
		set_fs_state(prev_checkpoint, HMFS_ADD_CP);

	/* GC process should not update nat tree */
	if (!gc_cp) {
		nat_root = flush_nat_entries(sbi);
		if (IS_ERR(nat_root))
			return PTR_ERR(nat_root);
		nat_root_addr = L_ADDR(sbi, nat_root);
	} else {
		nat_root_addr = le64_to_cpu(prev_checkpoint->nat_addr);
		nat_root = ADDR(sbi, nat_root_addr);
	}

	/* 1. set new cp block */
	if (!gc_cp)
		ret = flush_orphan_inodes(sbi, orphan_addrs);

	store_version = cm_i->new_version;
	store_checkpoint = alloc_new_node(sbi, 0, NULL, SUM_TYPE_CP);
	store_checkpoint_addr = L_ADDR(sbi, store_checkpoint);
	summary = get_summary_by_addr(sbi, store_checkpoint_addr);
	make_summary_entry(summary, 0, cm_i->new_version, 0, SUM_TYPE_CP);

	flush_orphan_inodes_finish(sbi, orphan_addrs, store_checkpoint_addr);

	store_checkpoint = ADDR(sbi, store_checkpoint_addr);
	store_checkpoint->next_cp_addr = prev_checkpoint->next_cp_addr;
	store_checkpoint->prev_cp_addr = next_checkpoint->prev_cp_addr;
	set_struct(store_checkpoint, nat_addr, nat_root_addr);

	set_struct(store_checkpoint, checkpoint_ver, store_version);
	set_struct(store_checkpoint, valid_block_count, cm_i->valid_block_count);
	set_struct(store_checkpoint, valid_inode_count, cm_i->valid_inode_count);
	set_struct(store_checkpoint, valid_node_count, cm_i->valid_node_count);
	set_struct(store_checkpoint, alloc_block_count, cm_i->alloc_block_count);
	set_struct(store_checkpoint, free_segment_count, free_i->free_segments);
	set_struct(store_checkpoint, cur_node_segno, curseg_i[CURSEG_NODE].segno);
	set_struct(store_checkpoint, cur_node_blkoff,
			curseg_i[CURSEG_NODE].next_blkoff);
	set_struct(store_checkpoint, cur_data_segno,
			curseg_i[CURSEG_DATA].segno);
	set_struct(store_checkpoint, cur_data_blkoff,
			curseg_i[CURSEG_DATA].next_blkoff);
	set_struct(store_checkpoint, next_scan_nid, nm_i->next_scan_nid);
	set_struct(store_checkpoint, elapsed_time, get_mtime(sbi));
	set_struct(store_checkpoint, type, gc_cp ? CP_GC : CP_NORMAL);

	cp_checksum = hmfs_make_checksum(store_checkpoint);
	set_struct(store_checkpoint, checksum, cp_checksum);

	/* 2. flush SIT to cp */
	flush_sit_entries(sbi, store_checkpoint_addr, nat_root, gc_cp);
	set_summary_valid_bit(summary);

	/* 6. connect to super */
	hmfs_memcpy_atomic(&prev_checkpoint->next_cp_addr, 
			&store_checkpoint_addr, 8);
	hmfs_memcpy_atomic(&next_checkpoint->prev_cp_addr,
			&store_checkpoint_addr, 8);
	hmfs_memcpy_atomic(&raw_super->cp_page_addr, &store_checkpoint_addr, 8);
	sb_checksum = hmfs_make_checksum(raw_super);

	set_struct(raw_super, checksum, sb_checksum);
	
	//TODO: memory barrier?
	raw_super = next_super_block(raw_super);
	hmfs_memcpy(raw_super, HMFS_RAW_SUPER(sbi), sizeof(struct hmfs_super_block));

	set_fs_state(prev_checkpoint, HMFS_NONE);
	move_to_next_checkpoint(sbi, store_checkpoint);

	return 0;
}

//      Step1: calculate info and write sit and nat to NVM
//      Step2: write CP itself to NVM
//      Step3: remaining job
int write_checkpoint(struct hmfs_sb_info *sbi, bool gc_cp)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	int ret;

	mutex_lock(&cm_i->cp_mutex);
	block_operations(sbi);

	ret = do_checkpoint(sbi, gc_cp);

	unblock_operations(sbi);
	mutex_unlock(&cm_i->cp_mutex);
	return ret;
}

int redo_checkpoint(struct hmfs_sb_info *sbi, struct hmfs_checkpoint *prev_cp)
{
	//XXX:after sbi initilization?
	struct hmfs_super_block *raw_super = HMFS_RAW_SUPER(sbi);
	struct hmfs_summary *summary;
	unsigned int sb_checksum;
	block_t store_cp_addr = 0;
	struct hmfs_checkpoint *next_cp;
	struct hmfs_checkpoint *store_cp;
	ver_t store_version;
	void *nat_root;

	/* 1. restore addr */
	store_cp_addr = le64_to_cpu(prev_cp->state_arg_2);
	store_cp = ADDR(sbi, store_cp_addr);

	hmfs_bug_on(sbi, L_ADDR(sbi, prev_cp)!=
			le64_to_cpu(store_cp->prev_cp_addr));

	summary = get_summary_by_addr(sbi, store_cp_addr);
	set_summary_valid_bit(summary);

	/* 2. flush cp-inlined SIT journal */
	recovery_sit_entries(sbi, prev_cp, false);

	/* 3. mark valid */
	store_version = le32_to_cpu(store_cp->checkpoint_ver);
	nat_root = ADDR(sbi, le32_to_cpu(store_cp->nat_addr));
	__mark_block_valid(sbi, nat_root, 0, store_version, sbi->nat_height);

	/* 4. connect to super */
	next_cp = ADDR(sbi, le64_to_cpu(store_cp->next_cp_addr));
	hmfs_memcpy_atomic(&prev_cp->next_cp_addr, &store_cp_addr, 8);
	hmfs_memcpy_atomic(&next_cp->prev_cp_addr, &store_cp_addr, 8);
	hmfs_memcpy_atomic(&raw_super->cp_page_addr, &store_cp_addr, 8);
	sb_checksum = hmfs_make_checksum(raw_super);
	set_struct(raw_super, checksum, sb_checksum);
	
	//TODO: memory barrier?
	raw_super = next_super_block(raw_super);
	hmfs_memcpy(raw_super, ADDR(sbi, 0), sizeof(struct hmfs_super_block));

	move_to_next_checkpoint(sbi, store_cp);

	return 0;
}

//      Step1: delete all valid counter
//      Step2: construct bypass link
//      Step3: delete checkpoint itself
int delete_checkpoint(struct hmfs_sb_info *sbi, unsigned int version)
{
	struct hmfs_checkpoint *checkpoint = NULL;
	struct hmfs_checkpoint *next_cp = NULL, *prev_cp = NULL;
	block_t nat_root_addr;

	printk(KERN_INFO "Delete checkpoint stage 1.\n");

	checkpoint = CM_I(sbi)->last_cp_i->cp;
	checkpoint = get_mnt_checkpoint(sbi, checkpoint, version);
	if (!checkpoint) {
		printk("Version %d not found.\n", version);
		return -1;
	}
	nat_root_addr = le64_to_cpu(checkpoint->nat_addr);
	dc_nat_root(sbi, nat_root_addr);

	printk(KERN_INFO "Delete checkpoint stage 2.\n");
	next_cp = ADDR(sbi, le64_to_cpu(checkpoint->next_cp_addr));
	prev_cp = ADDR(sbi, le64_to_cpu(checkpoint->prev_cp_addr));

	next_cp->prev_cp_addr = checkpoint->prev_cp_addr;
	prev_cp->next_cp_addr = checkpoint->next_cp_addr;

	printk(KERN_INFO "Delete checkpoint stage 3.\n");
	dc_checkpoint(sbi, L_ADDR(sbi, checkpoint));
	return 0;
}
