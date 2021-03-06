#include <linux/pagemap.h>
#include <crypto/hash.h>
#include <crypto/md5.h>
#include <crypto/sha.h>
#include <crypto/algapi.h>
#include "dedupe.h"
#include <linux/f2fs_fs.h>
#include <linux/vmalloc.h>

int f2fs_dedupe_calc_hash(struct page *p, u8 hash[], struct dedupe_info *dedupe_info)
{
	//int i;
	int ret;
	
	struct {
		struct shash_desc desc;
		char ctx[dedupe_info->crypto_shash_descsize];
	} sdesc;
	char *d;

	sdesc.desc.tfm = dedupe_info->tfm;
	sdesc.desc.flags = 0;
	ret = crypto_shash_init(&sdesc.desc);
	if (ret)
		return ret;

	d = kmap(p);
	ret = crypto_shash_digest(&sdesc.desc, d, PAGE_SIZE, hash);
	kunmap(p);

	/*for(i=0;i<4;i++)
	{
		printk("%llx",be64_to_cpu(*(long long*)&hash[i*8]));
	}
	printk("\n");*/

	return ret;
}

#ifdef F2FS_BLOOM_FILTER
int f2fs_dedupe_bloom_filter(u8 hash[], struct dedupe_info *dedupe_info)
{
	int i;
	unsigned int *pos = (unsigned int *)hash;
	for(i=0;i<dedupe_info->bloom_filter_hash_fun_count;i++)
	{
		if(0 == dedupe_info->bloom_filter[*(pos++)&dedupe_info->bloom_filter_mask])
		{
			return 1;//stand for not in hash table;
		}
	}
	return 0;
}

void init_f2fs_dedupe_bloom_filter(struct dedupe_info *dedupe_info)
{
	struct dedupe *cur;
	int i;
	for(cur = dedupe_info->dedupe_md; cur < dedupe_info->dedupe_md + dedupe_info->dedupe_block_count * DEDUPE_PER_BLOCK;cur++)
	{
		if(unlikely(cur->ref))
		{
			unsigned int *pos = (unsigned int *)cur->hash;
			for(i=0;i<dedupe_info->bloom_filter_hash_fun_count;i++)
			{
				dedupe_info->bloom_filter[*(pos++)&dedupe_info->bloom_filter_mask]++;
			}
		}
	}
}
#endif


struct dedupe *f2fs_dedupe_search(u8 hash[], struct dedupe_info *dedupe_info)
{
	struct dedupe *c = &dedupe_info->dedupe_md[(*(unsigned int *)hash)%(dedupe_info->dedupe_block_count/64) * DEDUPE_PER_BLOCK*64],*cur;
#ifdef F2FS_NO_HASH
	c = dedupe_info->dedupe_md;
#endif

#ifdef F2FS_BLOOM_FILTER
	if(f2fs_dedupe_bloom_filter(hash, dedupe_info)) return NULL;
#endif

	for(cur=c; cur < dedupe_info->dedupe_md + dedupe_info->dedupe_block_count * DEDUPE_PER_BLOCK; cur++)
	{
		if(unlikely(cur->ref&&!memcmp(hash, cur->hash, dedupe_info->digest_len)))
		{
			dedupe_info->logical_blk_cnt++;
			return cur;
		}
	}
	for(cur = dedupe_info->dedupe_md; cur < c; cur++)
	{
		if(unlikely(cur->ref&&!memcmp(hash, cur->hash, dedupe_info->digest_len)))
		{
			dedupe_info->logical_blk_cnt++;
			return cur;
		}
	}

	return NULL;
}

void set_dedupe_dirty(struct dedupe_info *dedupe_info, struct dedupe *dedupe)
{
	set_bit((dedupe - dedupe_info->dedupe_md)/DEDUPE_PER_BLOCK,  (long unsigned int *)dedupe_info->dedupe_md_dirty_bitmap);
}

