#ifndef _DEDUPE_H
#define _DEDUPE_H

#define F2FS_BLOOM_FILTER 1
//#define F2FS_NO_HASH 1
#define F2FS_REVERSE_ADDR 1

#define DEDUPE_SEGMENT_COUNT 6
#define DEDUPE_PER_BLOCK (PAGE_CACHE_SIZE/sizeof(struct dedupe))
#define SUM_TABLE_LEN 250000

typedef u32 block_t;

struct dedupe
{
	block_t addr;
	int start_pos_st;//the start position of the summary table;
	int ref;
	u8 hash[16];
};

//F2FS_GC_DEDUPE
struct summary_table_entry{
	__le32 nid;
	__le16 ofs_in_node;
	unsigned int next;
};


struct dedupe_info
{
	int digest_len;
#ifdef F2FS_BLOOM_FILTER
	unsigned int bloom_filter_mask;
	unsigned int *bloom_filter;
	unsigned int bloom_filter_hash_fun_count;
#endif
	unsigned int logical_blk_cnt;
	unsigned int physical_blk_cnt;
	struct dedupe* dedupe_md;
	char *dedupe_md_dirty_bitmap;	/*bitmap for dirty dedupe blocks*/
	char *dedupe_bitmap;				/*bitmap for dedupe checkpoint*/
	unsigned int dedupe_segment_count;
	unsigned int dedupe_bitmap_size;	/*bitmap size of dedupe_md_dirty_bitmap&dedupe_bitmap*/
	unsigned int dedupe_size;			/*size of dedupes in memory*/
	unsigned int dedupe_block_count;
	struct dedupe* last_delete_dedupe;
	struct list_head queue;
	spinlock_t lock;
	struct crypto_shash *tfm;
	unsigned int crypto_shash_descsize;
	struct summary_table_entry *sum_table;
#ifdef F2FS_REVERSE_ADDR
	int *reverse_addr;
#endif
};


extern int f2fs_dedupe_calc_hash(struct page *p, u8 hash[], struct dedupe_info *dedupe_info);
extern struct dedupe *f2fs_dedupe_search(u8 hash[], struct dedupe_info *dedupe_info);
extern int f2fs_dedupe_add(u8 hash[], struct dedupe_info *dedupe_info, block_t addr);
extern int init_dedupe_info(struct dedupe_info *dedupe_info);
extern void init_f2fs_dedupe_bloom_filter(struct dedupe_info *dedupe_info);
extern void exit_dedupe_info(struct dedupe_info *dedupe_info);
extern int f2fs_dedupe_delete_addr(block_t addr, struct dedupe_info *dedupe_info,int *index);
extern void set_dedupe_dirty(struct dedupe_info *dedupe_info, struct dedupe *dedupe);
extern int f2fs_add_summary_table_entry(struct dedupe_info *dedupe_info,struct dedupe *dedupe,__le32 nid,__le16 ofs_in_node);
extern int f2fs_del_summary_table_entry(struct dedupe_info *dedupe_info,int index,struct summary_table_entry *origin_summary,struct summary_table_entry del_summary);
extern void test_summary_table(struct dedupe_info * dedupe_info, int index);
extern void f2fs_gc_change_reverse_and_bloom(struct dedupe_info *dedupe_info, block_t old_blkaddr,block_t new_blkaddr,int offset);
#endif

