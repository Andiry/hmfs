#ifndef HMFS_SEGMENT_H
#define HMFS_SEGMENT_H

#include "hmfs.h"

#define SIT_ENTRY_CLEAN			0
#define SIT_ENTRY_DIRTY				1

#define MAX_SIT_ITEMS_FOR_GANG_LOOKUP		10240

#define hmfs_bitmap_size(nr)			\
	(BITS_TO_LONGS(nr) * sizeof(unsigned long))
#define TOTAL_SEGS(sbi)	(SM_I(sbi)->main_segments)

/* constant macro */
#define NULL_SEGNO			((unsigned int)(~0))

#define LIMIT_INVALID_BLOCKS	50	/* percentage over total user space */
#define LIMIT_FREE_BLOCKS		50	/* percentage of free blocks over total user space */
#define SEVERE_FREE_BLOCKS		75	/* percentage of free blocks over total in emergency case */
#define NR_MAX_FG_SEGS			200

struct seg_entry {
	unsigned short valid_blocks;	/* # of valid blocks */
	unsigned long mtime;	/* modification time of the segment */
};

struct sit_info {
	unsigned long long bitmap_size;

	unsigned long *dirty_sentries_bitmap;	/* bitmap for dirty sentries */
	unsigned int dirty_sentries;			/* # of dirty sentries */
	struct mutex sentry_lock;				/* to protect SIT cache */
	struct seg_entry *sentries;				/* SIT segment-level cache */

	/* for cost-benefit valuing */
	unsigned long long elapsed_time;	/* The elapsed time from FS format */
	unsigned long long mounted_time;	/* Timestamp for FS mounted */
	unsigned long long min_mtime;		/* Minimum mtime in SIT */
	unsigned long long max_mtime;		/* Maximum mtime in SIT */
};

/* Dirty segment is the segment which has both valid blocks and invalid blocks */
struct dirty_seglist_info {
	unsigned long *dirty_segmap;		/* bitmap for dirty segment */
};

/* Free segment is the segment which does not have valid blocks */
struct free_segmap_info {
	pgc_t free_segments;			/* # of free segments */
	rwlock_t segmap_lock;				/* free segmap lock */
	unsigned long *free_segmap;			/* free segment bitmap */
	unsigned long *prefree_segmap;
};

/* for active log information */
struct curseg_info {
	struct mutex curseg_mutex;	/* lock for consistency */
	atomic_t segno;				/* current segment number */
	unsigned short next_blkoff;	/* next block offset to write */
	seg_t next_segno;			/* preallocated segment */
};

struct hmfs_sm_info {
	struct sit_info *sit_info;				/* whole segment information */
	struct free_segmap_info *free_info;		/* free segment information */
	struct dirty_seglist_info *dirty_info;	/* dirty segment information */
	struct curseg_info *curseg_array;		/* active segment information */

	pgc_t segment_count;				/* total # of segments */
	pgc_t main_segments;				/* # of segments in main area */
	pgc_t reserved_segments;			/* # of reserved segments */
	pgc_t ovp_segments;				/* # of overprovision segments */
	pgc_t limit_invalid_blocks;		/* # of limit invalid blocks */
	pgc_t limit_free_blocks;		/* # of limit free blocks */
	pgc_t severe_free_blocks;		/* # of free blocks in emergency case */
};

/* Segment inlined functions */
static inline void lock_read_segmap(struct free_segmap_info *free_i)
{
	read_lock(&free_i->segmap_lock);
}

static inline void unlock_read_segmap(struct free_segmap_info *free_i)
{
	read_unlock(&free_i->segmap_lock);
}

static inline void lock_write_segmap(struct free_segmap_info *free_i)
{
	write_lock(&free_i->segmap_lock);
}

static inline void unlock_write_segmap(struct free_segmap_info *free_i)
{
	write_unlock(&free_i->segmap_lock);
}

static inline void lock_sentry(struct sit_info *sit_i)
{
	mutex_lock(&sit_i->sentry_lock);
}

static inline void unlock_sentry(struct sit_info *sit_i)
{
	mutex_unlock(&sit_i->sentry_lock);
}

static inline void lock_curseg(struct curseg_info *seg_i)
{
	mutex_lock(&seg_i->curseg_mutex);
}