void test_summary_table(struct dedupe_info *dedupe_info,int index)
{
	struct summary_table_entry *entry;
	struct dedupe *dedupe=&dedupe_info->dedupe_md[index];
	printk("---------------------summary table--------------------------\n");
	if(dedupe->start_pos_st!=-1)
	{
		entry=dedupe_info->sum_table+dedupe->start_pos_st;
		while(entry->next!=-1)
		{
			printk("nid:%d      ",entry->nid);
			printk("ofs_in_node:%d       ",entry->ofs_in_node);
			printk("next:%d\n",entry->next);
			if(entry->next!=-1)
			{
				entry=dedupe_info->sum_table+entry->next;
			}
		}
		printk("nid:%d      ",entry->nid);
		printk("ofs_in_node:%d       ",entry->ofs_in_node);
		printk("next:%d\n",entry->next);
	}
	else
		printk("---------------empty------------------------------\n");
}

int f2fs_del_summary_table_entry(struct dedupe_info *dedupe_info,int index,struct summary_table_entry *origin_summary,struct summary_table_entry del_summary)
{
	struct summary_table_entry *entry;
	struct summary_table_entry *pre_entry;
	struct dedupe *dedupe=&dedupe_info->dedupe_md[index];
	if(dedupe->start_pos_st!=-1)
	{
		if(origin_summary->nid==del_summary.nid
			&&origin_summary->ofs_in_node==del_summary.ofs_in_node)
		{
			entry=dedupe_info->sum_table+dedupe->start_pos_st;
			origin_summary->nid=entry->nid;
			origin_summary->ofs_in_node=entry->ofs_in_node;
			dedupe->start_pos_st=entry->next;
			entry->next=dedupe_info->sum_table->next;
			dedupe_info->sum_table->next=entry-dedupe_info->sum_table;
			return 1;
		}
		else
		{
			pre_entry=dedupe_info->sum_table+dedupe->start_pos_st;
			if(pre_entry->nid==del_summary.nid
			 &&pre_entry->ofs_in_node==del_summary.ofs_in_node)
			{
				dedupe->start_pos_st=pre_entry->next;
				pre_entry->next=dedupe_info->sum_table->next;
				dedupe_info->sum_table->next=pre_entry-dedupe_info->sum_table;
			}
			else
			{
				while(pre_entry->next!=-1)
				{
					entry=dedupe_info->sum_table+pre_entry->next;
					if(unlikely(entry->nid==del_summary.nid&&entry->ofs_in_node==del_summary.ofs_in_node))
					{
						pre_entry->next=entry->next;
						entry->next=dedupe_info->sum_table->next;
						dedupe_info->sum_table->next=entry-dedupe_info->sum_table;
						return 0;
					}
					else
						pre_entry=entry;
				}
			}
		}
	}

	return -1;
}

void f2fs_gc_change_reverse_and_bloom(struct dedupe_info *dedupe_info, block_t old_blkaddr,block_t new_blkaddr,int offset)
{
#ifdef F2FS_REVERSE_ADDR
	dedupe_info->reverse_addr[old_blkaddr]=-1;
	dedupe_info->reverse_addr[new_blkaddr]=offset;
#endif
	
}