static inline void unlock_curseg(struct curseg_info *seg_i)
{
	mutex_unlock(&seg_i->curseg_mutex);
}

static inline struct hmfs_sm_info *SM_I(struct hmfs_sb_info *sbi)
{
	return sbi->sm_info;
}

static inline struct sit_info *SIT_I(struct hmfs_sb_info *sbi)
{
	return (SM_I(sbi)->sit_info);
}

static inline struct seg_entry *get_seg_entry(struct hmfs_sb_info *sbi,
					      seg_t segno)
{
	return &(SIT_I(sbi)->sentries[segno]);
}

static inline unsigned int get_valid_blocks(struct hmfs_sb_info *sbi,
					    seg_t segno)
{
	return get_seg_entry(sbi, segno)->valid_blocks;
}

static inline struct hmfs_sit_entry *get_sit_entry(struct hmfs_sb_info *sbi,
						   seg_t segno)
{
	return &sbi->sit_entries[segno];
}

static inline struct curseg_info *CURSEG_I(struct hmfs_sb_info *sbi)
{
	return SM_I(sbi)->curseg_array;
}

static inline struct free_segmap_info *FREE_I(struct hmfs_sb_info *sbi)
{
	return SM_I(sbi)->free_info;
}

static inline struct dirty_seglist_info *DIRTY_I(struct hmfs_sb_info *sbi)
{
	return SM_I(sbi)->dirty_info;
}

static inline seg_t find_next_inuse(struct free_segmap_info *free_i,
					   seg_t max, seg_t segno)
{
	seg_t ret;

	lock_read_segmap(free_i);
	ret = find_next_bit(free_i->free_segmap, max, segno);
	unlock_read_segmap(free_i);
	return ret;
}

static inline pgc_t overprovision_segments(struct hmfs_sb_info *sbi)
{
	return SM_I(sbi)->ovp_segments;
}

static inline pgc_t free_segments(struct hmfs_sb_info *sbi)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	pgc_t free_segs;

	lock_read_segmap(free_i);
	free_segs = free_i->free_segments;
	unlock_read_segmap(free_i);

	return free_segs;
}

static inline pgc_t free_user_blocks(struct hmfs_sb_info *sbi)
{
	if (free_segments(sbi) < overprovision_segments(sbi))
		return 0;
	else
		return (free_segments(sbi) - overprovision_segments(sbi))
						<< HMFS_PAGE_PER_SEG_BITS;
}

static inline bool has_enough_invalid_blocks(struct hmfs_sb_info *sbi)
{
	struct hmfs_cm_info *cm_i = CM_I(sbi);
	struct hmfs_sm_info *sm_i = SM_I(sbi);
	unsigned long invalid_user_blocks = cm_i->alloc_block_count
						- cm_i->valid_block_count;

	hmfs_bug_on(sbi, cm_i->alloc_block_count < cm_i->valid_block_count);

	if (invalid_user_blocks > sm_i->limit_invalid_blocks
	    && free_user_blocks(sbi) < sm_i->limit_free_blocks)
		return true;
	return false;
}

static inline bool has_not_enough_free_segs(struct hmfs_sb_info *sbi)
{
	return free_user_blocks(sbi) < SM_I(sbi)->limit_free_blocks;
}

static inline unsigned long long get_mtime(struct hmfs_sb_info *sbi)
{
	struct sit_info *sit_i = SIT_I(sbi);

	return sit_i->elapsed_time + CURRENT_TIME_SEC.tv_sec -
	 sit_i->mounted_time;
}

static inline void seg_info_from_raw_sit(struct seg_entry *se,
					 struct hmfs_sit_entry *raw_entry)
{
	se->valid_blocks = le16_to_cpu(raw_entry->vblocks);
	se->mtime = le32_to_cpu(raw_entry->mtime);
}

static inline void seg_info_to_raw_sit(struct seg_entry *se,
				       struct hmfs_sit_entry *raw_entry)
{
	raw_entry->vblocks = cpu_to_le16(se->valid_blocks);
	raw_entry->mtime = cpu_to_le32(se->mtime);
}

static inline void __set_inuse(struct hmfs_sb_info *sbi, seg_t segno)
{
	struct free_segmap_info *free_i = FREE_I(sbi);
	set_bit(segno, free_i->free_segmap);
	free_i->free_segments--;
}

#endif