int f2fs_dedupe_delete_addr(block_t addr, struct dedupe_info *dedupe_info,int *index)
{
	struct dedupe *cur,*c = dedupe_info->last_delete_dedupe;
	
	spin_lock(&dedupe_info->lock);
	if(NEW_ADDR == addr) return -1;

#ifdef F2FS_REVERSE_ADDR
	if(-1 == dedupe_info->reverse_addr[addr])
	{
		return -1;
	}
	cur = &dedupe_info->dedupe_md[dedupe_info->reverse_addr[addr]];
	if(cur->ref)
	{
		goto aa;
	}
	else
	{
		return -1;
	}
#endif

	for(cur=c; cur < dedupe_info->dedupe_md + dedupe_info->dedupe_block_count * DEDUPE_PER_BLOCK; cur++)
	{
		if(unlikely(cur->ref && addr == cur->addr))
		{
			*index=cur-dedupe_info->dedupe_md;
			cur->ref--;
			dedupe_info->logical_blk_cnt--;
			dedupe_info->last_delete_dedupe = cur;
			set_dedupe_dirty(dedupe_info, cur);
			if(0 == cur->ref)
			{
#ifdef F2FS_BLOOM_FILTER
				int i;
				unsigned int *pos = (unsigned int *)cur->hash;
				for(i=0;i<dedupe_info->bloom_filter_hash_fun_count;i++)
				{
					dedupe_info->bloom_filter[*(pos++)&dedupe_info->bloom_filter_mask]--;
				}
#endif
				*index=-1;
				cur->addr = 0;
				dedupe_info->physical_blk_cnt--;
				return 0;
			}
			else
			{
				return cur->ref;
			}
		}
	}
	for(cur = dedupe_info->dedupe_md; cur < c; cur++)
	{
		if(unlikely(cur->ref && addr == cur->addr))
		{
#ifdef F2FS_REVERSE_ADDR
aa:
#endif
			*index=cur-dedupe_info->dedupe_md;
			cur->ref--;
			dedupe_info->logical_blk_cnt--;
			dedupe_info->last_delete_dedupe = cur;
			set_dedupe_dirty(dedupe_info, cur);
			if(0 == cur->ref)
			{
#ifdef F2FS_BLOOM_FILTER
				int i;
				unsigned int *pos = (unsigned int *)cur->hash;
				for(i=0;i<dedupe_info->bloom_filter_hash_fun_count;i++)
				{
					dedupe_info->bloom_filter[*(pos++)&dedupe_info->bloom_filter_mask]--;
				}
#endif
				*index=-1;
				cur->addr = 0;
				dedupe_info->physical_blk_cnt--;

#ifdef F2FS_REVERSE_ADDR
				dedupe_info->reverse_addr[addr] = -1;
#endif
				return 0;
			}
			else
			{
				return cur->ref;
			}
		}
	}
	return -1;
}


int f2fs_dedupe_add(u8 hash[], struct dedupe_info *dedupe_info, block_t addr)
{
	int ret = 0;
	int search_count = 0;
	struct dedupe* cur = &dedupe_info->dedupe_md[(*(unsigned int *)hash)%(dedupe_info->dedupe_block_count/64) * DEDUPE_PER_BLOCK* 64];
#ifdef F2FS_NO_HASH
	cur = dedupe_info->dedupe_md;
#endif
	while(cur->ref)
	{
		if(likely(cur != dedupe_info->dedupe_md + dedupe_info->dedupe_block_count * DEDUPE_PER_BLOCK - 1))
		{
			cur++;
		}
		else
		{
			cur = dedupe_info->dedupe_md;
		}
		search_count++;
		if(search_count>dedupe_info->dedupe_block_count * DEDUPE_PER_BLOCK)
		{
			printk("can not add f2fs dedupe md.\n");
			ret = -1;
			break;
		}
	}
	if(0 == ret)
	{
#ifdef F2FS_BLOOM_FILTER
		unsigned int *pos;
		int i;
#endif
		cur->addr = addr;
		cur->ref = 1;
		cur->start_pos_st=-1;
		memcpy(cur->hash, hash, dedupe_info->digest_len);
#ifdef F2FS_REVERSE_ADDR
		dedupe_info->reverse_addr[addr] = cur - dedupe_info->dedupe_md;
#endif
#ifdef F2FS_BLOOM_FILTER
		pos = (unsigned int *)cur->hash;
		for(i=0;i<dedupe_info->bloom_filter_hash_fun_count;i++)
		{
			dedupe_info->bloom_filter[*(pos++)&dedupe_info->bloom_filter_mask]++;
			//printk("add %d\n", *(pos++)&dedupe_info->bloom_filter_mask);
		}
#endif
		set_dedupe_dirty(dedupe_info, cur);
		dedupe_info->logical_blk_cnt++;
		dedupe_info->physical_blk_cnt++;
	}
	return ret;
}

int f2fs_dedupe_O_log2(unsigned int x)
{
  unsigned char log_2[256] = {
    0,1,2,2,3,3,3,3,4,4,4,4,4,4,4,4,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,
    8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8,8
  };
  int l = -1;
  while (x >= 256) { l += 8; x >>= 8; }
  return l + log_2[x];
}

void init_summary_table(struct dedupe_info *dedupe_info)
{
	int i;
	struct summary_table_entry *entry;
	dedupe_info->sum_table=vmalloc(SUM_TABLE_LEN*sizeof(struct summary_table_entry));
	memset(dedupe_info->sum_table,0,SUM_TABLE_LEN*sizeof(struct summary_table_entry));
	entry=dedupe_info->sum_table;
	for(i=0;i<SUM_TABLE_LEN;i++)
	{
		(entry+i)->next=i+1;
	}
}

int init_dedupe_info(struct dedupe_info *dedupe_info)
{
	int ret = 0;
	dedupe_info->digest_len = 16;
	spin_lock_init(&dedupe_info->lock);
	INIT_LIST_HEAD(&dedupe_info->queue);
	dedupe_info->dedupe_md = vmalloc(dedupe_info->dedupe_size);
	memset(dedupe_info->dedupe_md, 0, dedupe_info->dedupe_size);
	dedupe_info->dedupe_md_dirty_bitmap = kzalloc(dedupe_info->dedupe_bitmap_size, GFP_KERNEL);
	dedupe_info->dedupe_segment_count = DEDUPE_SEGMENT_COUNT;
#ifdef F2FS_BLOOM_FILTER
	dedupe_info->bloom_filter_mask = (1<<(f2fs_dedupe_O_log2(dedupe_info->dedupe_block_count) + 10)) -1;
	dedupe_info->bloom_filter = vmalloc((dedupe_info->bloom_filter_mask + 1) * sizeof(unsigned int));
	memset(dedupe_info->bloom_filter, 0, dedupe_info->bloom_filter_mask * sizeof(unsigned int));
	dedupe_info->bloom_filter_hash_fun_count = 4;
#endif
	init_summary_table(dedupe_info);
	dedupe_info->last_delete_dedupe = dedupe_info->dedupe_md;
	dedupe_info->tfm = crypto_alloc_shash("md5", 0, 0);
	dedupe_info->crypto_shash_descsize = crypto_shash_descsize(dedupe_info->tfm);
	return ret;
}

void exit_dedupe_info(struct dedupe_info *dedupe_info)
{
	vfree(dedupe_info->dedupe_md);
	vfree(dedupe_info->sum_table);
	kfree(dedupe_info->dedupe_md_dirty_bitmap);
	kfree(dedupe_info->dedupe_bitmap);
#ifdef F2FS_REVERSE_ADDR
	vfree(dedupe_info->reverse_addr);
#endif
	crypto_free_shash(dedupe_info->tfm);
#ifdef F2FS_BLOOM_FILTER
	vfree(dedupe_info->bloom_filter);
#endif
}

//f2fs_gc_dedupe
void set_summary_table_entry(struct summary_table_entry *entry,__le32 nid,__le16 ofs_in_node)
{
	entry->nid=nid;
	entry->ofs_in_node=ofs_in_node;
}

int f2fs_add_summary_table_entry(struct dedupe_info *dedupe_info,struct dedupe *dedupe,__le32 nid,__le16 ofs_in_node)
{
	struct summary_table_entry *entry=NULL;
	if(unlikely(dedupe_info->sum_table->next>=SUM_TABLE_LEN))
	{
		printk("can't add summary table entry!\n");
		return -1;//beyond the array length;
	}
	entry=dedupe_info->sum_table+dedupe_info->sum_table->next;
	set_summary_table_entry(entry, nid, ofs_in_node);
	dedupe_info->sum_table->next=entry->next;
	entry->next=dedupe->start_pos_st;
	dedupe->start_pos_st=entry-dedupe_info->sum_table;
	return 0;//add success;
}


