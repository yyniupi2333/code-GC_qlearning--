/*
 * YAFFS: Yet Another Flash File System. A NAND-flash specific file system.
 *
 * Copyright (C) 2002-2018 Aleph One Ltd.
 *
 * Created by Charles Manning <charles@aleph1.co.uk>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include "yportenv.h"
#include "yaffs_trace.h"

#include "yaffs_guts.h"

#include "yaffs_cache.h"
#include "yaffs_endian.h"
#include "yaffs_getblockinfo.h"
#include "yaffs_tagscompat.h"
#include "yaffs_tagsmarshall.h"
#include "yaffs_nand.h"
#include "yaffs_yaffs1.h"
#include "yaffs_yaffs2.h"
#include "yaffs_bitmap.h"
#include "yaffs_verify.h"
#include "yaffs_nand.h"
#include "yaffs_packedtags2.h"
#include "yaffs_nameval.h"
#include "yaffs_allocator.h"
#include "yaffs_attribs.h"
#include "yaffs_summary.h"
volatile unsigned chunk_seq=0;//add by ed 2024.04.17
/* Note YAFFS_GC_GOOD_ENOUGH must be <= YAFFS_GC_PASSIVE_THRESHOLD */
#define YAFFS_GC_GOOD_ENOUGH 2
#define YAFFS_GC_PASSIVE_THRESHOLD 4
#define TIME_NOW chunk_seq

#include "yaffs_ecc.h"

/* Forward declarations */

static void yaffs_fix_null_name(struct yaffs_obj *obj, YCHAR *name,
				int buffer_size);

// /* Added by Ed. 2024.04.22 */
// #if defined(GC_GR) ||defined(GC_CAT)||defined(GC_CB) || defined(GC_FeGC) || defined(GC_MSCGC)
static unsigned yaffs_gc_urgency(struct yaffs_dev *dev)
{
	unsigned erased_chunks =
	    dev->n_erased_blocks * dev->param.chunks_per_block;
	if (erased_chunks < dev->n_free_chunks/10  /* &&  dev->n_erased_blocks < thresh */)
	{
	  //   printk("\nit's ok to start gc_urgency,%d free blocks left\n ",dev->n_erased_blocks);
         return 1;
	}
	return 0;
}


#if defined(GC_Qlearning)
static unsigned time_alloc_block[BLOCK_NUMBER];  //record the time when block was allocated
static struct fagc_cps_item sum_block[BLOCK_NUMBER];  
volatile unsigned start_time =0;
static int exploit_gc_copies;

#define FRACTION_BITS 15

static inline uint32_t clz32(uint32_t x) {
    if (x == 0) return 32;
    uint32_t n = 0;
    if (x <= 0x0000FFFF) { n += 16; x <<= 16; }
    if (x <= 0x00FFFFFF) { n += 8; x <<= 8; }
    if (x <= 0x0FFFFFFF) { n += 4; x <<= 4; }
    if (x <= 0x3FFFFFFF) { n += 2; x <<= 2; }
    if (x <= 0x7FFFFFFF) { n += 1; }
    return n;
}

uint32_t fixed_log2(uint32_t x) {
    uint32_t m = 31 - clz32(x);
    uint64_t temp = (uint64_t)x << 15;
    uint32_t a_q15 = (uint32_t)(temp >> m);
    uint32_t g = a_q15 - (1 << FRACTION_BITS);
    uint32_t m_q15 = m << FRACTION_BITS;
    return m_q15 + g;
}

/**
 * 计算两个数的几何平均数（纯整数运算）
 * 几何平均 = sqrt(a * b) ≈ 2^((log2(a) + log2(b))/2)
 * 
 * 使用定点算术避免浮点数
 */
static inline s64 geometric_mean(s64 a, s64 b) {
	if (a <= 0 || b <= 0) {
		/* 边界情况：返回算术平均 */
		return (a + b) / 2;
	}
	
	/* 计算对数和 */
	uint32_t log_a = fixed_log2((uint32_t)a);
	uint32_t log_b = fixed_log2((uint32_t)b);
	uint32_t log_sum = log_a + log_b;
	uint32_t log_avg = log_sum / 2;  /* 对数平均 */
	
	/* 转换回线性空间：2^(log_avg) */
	/* 使用近似：2^x ≈ (x >> 15) * (1 + (x & 0x7FFF) / 32768) */
	uint32_t frac = log_avg & 0x7FFF;  /* 小数部分 */
	uint32_t int_part = log_avg >> 15;  /* 整数部分 */
	
	if (int_part >= 31) return LLONG_MAX;  /* 防止溢出 */
	
	/* 近似计算 2^frac，使用泰勒展开 */
	/* 2^x ≈ 1 + x*ln(2) + (x*ln(2))^2/2 + ... */
	/* 简化：使用查表或直接计算 */
	u64 result = 1ULL << int_part;
	
	/* 调整小数部分的贡献 */
	if (frac > 0) {
		/* 2^(frac/32768) ≈ 1 + frac*ln(2)/32768 */
		result = result + (result * frac) / (32768 / 1);  /* ln(2) ≈ 1 */
	}
	
	return (s64)result;
}


#endif

#if defined(GC_Qlearning)
static int Twl=	INIT_TWL;		/* Threshold of wearing level */
static int last_erasure_count=0;
extern int n_blk_erasure_max;
extern int n_blk_erasure_min;
extern int n_blk_erasure_counts;


static int thresh_val=0;  //this is the result of alpha multiply by INIT_TWL
static unsigned fagc_check_erase_coldest(struct yaffs_dev *dev)
{
	return  n_blk_erasure_counts - last_erasure_count >=Twl ? 1:0;
}
unsigned int get_min_erase(void)//遍历算法效率不够高，或许该在每次擦除操作时更新最小擦除次数（可以尝试修改）
{
	extern int n_blk_erasures[BLOCK_NUMBER];
	int i = 0;
	unsigned int min =0x1FFFFFFF;	
	for(i =0;i<BLOCK_NUMBER;i++)
	{
		if(min > n_blk_erasures[i])
		{
			min = n_blk_erasures[i];
		}
	}
	return min;
}
static void fagc_update_threshold(void)
{	
	if(thresh_val < 0)
	{
		thresh_val = -thresh_val;
	}
	Twl=INIT_TWL - thresh_val - (n_blk_erasure_max-n_blk_erasure_min);
	if( Twl < Tmin )
	{
		Twl=Tmin;
	}
	if( Twl> INIT_TWL)
	{
		Twl=INIT_TWL;
	}
	last_erasure_count=n_blk_erasure_counts;


	
}
static void fagc_update_alpha(int cnt)
{
	thresh_val = INIT_TWL * cnt / BLOCK_NUMBER;
}

#endif


#if defined(GC_Qlearning)
/*
文件大小为0~1024k，而一个页大小为4k，故一个文件最多需要256页
未精确区分一个文件中的逻辑页，可用chunk_id的高八位表示
文件id或许不用那么精确地区分？
*/
static struct fagc_hdt_item hdt[4194304];
void get_time_hot(unsigned object_id,unsigned chunk_id, unsigned *time, int *cnt )
{
	unsigned index;

	index = (chunk_id&0xff)|(object_id<<8);
	if(index>=4194304) printk("unusable index ,locate in %d",__LINE__);

	*time=hdt[index].time;
	*cnt=hdt[index].cnt;
	
}

void set_time_hot(unsigned object_id,unsigned chunk_id, unsigned time,int cnt)
{
	unsigned index;

	index = (chunk_id&0xff)|(object_id<<8);
	if(index>=4194304) printk("unusable index ,locate in %d",__LINE__);
	hdt[index].time=time;
	hdt[index].cnt=cnt;

}
#endif



//add by ry 2024.04.29 //定义q-table数据结构及操作，状态，动作空间
#ifdef GC_Qlearning 
		static int Qlearning_count=0;
		// static struct State* last_last_state=NULL;
		// static enum Action last_last_action;
		static struct State* last_state=NULL;
		static enum Action last_action=ACTION_ZERO;
		static int number_of_migrations;
		static int new_start;
		struct QTableEntry* QTable;
		struct HashNode* BTable[BLOCK_NUMBER]={NULL}; 
		//new
		struct block_alloc_time bat[BLOCK_NUMBER];
		//end
		static u32 CAPACITY=256;
		int learningRate=1;//learningRate_mul_ten
		int discountFactor=5;//discountFactor_mul_ten
		const static unsigned num_trainings=50000;//小q表减少训练次数
		// const static unsigned num_trainings=1000000;//训练次数
		/* epsilon 初始值 = num_trainings（100%探索）
		 * 通过衰减策略逐步降低探索概率
		 * 最终保留 MIN_EPSILON（5%）的永久探索，应对环境变化
		 */
		static unsigned epsilon=num_trainings;//探索率

/* ============ 调试监控统计结构体 ============ */
#define DEBUG_QLGC  /* 启用调试信息 */

#ifdef DEBUG_QLGC
struct {
	/* 动作统计 */
	unsigned action_count[4];      /* ACTION_ZERO, ONE, TWO, THREE的计数 */
	unsigned total_actions;        /* 总动作数 */
	
	/* 状态统计 */
	unsigned unique_states_visited;/* 访问过的不同状态数 */
	unsigned state_action_pairs;   /* 状态-动作对数量 */
	
	/* Q值统计 */
	int max_qvalue;
	int min_qvalue;
	int avg_qvalue;
	int qvalue_variance;
	
	/* 奖励统计 */
	unsigned long total_reward;    /* 累积奖励 */
	unsigned gc_operations;        /* GC操作总次数 */
	unsigned total_migrations;     /* 总页面迁移数 */
	unsigned total_deleted_pages;  /* 总无效页数 */
	
	/* 哈希表碰撞统计 */
	unsigned hash_collisions;
	unsigned q_entries_used;
	
	/* GC效率统计 */
	unsigned long chunks_recovered_total;  /* 总恢复块数 */
	unsigned gc_success_count;     /* GC成功次数 */
	unsigned gc_fail_count;        /* GC失败次数 */
	
	/* 磨损均衡统计 */
	int last_wear_gap;             /* 上次擦除差距 */
	int max_wear_gap;              /* 最大擦除差距 */
	
	/* 上次打印的时间戳 */
	unsigned last_print_count;
} ql_stats = {0};

#endif

/* ============ 简化离散化器 - 用于UFT特征离散化 ============ */
/* 每个分组独立维护min/max值 */
/* 注意：Q表有64个状态，所以最多支持64个分组 */
struct {
	/* 分组信息 */
	struct {
		s64 min_val;        /* 该分组的最小值 */
		s64 max_val;        /* 该分组的最大值 */
		u32 sample_count;   /* 该分组的样本数 */
	} groups[64];           /* 支持最多64个分组，每个分组对应一个状态ID */
	u32 group_count;        /* 活跃分组数 */
	u32 total_samples;      /* 总采样次数 */
} uft_discretizer = {
	.group_count = 1,
	.total_samples = 0,
	.groups = {
		[0] = { .min_val = LLONG_MAX, .max_val = LLONG_MIN, .sample_count = 0 },
	}
};

/* 离散器打印计数器：每执行100次GC打印一次分组范围 */
static u32 discretizer_print_count = 0;

/**
 * 打印所有活跃分组的范围信息
 */
static void print_discretizer_groups(void) {
	u32 i;
	if (uft_discretizer.group_count == 0) return;
	
	printk("[UFT-Discretizer] Group count: %u, Total samples: %u\n", 
	       uft_discretizer.group_count, uft_discretizer.total_samples);
	
	for (i = 0; i < uft_discretizer.group_count; i++) {
		printk("  Group-%u: [%lld, %lld] samples=%u\n",
		       i,
		       uft_discretizer.groups[i].min_val,
		       uft_discretizer.groups[i].max_val,
		       uft_discretizer.groups[i].sample_count);
	}
}

/**
 * 离散化UFT值 - 替代 get_uft_flag()
 * 将连续的UFT值自适应地离散化为 0-63 的分组ID
 * 
 * 关键条件：
 * - 每个分组必须满足：max_val < 2 * min_val
 * - 当某分组违反该条件且样本数足够时，进行分裂
 * - 最多8个分组（以节省状态空间）
 */
unsigned discretize_uft_value(s64 uft_value) {
	u32 best_group = 0;
	s32 left, right, mid;
	
	/* 第一个样本：初始化第一个分组 */
	if (uft_discretizer.total_samples == 0) {
		uft_discretizer.groups[0].min_val = uft_value;
		uft_discretizer.groups[0].max_val = uft_value;
		uft_discretizer.groups[0].sample_count = 1;
		uft_discretizer.total_samples = 1;
		return 0;
	}
	
	uft_discretizer.total_samples++;
	
	/* ===== 二分查找：找到值应该属于的分组 ===== */
	/* 分组按创建顺序有序：groups[0].max <= groups[1].min <= groups[1].max <= ... */
	left = 0;
	right = (s32)uft_discretizer.group_count - 1;
	
	while (left <= right) {
		mid = left + (right - left) / 2;
		s64 group_min = uft_discretizer.groups[mid].min_val;
		s64 group_max = uft_discretizer.groups[mid].max_val;
		
		/* 精确匹配：值在该分组范围内 */
		if (uft_value >= group_min && uft_value <= group_max) {
			best_group = mid;
			break;
		}
		/* 值小于中间分组，搜索左半部分 */
		else if (uft_value < group_min) {
			right = mid - 1;
		}
		/* 值大于中间分组，搜索右半部分 */
		else {
			left = mid + 1;
		}
	}
	
	/* 二分查找结束后，left指向第一个min > uft_value的分组 */
	/* 如果没有精确匹配，选择left作为扩展分组 */
	if (uft_value < uft_discretizer.groups[best_group].min_val || 
	    uft_value > uft_discretizer.groups[best_group].max_val) {
		if (left < (s32)uft_discretizer.group_count) {
			best_group = left;
		} else {
			/* 值大于所有分组，扩展最后一个分组 */
			best_group = uft_discretizer.group_count - 1;
		}
	}
	
	/* 更新该分组的范围 */
	if (uft_discretizer.groups[best_group].sample_count == 0) {
		uft_discretizer.groups[best_group].min_val = uft_value;
		uft_discretizer.groups[best_group].max_val = uft_value;
	} else {
		if (uft_value < uft_discretizer.groups[best_group].min_val) 
			uft_discretizer.groups[best_group].min_val = uft_value;
		if (uft_value > uft_discretizer.groups[best_group].max_val) 
			uft_discretizer.groups[best_group].max_val = uft_value;
	}
	uft_discretizer.groups[best_group].sample_count++;
	
	/* ===== 检查是否需要分裂 ===== */
	/* 条件1：样本数足够（至少100个） */
	/* 条件2：该分组违反 max < 2*min 的条件 */
	/* 条件3：还有分组空间（< 64个，因为Q表只有64个状态） */
	if (uft_discretizer.groups[best_group].sample_count >= 100 &&
		uft_discretizer.group_count < 64 &&
		uft_discretizer.groups[best_group].min_val > 0) {
		
		s64 group_min = uft_discretizer.groups[best_group].min_val;
		s64 group_max = uft_discretizer.groups[best_group].max_val;
		
		/* 检查是否违反 max < 2*min 条件 */
		int violates_condition = (group_max >= 2 * group_min);
		
		if (violates_condition) {
			/* 分裂：使用几何平均作为分裂点 */
			s64 split_point = geometric_mean(group_min, group_max);
			u32 new_group_idx;
			
			/* 确保分裂点在有效范围内 */
			if (split_point <= group_min) {
				split_point = group_min + 1;
			}
			if (split_point >= group_max) {
				split_point = group_max - 1;
			}
			
			new_group_idx = uft_discretizer.group_count;
			
			/* 新分组：[split_point, group_max] */
			uft_discretizer.groups[new_group_idx].min_val = split_point;
			uft_discretizer.groups[new_group_idx].max_val = group_max;
			uft_discretizer.groups[new_group_idx].sample_count = 0;
			
			/* 旧分组调整：[group_min, split_point] */
			uft_discretizer.groups[best_group].max_val = split_point;
			
			uft_discretizer.group_count++;
		}
	}
	
	/* 直接返回分组ID作为状态ID（0-63） */
	/* 每个分组对应一个唯一的状态，不需要重新映射 */
	
	/* 每执行100次离散化，打印一次分组信息 */
	// discretizer_print_count++;
	// if (discretizer_print_count % 100 == 0) {
	// 	print_discretizer_groups();
	// }
	
	return best_group % 64;  /* 安全防护：确保不超过64 */
}

unsigned get_uft_flag(unsigned uft, unsigned aver_uft) {
	/* 使用离散化器替代之前的固定策略 */
	return discretize_uft_value(uft);
}

struct State* get_usable_state(struct yaffs_ext_tags *tags, unsigned aver_uft) {
    unsigned now = TIME_NOW;
    unsigned time ;
    unsigned cnt ;

    get_time_hot(tags->obj_id,tags->chunk_id, &time, &cnt);
	int uft = (cnt > 0) ? (now - time) / cnt : (now - time);  /* 修复除零风险 */ 

    // 分配内存给 state
    struct State* state = (struct State*)kvmalloc(sizeof(struct State), GFP_KERNEL);
    if (!state) {
        printk("Error: Memory allocating failed\n");
        return NULL;
    }
    state->uft_flag=get_uft_flag(uft,aver_uft);
    return state;
}
unsigned enumtoint(enum Action action){
	unsigned ac_num;
	if(action==ACTION_ZERO) ac_num=0;
	else if(action==ACTION_ONE) ac_num=1;
	else if(action==ACTION_TWO) ac_num=2;
	else ac_num=3;
	return ac_num;
}

//不确定uft与aver_uft范围，可采用降维（耗时），或者扩容，或者uft根据aver_uft多分几个类，这样q表必然是很小的（太小了效果到底好不好？）
unsigned int hashStateActionPair(struct State* state, enum Action action) {  
    unsigned ac_num=enumtoint(action);
    int index = (state->uft_flag<<2) | ac_num;
	if(index>=CAPACITY){ 
		printk("error index in %d",__LINE__);
		return index % CAPACITY;  /* 使用模运算避免超界 */
	}
	return index; 

}

struct QTableEntry* initializeQTable(u64 capacity) {//动态数组在堆区分配
    struct QTableEntry* table = (struct QTableEntry*)kvmalloc(capacity * sizeof(struct QTableEntry), GFP_KERNEL);
    if (!table) {
		printk("error in %d",__LINE__);
        return NULL;
    }
	printk("capacity is %d",capacity);
	u32 i;
	for (i = 0; i < capacity; ++i) {
		// table[i].block = 0;
        table[i].qValue = 0;
        table[i].state = NULL;
        table[i].action = ACTION_ZERO; // 或者其他合适的初始值
    }
    return table;
} 


// 清理 Q-table
void freeQTable(struct  QTableEntry* table) {
	if (table) {
		u32 i;
        for (i = 0; i<CAPACITY; ++i) {
            if (table[i].state) {
                kvfree(table[i].state);
            }
        }
	kvfree(table);
	}
}

struct QTableEntry* findOrInsertQEntry(struct QTableEntry* table, struct State* state, enum Action action) {
    if (table == NULL) {
		printk("Q-table pointer is NULL");
        return NULL;
    }
    u32 index = hashStateActionPair(state, action);
	if(table[index].state != NULL) return &table[index];
	else {
		table[index].state = state;
		table[index].action = action;
		ql_stats.state_action_pairs++;  /* 统计新添加的状态-动作对 */
		if(ql_stats.state_action_pairs > CAPACITY) {
			ql_stats.state_action_pairs = CAPACITY;  /* 防止溢出 */
		}
	}
    return &table[index];
}


//exploit阶段获取当前状态下最佳action
enum Action getbestaction(struct QTableEntry* table,struct State* state){
	int maxqvalue=-2147483648;  /* INT_MIN */
	enum Action action=ACTION_ZERO;  /* 初始化防止未定义行为 */
	u32 i;
	for( i=0;i<4;i++){
		enum Action temp;
		if(i==0) temp=ACTION_ZERO;
		else if(i==1) temp=ACTION_ONE;
		else if(i==2) temp=ACTION_TWO;
		else if(i==3) temp=ACTION_THREE;
		struct QTableEntry * temp_QE=findOrInsertQEntry(table,state, temp);
		if(temp_QE && temp_QE->qValue > maxqvalue){  /* 添加NULL检查 */
			maxqvalue=temp_QE->qValue;
		    action=temp;
		}
	}
	return action;
}
//expore阶段随机获取action
enum Action getrandomaction(struct QTableEntry* table,struct State* state){
	int i =prandom_u32()%4;
	enum Action temp;
	if(i==0) temp=ACTION_ZERO;
	else if(i==1) temp=ACTION_ONE;
	else if(i==2) temp=ACTION_TWO;
	else if(i==3) temp=ACTION_THREE;
	struct QTableEntry* entry = findOrInsertQEntry(table,state, temp);
	if(entry) {}  /* 确保条目存在 */
	return temp; 
}

//根据epsilon按概率选择explore与exploit
/* epsilon-Greedy 策略：
 * - 当 random() > epsilon 时：利用最优策略 (exploitation)
 * - 当 random() <= epsilon 时：随机探索 (exploration)
 * 
 * epsilon 随训练进度衰减：
 * - 初期：epsilon ≈ num_trainings（高探索）
 * - 中期：epsilon 线性降低（逐步利用）
 * - 后期：epsilon ≥ MIN_EPSILON（保留5%探索，适应环境变化）
 */
enum Action getaction(struct QTableEntry *table,struct State* state){
	enum Action action;
	if ((prandom_u32()%num_trainings +1)>epsilon){
		action=getbestaction(table,state);}  /* 利用已学策略 */
	else{
		action=getrandomaction(table,state);  /* 探索新动作 */
	}
	
	/* ========== 调试统计 ========== */
#ifdef DEBUG_QLGC
	ql_stats.action_count[enumtoint(action)]++;
	ql_stats.total_actions++;
	ql_stats.unique_states_visited++;  /* 这个计数可能不够精确，实际应该用hash集合 */
#endif
	
	return action;
}


int getqvalue(struct QTableEntry* table,struct State* state, enum Action action){
	struct QTableEntry* temp=findOrInsertQEntry(table, state, action);
	return (temp != NULL) ? temp->qValue : 0;  /* 添加NULL检查 */
}
//获取当前状态下最大q值
int getbestqvalue(struct QTableEntry* table,struct State* state){
	int maxqvalue=-2147483648;
	enum Action temp;
	u32 i;
	for(i=0;i<4;i++){
		if(i==0) temp=ACTION_ZERO;
		else if(i==1) temp=ACTION_ONE;
		else if(i==2) temp=ACTION_TWO;
		else if(i==3) temp=ACTION_THREE;
		struct QTableEntry* temp_QE=findOrInsertQEntry(table,state, temp);
		if(temp_QE && temp_QE->qValue > maxqvalue){  /* 添加NULL检查 */
			maxqvalue=temp_QE->qValue;
		}
	}
	return maxqvalue;
}

//给定奖励，考虑到最冷块回收可能对学习策略的误导，使用回收块中无效页与有效页的比例作为奖励（相似性）
// int getreward(int num_gc_copies,int deleted_pages){
// 	int gc_copies=(num_gc_copies==0)? 1:num_gc_copies;
// 	int reward=num_gc_copies < deleted_pages? 10*deleted_pages/gc_copies:10*gc_copies/deleted_pages;
// 	return reward;
// }//一次垃圾回收期间页面迁移之间的reward为0
// int getreward(int num_gc_copies,int deleted_pages){
// 	int reward;
// 	if(num_gc_copies==0 || deleted_pages==0) reward=128;
// 	else 
// 		reward=num_gc_copies < deleted_pages? (10*deleted_pages/num_gc_copies)/10:(10*num_gc_copies/deleted_pages)/10;
// 	return reward;
//}//一次垃圾回收期间页面迁移之间的reward为0
int getreward(int num_gc_copies,int deleted_pages,int time){
	int reward;
	if(num_gc_copies==0 || deleted_pages==0) reward=128;
	else 
		reward=num_gc_copies < deleted_pages? (10*deleted_pages/num_gc_copies)/10:(10*num_gc_copies/deleted_pages)/10;
	
	/* 修复内核环境：避免除零 */
	if(time <= 0) time = 1;
	return reward/time;
}

int updateQValue_q(struct QTableEntry* table, struct State* laststate,
	struct State* currentstate,enum Action lastaction) {  
    struct QTableEntry* lastentry = findOrInsertQEntry(table,laststate, lastaction);  
	int best_current_qValue = getbestqvalue(table,currentstate);
    if (lastentry) {  
        /* 使用 int64_t 避免整数溢出 */
        int64_t target = ((int64_t)discountFactor * best_current_qValue) / 10;
        int64_t td_error = target - lastentry->qValue;
        int64_t update = ((int64_t)learningRate * td_error) / 10;
        lastentry->qValue += (int)update;  
    }  
	return lastentry ? lastentry->qValue : 0;
}  
//仅有reward
int updateQValue_r(struct QTableEntry* table, struct State* laststate,
	enum Action lastaction,int reward) {  
    struct QTableEntry* lastentry = findOrInsertQEntry(table,laststate, lastaction);  
    if (lastentry) {  
        /* 使用 int64_t 避免整数溢出 */
        int64_t delta = reward - lastentry->qValue;
        int64_t update = ((int64_t)learningRate * delta) / 10;
        lastentry->qValue += (int)update;  
    }  
	return lastentry ? lastentry->qValue : 0;
} 

void update_to_handle_overflow(struct QTableEntry* table,int temp_qValue,struct State* state,
	enum Action action){
	if(temp_qValue<0){
	u32 i;
	enum Action temp;
	for(i=0;i<4;i++){
		if(i==0) temp=ACTION_ZERO;
		else if(i==1) temp=ACTION_ONE;
		else if(i==2) temp=ACTION_TWO;
		else if(i==3) temp=ACTION_THREE;
		struct QTableEntry* temp_QE=findOrInsertQEntry(table,state, temp);
		if(temp_QE && temp_QE->qValue >= 0){  /* 添加NULL检查 */
			temp_QE->qValue-=2147483647;
		}
		else if(temp_QE){
			temp_QE->qValue+=2147483647;
		}
	}		
	}
	else{//达到int最大值，该state下q值均大于0，减去最小值确保相对顺序
	int minqvalue=2147483647;
	enum Action temp;
	u32 i;
	for(i=0;i<4;i++){
		if(i==0) temp=ACTION_ZERO;
		else if(i==1) temp=ACTION_ONE;
		else if(i==2) temp=ACTION_TWO;
		else if(i==3) temp=ACTION_THREE;
		struct QTableEntry* temp_QE=findOrInsertQEntry(table,state, temp);
		if(temp_QE && temp_QE->qValue < minqvalue){  /* 添加NULL检查 */
			minqvalue=temp_QE->qValue;
		}
	}
	for(i=0;i<4;i++){
		if(i==0) temp=ACTION_ZERO;
		else if(i==1) temp=ACTION_ONE;
		else if(i==2) temp=ACTION_TWO;
		else if(i==3) temp=ACTION_THREE;
		struct QTableEntry* temp_QE=findOrInsertQEntry(table,state, temp);
		if(temp_QE){  /* 添加NULL检查 */
			temp_QE->qValue-=minqvalue;
		}
	}		
	}

}


//用于快速存取状态动作对，加速更新的哈希表部分

struct HashNode* current_node = NULL;
struct HashNode* Next_node = NULL;

void insertHashTable(struct State* state, enum Action action, unsigned block) {
	struct HashNode* newNode = (struct HashNode*)kvmalloc(sizeof(struct HashNode),GFP_KERNEL);
    if (newNode == NULL) {
		printk("error in %d",__LINE__);
    }
	newNode->state = state;
    newNode->action = action;
	newNode->next=BTable[block];	//头插
	BTable[block] = newNode;
}

// 返回一个buckets中的所有元素 返回一个链表首地址 当current->next为NULL时结束
struct HashNode* searchInHashTable(unsigned block) {
    return BTable[block];

}

// 删除哈希表中对应桶中的所有元素 
void deleteFromHashTable(unsigned block) {
    current_node = BTable[block];
    Next_node = NULL;
    while (current_node != NULL) {
		Next_node=current_node->next;
		kvfree(current_node);
		current_node=Next_node;
	}
	BTable[block] = NULL;
}

// 释放哈希表内存
void freeHashBTable(void) {
	u32 i;
    for (i = 0; i < BLOCK_NUMBER; ++i) {
		current_node = BTable[i];
		Next_node = NULL;
		while (current_node != NULL) {
			Next_node=current_node->next;
			kvfree(current_node);
			current_node=Next_node;
		}
    }
}

#endif


/* Function to calculate chunk and offset */

void yaffs_addr_to_chunk(struct yaffs_dev *dev, loff_t addr,
				int *chunk_out, u32 *offset_out)
{
	int chunk;
	u32 offset;

	chunk = (u32) (addr >> dev->chunk_shift);

	if (dev->chunk_div == 1) {
		/* easy power of 2 case */
		offset = (u32) (addr & dev->chunk_mask);
	} else {
		/* Non power-of-2 case */

		loff_t chunk_base;

		chunk /= dev->chunk_div;

		chunk_base = ((loff_t) chunk) * dev->data_bytes_per_chunk;
		offset = (u32) (addr - chunk_base);
	}

	*chunk_out = chunk;
	*offset_out = offset;
}

/* Function to return the number of shifts for a power of 2 greater than or
 * equal to the given number
 * Note we don't try to cater for all possible numbers and this does not have to
 * be hellishly efficient.
 */

static inline u32 calc_shifts_ceiling(u32 x)
{
	int extra_bits;
	int shifts;

	shifts = extra_bits = 0;

	while (x > 1) {
		if (x & 1)
			extra_bits++;
		x >>= 1;
		shifts++;
	}

	if (extra_bits)
		shifts++;

	return shifts;
}

/* Function to return the number of shifts to get a 1 in bit 0
 */

static inline u32 calc_shifts(u32 x)
{
	u32 shifts;

	shifts = 0;

	if (!x)
		return 0;

	while (!(x & 1)) {
		x >>= 1;
		shifts++;
	}

	return shifts;
}

/*
 * Temporary buffer manipulations.
 */

static int yaffs_init_tmp_buffers(struct yaffs_dev *dev)
{
	int i;
	u8 *buf = (u8 *) 1;

	memset(dev->temp_buffer, 0, sizeof(dev->temp_buffer));

	for (i = 0; buf && i < YAFFS_N_TEMP_BUFFERS; i++) {
		dev->temp_buffer[i].in_use = 0;
		buf = kmalloc(dev->param.total_bytes_per_chunk, GFP_NOFS);
		dev->temp_buffer[i].buffer = buf;
	}

	return buf ? YAFFS_OK : YAFFS_FAIL;
}

u8 *yaffs_get_temp_buffer(struct yaffs_dev * dev)
{
	int i;

	dev->temp_in_use++;
	if (dev->temp_in_use > dev->max_temp)
		dev->max_temp = dev->temp_in_use;

	for (i = 0; i < YAFFS_N_TEMP_BUFFERS; i++) {
		if (dev->temp_buffer[i].in_use == 0) {
			dev->temp_buffer[i].in_use = 1;
			return dev->temp_buffer[i].buffer;
		}
	}

	yaffs_trace(YAFFS_TRACE_BUFFERS, "Out of temp buffers");
	/*
	 * If we got here then we have to allocate an unmanaged one
	 * This is not good.
	 */

	dev->unmanaged_buffer_allocs++;
	return kmalloc(dev->data_bytes_per_chunk, GFP_NOFS);

}

/* Frees all the temp_buffer objects in the yaffs_dev instance
*/
void yaffs_release_temp_buffer(struct yaffs_dev *dev, u8 *buffer)
{
	int i;

	dev->temp_in_use--;

	for (i = 0; i < YAFFS_N_TEMP_BUFFERS; i++) {
		if (dev->temp_buffer[i].buffer == buffer) {
			dev->temp_buffer[i].in_use = 0;
			return;
		}
	}

	if (buffer) {
		/* assume it is an unmanaged one. */
		yaffs_trace(YAFFS_TRACE_BUFFERS,
			"Releasing unmanaged temp buffer");
		kfree(buffer);
		dev->unmanaged_buffer_deallocs++;
	}

}

/*
 * Functions for robustisizing TODO
 *
 */

static void yaffs_handle_chunk_wr_ok(struct yaffs_dev *dev, int nand_chunk,
				     const u8 *data,
				     const struct yaffs_ext_tags *tags)
{
	(void) dev;
	(void) nand_chunk;
	(void) data;
	(void) tags;
}

static void yaffs_handle_chunk_update(struct yaffs_dev *dev, int nand_chunk,
				      const struct yaffs_ext_tags *tags)
{
	(void) dev;
	(void) nand_chunk;
	(void) tags;
}

void yaffs_handle_chunk_error(struct yaffs_dev *dev,
			      struct yaffs_block_info *bi)
{
	if (!bi->gc_prioritise) {
		bi->gc_prioritise = 1;
		dev->has_pending_prioritised_gc = 1;
		bi->chunk_error_strikes++;

		if (bi->chunk_error_strikes > 3) {
			bi->needs_retiring = 1;	/* Too many stikes, so retire */
			yaffs_trace(YAFFS_TRACE_ALWAYS,
				"yaffs: Block struck out");

		}
	}
}

static void yaffs_handle_chunk_wr_error(struct yaffs_dev *dev, int nand_chunk,
					int erased_ok)
{
	//printk("\n test fun %s is still in use \n",__func__);
	int flash_block = nand_chunk / dev->param.chunks_per_block;
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, flash_block);

	yaffs_handle_chunk_error(dev, bi);

	if (erased_ok) {
		/* Was an actual write failure,
		 * so mark the block for retirement.*/
		bi->needs_retiring = 1;
		yaffs_trace(YAFFS_TRACE_ERROR | YAFFS_TRACE_BAD_BLOCKS,
		  "**>> Block %d needs retiring", flash_block);
	}

	/* Delete the chunk */
	yaffs_chunk_del(dev, nand_chunk, 1, __LINE__);
	yaffs_skip_rest_of_block(dev);
}

/*
 * Verification code
 */

/*
 *  Simple hash function. Needs to have a reasonable spread
 */

static inline int yaffs_hash_fn(int n)
{
	if (n < 0)
		n = -n;
	return n % YAFFS_NOBJECT_BUCKETS;
}

/*
 * Access functions to useful fake objects.
 * Note that root might have a presence in NAND if permissions are set.
 */

struct yaffs_obj *yaffs_root(struct yaffs_dev *dev)
{
	return dev->root_dir;
}

struct yaffs_obj *yaffs_lost_n_found(struct yaffs_dev *dev)
{
	return dev->lost_n_found;
}

/*
 *  Erased NAND checking functions
 */

int yaffs_check_ff(u8 *buffer, int n_bytes)
{
	/* Horrible, slow implementation */
	while (n_bytes--) {
		if (*buffer != 0xff)
			return 0;
		buffer++;
	}
	return 1;
}

static int yaffs_check_chunk_erased(struct yaffs_dev *dev, int nand_chunk)
{
	int retval = YAFFS_OK;
	u8 *data = yaffs_get_temp_buffer(dev);
	struct yaffs_ext_tags tags;
	int result;

	result = yaffs_rd_chunk_tags_nand(dev, nand_chunk, data, &tags);

	if (result == YAFFS_FAIL ||
	    tags.ecc_result > YAFFS_ECC_RESULT_NO_ERROR)
		retval = YAFFS_FAIL;

	if (!yaffs_check_ff(data, dev->data_bytes_per_chunk) ||
		tags.chunk_used) {
		yaffs_trace(YAFFS_TRACE_NANDACCESS,
			"Chunk %d not erased", nand_chunk);
		retval = YAFFS_FAIL;
	}

	yaffs_release_temp_buffer(dev, data);

	return retval;

}

static int yaffs_verify_chunk_written(struct yaffs_dev *dev,
				      int nand_chunk,
				      const u8 *data,
				      struct yaffs_ext_tags *tags)
{
	int retval = YAFFS_OK;
	struct yaffs_ext_tags temp_tags;
	u8 *buffer = yaffs_get_temp_buffer(dev);
	int result;

	result = yaffs_rd_chunk_tags_nand(dev, nand_chunk, buffer, &temp_tags);
	if (result == YAFFS_FAIL ||
	    memcmp(buffer, data, dev->data_bytes_per_chunk) ||
	    temp_tags.obj_id != tags->obj_id ||
	    temp_tags.chunk_id != tags->chunk_id ||
	    temp_tags.n_bytes != tags->n_bytes)
		retval = YAFFS_FAIL;

	yaffs_release_temp_buffer(dev, buffer);

	return retval;
}


int yaffs_check_alloc_available(struct yaffs_dev *dev, int n_chunks)
{
	int reserved_chunks;
	int reserved_blocks = dev->param.n_reserved_blocks;
	int checkpt_blocks;

	checkpt_blocks = yaffs_calc_checkpt_blocks_required(dev);

	reserved_chunks =
	    (reserved_blocks + checkpt_blocks) * dev->param.chunks_per_block;

	return (dev->n_free_chunks > (reserved_chunks + n_chunks));
}

#if defined(GC_Qlearning)

static int yaffs_find_alloc_block(struct yaffs_dev *dev, int hot)
{
	int i;
	struct yaffs_block_info *bi;
	int selected=0;
	extern int n_blk_erasures[BLOCK_NUMBER];

	if (dev->n_erased_blocks < 1) 
	{
		/* Hoosterman we've got a problem.
		 * Can't get space to gc
		 */
		yaffs_trace(YAFFS_TRACE_ERROR,
		  "yaffs tragedy: no more erased blocks");

		return -1;
	}

	/* Find an empty block. */
	if(1 == hot)	/* Hot data, write to  block with min erasure count. */
	{
		int erasures=0;
		dev->alloc_block_finder = dev->internal_start_block;
		for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
			dev->alloc_block_finder++;
			if (dev->alloc_block_finder < dev->internal_start_block
			    || dev->alloc_block_finder > dev->internal_end_block) {
				dev->alloc_block_finder = dev->internal_start_block;
			}

			bi = yaffs_get_block_info(dev, dev->alloc_block_finder);

			if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY && (selected<1 || n_blk_erasures[dev->alloc_block_finder-1]<erasures)) { 
				selected = dev->alloc_block_finder;
				erasures = n_blk_erasures[dev->alloc_block_finder-1];
			}
		}
		
		if(selected>0)
		{
				dev->alloc_block_finder = selected;
				bi = yaffs_get_block_info(dev, dev->alloc_block_finder);

				bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
				dev->seq_number++;
				bi->seq_number = dev->seq_number;
				dev->n_erased_blocks--;
				yaffs_trace(YAFFS_TRACE_ALLOCATE,
				  "Allocated block %d, seq  %d, %d left" ,
				   dev->alloc_block_finder, dev->seq_number,
				   dev->n_erased_blocks);
				time_alloc_block[dev->alloc_block_finder - 1] = TIME_NOW;  //add by HuangYong
				return dev->alloc_block_finder;
		}
	}
	else if(2 == hot)	//any emtpy is ok for this kind of data 
	{
	    int erasures=0;
		dev->alloc_block_finder2= dev->internal_start_block;
		for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
			dev->alloc_block_finder2++;
			if (dev->alloc_block_finder2< dev->internal_start_block
			    || dev->alloc_block_finder2> dev->internal_end_block) {
				dev->alloc_block_finder2= dev->internal_start_block;
			}

			bi = yaffs_get_block_info(dev, dev->alloc_block_finder2);

			if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY && (selected<1 || n_blk_erasures[dev->alloc_block_finder2-1]<erasures)) { 
				selected = dev->alloc_block_finder2;
				erasures = n_blk_erasures[dev->alloc_block_finder2-1];
			}
		}
		
		if(selected>0)
		{
				dev->alloc_block_finder2= selected;
				bi = yaffs_get_block_info(dev, dev->alloc_block_finder2);

				bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
				dev->seq_number++;
				bi->seq_number = dev->seq_number;
				dev->n_erased_blocks--;
				yaffs_trace(YAFFS_TRACE_ALLOCATE,
				  "Allocated block %d, seq  %d, %d left" ,
				   dev->alloc_block_finder2, dev->seq_number,
				   dev->n_erased_blocks);
				time_alloc_block[dev->alloc_block_finder2 - 1] = TIME_NOW;  //add by HuangYong
				return dev->alloc_block_finder2;
		}	
	}
	else if(3 == hot)	/* Cold data, write to  block with max erasure count. */
	{
	    	  //  dev->alloc_block_finder3= dev->internal_start_block;
	    int erasures=0;
		for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
			dev->alloc_block_finder3++;
			if (dev->alloc_block_finder3< dev->internal_start_block
			    || dev->alloc_block_finder3> dev->internal_end_block) {
				dev->alloc_block_finder3= dev->internal_start_block;
			}

			bi = yaffs_get_block_info(dev, dev->alloc_block_finder3);

			if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY && (selected<1 || n_blk_erasures[dev->alloc_block_finder3-1]>erasures)) { 
				selected = dev->alloc_block_finder3;
				erasures = n_blk_erasures[dev->alloc_block_finder3-1];
			}
		}
		
		if(selected>0)
		{
				dev->alloc_block_finder3= selected;
				bi = yaffs_get_block_info(dev, dev->alloc_block_finder3);

				bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
				dev->seq_number++;
				bi->seq_number = dev->seq_number;
				dev->n_erased_blocks--;
				yaffs_trace(YAFFS_TRACE_ALLOCATE,
				  "Allocated block %d, seq  %d, %d left" ,
				   dev->alloc_block_finder3, dev->seq_number,
				   dev->n_erased_blocks);
				time_alloc_block[dev->alloc_block_finder3 - 1] = TIME_NOW;  //add by HuangYong
				return dev->alloc_block_finder3;
		}
		else
		{
           printk("something wrong with alloc block HY");
		}
	}
	else		/* Cold data, write to  block with max erasure count. */
	{
		// dev->alloc_block_finder4= dev->internal_start_block;
		int erasures=0;
		for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
			dev->alloc_block_finder4++;
			if (dev->alloc_block_finder4< dev->internal_start_block
			    || dev->alloc_block_finder4> dev->internal_end_block) {
				dev->alloc_block_finder4= dev->internal_start_block;
			}

			bi = yaffs_get_block_info(dev, dev->alloc_block_finder4);
			
			if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY && (selected<1 || n_blk_erasures[dev->alloc_block_finder4-1]>erasures)) { 
				selected = dev->alloc_block_finder4;
				erasures = n_blk_erasures[dev->alloc_block_finder4-1];
			}
		}
		
		if(selected>0)
		{
				dev->alloc_block_finder4= selected;
				bi = yaffs_get_block_info(dev, dev->alloc_block_finder4);

				bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
				dev->seq_number++;
				bi->seq_number = dev->seq_number;
				dev->n_erased_blocks--;
				yaffs_trace(YAFFS_TRACE_ALLOCATE,
				  "Allocated block %d, seq  %d, %d left" ,
				   dev->alloc_block_finder4, dev->seq_number,
				   dev->n_erased_blocks);
				time_alloc_block[dev->alloc_block_finder4 - 1] = TIME_NOW;  //add by HuangYong
				return dev->alloc_block_finder4;
		}	
	}
	yaffs_trace(YAFFS_TRACE_ALWAYS,
		"yaffs tragedy: no more erased blocks, but there should have been %d",
		dev->n_erased_blocks);

	return -1;
}

#elif defined(GC_COPYTO_LEAST_ERASURE) 	/* Modified by ed. 2014.04.16 */

static int yaffs_find_alloc_block(struct yaffs_dev *dev)
{
	int i;
	struct yaffs_block_info *bi;
	int selected=0, erasures=0;
	extern int n_blk_erasures[BLOCK_NUMBER];

	if (dev->n_erased_blocks < 1) {
		/* Hoosterman we've got a problem.
		 * Can't get space to gc
		 */
		yaffs_trace(YAFFS_TRACE_ERROR,
		  "yaffs tragedy: no more erased blocks");

		return -1;
	}

	/* Find an empty block. */

	for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		dev->alloc_block_finder++;
		if (dev->alloc_block_finder < dev->internal_start_block
		    || dev->alloc_block_finder > dev->internal_end_block) {
			dev->alloc_block_finder = dev->internal_start_block;
		}

		bi = yaffs_get_block_info(dev, dev->alloc_block_finder);

		if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY && (selected<1 || n_blk_erasures[dev->alloc_block_finder-1]<erasures)) { 
			selected = dev->alloc_block_finder;
			erasures = n_blk_erasures[dev->alloc_block_finder-1];
		}
	}
	
	if(selected>0)
	{
			dev->alloc_block_finder = selected;
			bi = yaffs_get_block_info(dev, dev->alloc_block_finder);

			bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
			dev->seq_number++;
			bi->seq_number = dev->seq_number;
			dev->n_erased_blocks--;
			yaffs_trace(YAFFS_TRACE_ALLOCATE,
			  "Allocated block %d, seq  %d, %d left" ,
			   dev->alloc_block_finder, dev->seq_number,
			   dev->n_erased_blocks);
			return dev->alloc_block_finder;
	}

	yaffs_trace(YAFFS_TRACE_ALWAYS,
		"yaffs tragedy: no more erased blocks, but there should have been %d",  
		dev->n_erased_blocks);

	return -1;
}

#else /* Original version. 2024.04.14 */

static int yaffs_find_alloc_block(struct yaffs_dev *dev)
{
	u32 i;
	struct yaffs_block_info *bi;

	if (dev->n_erased_blocks < 1) {
		/* Hoosterman we've got a problem.
		 * Can't get space to gc
		 */
		yaffs_trace(YAFFS_TRACE_ERROR,
		  "yaffs tragedy: no more erased blocks");

		return -1;
	}

	/* Find an empty block. */

	for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		dev->alloc_block_finder++;
		if (dev->alloc_block_finder < (int)dev->internal_start_block
		    || dev->alloc_block_finder > (int)dev->internal_end_block) {
			dev->alloc_block_finder = dev->internal_start_block;
		}

		bi = yaffs_get_block_info(dev, dev->alloc_block_finder);

		if (bi->block_state == YAFFS_BLOCK_STATE_EMPTY) {
			bi->block_state = YAFFS_BLOCK_STATE_ALLOCATING;
			dev->seq_number++;
			bi->seq_number = dev->seq_number;
			dev->n_erased_blocks--;
			yaffs_trace(YAFFS_TRACE_ALLOCATE,
			  "Allocated block %d, seq  %d, %d left" ,
			   dev->alloc_block_finder, dev->seq_number,
			   dev->n_erased_blocks);
			return dev->alloc_block_finder;
		}
	}

	yaffs_trace(YAFFS_TRACE_ALWAYS,
		"yaffs tragedy: no more erased blocks, but there should have been %d",
		dev->n_erased_blocks);

	return -1;
}
#endif


#if defined(GC_Qlearning )

static int yaffs_alloc_chunk(struct yaffs_dev *dev, int use_reserver,
			     struct yaffs_block_info **block_ptr, int hot)
{
	int ret_val;
	struct yaffs_block_info *bi;
    chunk_seq++;             //add by HuangYong
	if( 1 == hot)  // the smallest number, the hottest the data is -- HY
	{
		if (dev->alloc_block < 0) {
			/* Get next block to allocate off */
			dev->alloc_block = yaffs_find_alloc_block(dev,hot);
			dev->alloc_page = 0;
		}

		if (!use_reserver && !yaffs_check_alloc_available(dev, 1)) {
			/* No space unless we're allowed to use the reserve. */
			return -1;
		}

		if (dev->n_erased_blocks < dev->param.n_reserved_blocks
		    && dev->alloc_page == 0)
			yaffs_trace(YAFFS_TRACE_ALLOCATE, "Allocating reserve");

		/* Next page please.... */
		if (dev->alloc_block >= 0) {
			bi = yaffs_get_block_info(dev, dev->alloc_block);

			ret_val = (dev->alloc_block * dev->param.chunks_per_block) +
			    dev->alloc_page;
			bi->pages_in_use++;
			yaffs_set_chunk_bit(dev, dev->alloc_block, dev->alloc_page);

			dev->alloc_page++;

			dev->n_free_chunks--;

			/* If the block is full set the state to full */
			if (dev->alloc_page >= dev->param.chunks_per_block) {
				bi->block_state = YAFFS_BLOCK_STATE_FULL;
				dev->alloc_block = -1;
			}

			if (block_ptr)
				*block_ptr = bi;

			return ret_val;
		}
	}
	else if( 2 == hot )  //now use block3
	{
		if (dev->alloc_block2 < 0) {
			/* Get next block to allocate off */
			dev->alloc_block2 = yaffs_find_alloc_block(dev,hot);
			dev->alloc_page2 = 0;
		}

		if (!use_reserver && !yaffs_check_alloc_available(dev, 1)) {
			/* No space unless we're allowed to use the reserve. */
			return -1;
		}

		if (dev->n_erased_blocks < dev->param.n_reserved_blocks
		    && dev->alloc_page2 == 0)
			yaffs_trace(YAFFS_TRACE_ALLOCATE, "Allocating reserve");

		/* Next page please.... */
		if (dev->alloc_block2 >= 0) {
			bi = yaffs_get_block_info(dev, dev->alloc_block2);

			ret_val = (dev->alloc_block2 * dev->param.chunks_per_block) +
			    dev->alloc_page2;
			bi->pages_in_use++;
			yaffs_set_chunk_bit(dev, dev->alloc_block2, dev->alloc_page2);

			dev->alloc_page2++;

			dev->n_free_chunks--;

			/* If the block is full set the state to full */
			if (dev->alloc_page2 >= dev->param.chunks_per_block) {
				bi->block_state = YAFFS_BLOCK_STATE_FULL;
				dev->alloc_block2 = -1;
			}

			if (block_ptr)
				*block_ptr = bi;

			return ret_val;
		}	
	}
	else  if( 3 == hot )
	{
				if (dev->alloc_block3 < 0) {
			/* Get next block to allocate off */
			dev->alloc_block3 = yaffs_find_alloc_block(dev,hot);
			dev->alloc_page3 = 0;
		}

		if (!use_reserver && !yaffs_check_alloc_available(dev, 1)) {
			/* No space unless we're allowed to use the reserve. */
			return -1;
		}

		if (dev->n_erased_blocks < dev->param.n_reserved_blocks
		    && dev->alloc_page3 == 0)
			yaffs_trace(YAFFS_TRACE_ALLOCATE, "Allocating reserve");

		/* Next page please.... */
		if (dev->alloc_block3 >= 0) {
			bi = yaffs_get_block_info(dev, dev->alloc_block3);

			ret_val = (dev->alloc_block3 * dev->param.chunks_per_block) +
			    dev->alloc_page3;
			bi->pages_in_use++;
			yaffs_set_chunk_bit(dev, dev->alloc_block3, dev->alloc_page3);

			dev->alloc_page3++;

			dev->n_free_chunks--;

			/* If the block is full set the state to full */
			if (dev->alloc_page3 >= dev->param.chunks_per_block) {
				bi->block_state = YAFFS_BLOCK_STATE_FULL;
				dev->alloc_block3 = -1;
			}

			if (block_ptr)
				*block_ptr = bi;

			return ret_val;
		}	
	}
	else  
	{
		if (dev->alloc_block4 < 0) {
			/* Get next block to allocate off */
			dev->alloc_block4 = yaffs_find_alloc_block(dev,hot);
			dev->alloc_page4 = 0;
		}

		if (!use_reserver && !yaffs_check_alloc_available(dev, 1)) {
			/* No space unless we're allowed to use the reserve. */
			return -1;
		}

		if (dev->n_erased_blocks < dev->param.n_reserved_blocks
		    && dev->alloc_page4 == 0)
			yaffs_trace(YAFFS_TRACE_ALLOCATE, "Allocating reserve");

		/* Next page please.... */
		if (dev->alloc_block4 >= 0) {
			bi = yaffs_get_block_info(dev, dev->alloc_block4);

			ret_val = (dev->alloc_block4 * dev->param.chunks_per_block) +
			    dev->alloc_page4;
			bi->pages_in_use++;
			yaffs_set_chunk_bit(dev, dev->alloc_block4, dev->alloc_page4);

			dev->alloc_page4++;

			dev->n_free_chunks--;

			/* If the block is full set the state to full */
			if (dev->alloc_page4 >= dev->param.chunks_per_block) {
				bi->block_state = YAFFS_BLOCK_STATE_FULL;
				dev->alloc_block4 = -1;
			}

			if (block_ptr)
				*block_ptr = bi;

			return ret_val;
		}	
	}
	yaffs_trace(YAFFS_TRACE_ERROR,
		"!!!!!!!!! Allocator out !!!!!!!!!!!!!!!!!");

	return -1;
} 
#else	/* Original version. 2014.05.14 */

static int yaffs_alloc_chunk(struct yaffs_dev *dev, int use_reserver,
			     struct yaffs_block_info **block_ptr)
{
	int ret_val;
	struct yaffs_block_info *bi;

	if (dev->alloc_block < 0) {
		/* Get next block to allocate off */
		dev->alloc_block = yaffs_find_alloc_block(dev);
			//记录时间因子
		bat[dev->alloc_block].time=jiffies;

		dev->alloc_page = 0;
	}

	if (!use_reserver && !yaffs_check_alloc_available(dev, 1)) {
		/* No space unless we're allowed to use the reserve. */
		return -1;
	}

	if (dev->n_erased_blocks < (int)dev->param.n_reserved_blocks
	    && dev->alloc_page == 0)
		yaffs_trace(YAFFS_TRACE_ALLOCATE, "Allocating reserve");

	/* Next page please.... */
	if (dev->alloc_block >= 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block);

		ret_val = (dev->alloc_block * dev->param.chunks_per_block) +
		    dev->alloc_page;
		bi->pages_in_use++;
		yaffs_set_chunk_bit(dev, dev->alloc_block, dev->alloc_page);

		dev->alloc_page++;

		dev->n_free_chunks--;

		/* If the block is full set the state to full */
		if (dev->alloc_page >= dev->param.chunks_per_block) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block = -1;
		}

		if (block_ptr)
			*block_ptr = bi;

		return ret_val;
	}

	yaffs_trace(YAFFS_TRACE_ERROR,
		"!!!!!!!!! Allocator out !!!!!!!!!!!!!!!!!");

	return -1;
}
#endif
static int yaffs_get_erased_chunks(struct yaffs_dev *dev)
{
	int n;

	n = dev->n_erased_blocks * dev->param.chunks_per_block;

	if (dev->alloc_block > 0)
		n += (dev->param.chunks_per_block - dev->alloc_page);

#if defined(GC_Qlearning)
	if (dev->alloc_block2 > 0)
		n += (dev->param.chunks_per_block - dev->alloc_page2);
	if (dev->alloc_block3 > 0)
		n += (dev->param.chunks_per_block - dev->alloc_page3);
	if (dev->alloc_block4 > 0)
		n += (dev->param.chunks_per_block - dev->alloc_page4);
#endif
	return n;

}

/*
 * yaffs_skip_rest_of_block() skips over the rest of the allocation block
 * if we don't want to write to it.
 */
void yaffs_skip_rest_of_block(struct yaffs_dev *dev)
{
	//printk("\n test fun %s is still in use \n",__func__);
	struct yaffs_block_info *bi;

	if (dev->alloc_block > 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block);
		if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block = -1;
		}
	}
#if defined(GC_Qlearning)
	if (dev->alloc_block2 > 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block2);
		if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block2 = -1;
		}
	}
	if (dev->alloc_block3 > 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block3);
		if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block3 = -1;
		}
	}
	if (dev->alloc_block4 > 0) {
		bi = yaffs_get_block_info(dev, dev->alloc_block4);
		if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING) {
			bi->block_state = YAFFS_BLOCK_STATE_FULL;
			dev->alloc_block4 = -1;
		}
	}
#endif
}
static int yaffs_write_new_chunk_for_migration(struct yaffs_dev *dev,
				 const u8 *data,
				 struct yaffs_ext_tags *tags, int use_reserver,int aver_uft,int newest_once)
{
	int attempts = 0;
	int write_ok = 0;
	int chunk;

	yaffs2_checkpt_invalidate(dev);

	do {
		struct yaffs_block_info *bi = 0;
		int erased_ok = 0;


// add by ry 2024.05.10
#if defined(GC_Qlearning)
		unsigned now = jiffies;
		unsigned time = 0;
		unsigned cnt  = 0;
		unsigned block;
		unsigned index;
		struct State* current_state=NULL;
		enum Action current_action;
		int hot=0;
		get_time_hot(tags->obj_id,tags->chunk_id, &time, &cnt);
		current_state=get_usable_state(tags,aver_uft);
		if(current_state==NULL) printk("error with get_usable_state in %d",__LINE__);
		if(new_start) {//第一次垃圾回收的初始状态
			new_start=0;
		}
		else{//垃圾回收期间 迁移有效页的过程
			int cur_qvalue=updateQValue_q(QTable,last_state,current_state,last_action );//no reward
			if(cur_qvalue<0 || cur_qvalue==2147483647){
				update_to_handle_overflow(QTable,cur_qvalue,last_state,last_action);//处理溢出影响
			}
		}
		current_action=getaction(QTable,current_state);
		hot=enumtoint(current_action);
		chunk = yaffs_alloc_chunk(dev, use_reserver, &bi, hot);
		block=chunk / dev->param.chunks_per_block;
		insertHashTable(current_state, current_action,block) ;
		last_state=current_state;
		last_action=current_action;		
		set_time_hot(tags->obj_id,tags->chunk_id, now, ++cnt); 
//add end
#else	/* Original version */

		chunk = yaffs_alloc_chunk(dev, use_reserver, &bi);
#endif
		if (chunk < 0) {
			/* no space */
			printk("\n failed to alloc chunk something wrong happend when migrate valid chunk");
			break;
		}

		/* First check this chunk is erased, if it needs
		 * checking.  The checking policy (unless forced
		 * always on) is as follows:
		 *
		 * Check the first page we try to write in a block.
		 * If the check passes then we don't need to check any
		 * more.        If the check fails, we check again...
		 * If the block has been erased, we don't need to check.
		 *
		 * However, if the block has been prioritised for gc,
		 * then we think there might be something odd about
		 * this block and stop using it.
		 *
		 * Rationale: We should only ever see chunks that have
		 * not been erased if there was a partially written
		 * chunk due to power loss.  This checking policy should
		 * catch that case with very few checks and thus save a
		 * lot of checks that are most likely not needed.
		 *
		 * Mods to the above
		 * If an erase check fails or the write fails we skip the
		 * rest of the block.
		 */

		/* let's give it a try */
		attempts++;

		if (dev->param.always_check_erased)
			bi->skip_erased_check = 0;

		if (!bi->skip_erased_check) {
			erased_ok = yaffs_check_chunk_erased(dev, chunk);
			if (erased_ok != YAFFS_OK) {
				yaffs_trace(YAFFS_TRACE_ERROR,
				  "**>> yaffs chunk %d was not erased",
				  chunk);

				/* If not erased, delete this one,
				 * skip rest of block and
				 * try another chunk */
				yaffs_chunk_del(dev, chunk, 1, __LINE__);
				yaffs_skip_rest_of_block(dev);
				continue;
			}
		}

		write_ok = yaffs_wr_chunk_tags_nand(dev, chunk, data, tags);

		if (!bi->skip_erased_check)
			write_ok =
			    yaffs_verify_chunk_written(dev, chunk, data, tags);

		if (write_ok != YAFFS_OK) {
			/* Clean up aborted write, skip to next block and
			 * try another chunk */
			yaffs_handle_chunk_wr_error(dev, chunk, erased_ok);
			continue;
		}

		bi->skip_erased_check = 1;

		/* Copy the data into the robustification buffer */
		yaffs_handle_chunk_wr_ok(dev, chunk, data, tags);

	} while (write_ok != YAFFS_OK &&
		 (yaffs_wr_attempts <= 0 || attempts <= yaffs_wr_attempts));

	if (!write_ok)
		chunk = -1;

	if (attempts > 1) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"**>> yaffs write required %d attempts",
			attempts);
		dev->n_retried_writes += (attempts - 1);
	}

	return chunk;
}

static int yaffs_write_new_chunk(struct yaffs_dev *dev,
				 const u8 *data,
				 struct yaffs_ext_tags *tags, int use_reserver)
{
	u32 attempts = 0;
	int write_ok = 0;
	int chunk;

	yaffs2_checkpt_invalidate(dev);

	do {
		struct yaffs_block_info *bi = 0;
		int erased_ok = 0;

		#if defined(GC_Qlearning)
		//新写入看作次热，不一定需要
	    int hot = 2; //0 1 2 3
		#endif 
#if defined(GC_Qlearning )
/*
不同于之前的最后一次失效时间设置，不论是不是迁移时的页面写，
都将对应逻辑页的最后一次无效时间设置为now，至少我认为该如此才对，
失效时间与当前时间的间隔除以更新次数获得平均更新时间，
因此即使是第一次写入逻辑页也该设置为now（考虑更新间隔）

*/
		extern int max_objid;
		extern int max_chunkid;
		unsigned now = jiffies;
		unsigned time,cnt;    
		get_time_hot(tags->obj_id,tags->chunk_id, &time, &cnt);
        // if(0 == time)
        // {
        //     time = now;
		// }
		if(tags->obj_id>max_objid) max_objid=tags->obj_id;
		chunk = yaffs_alloc_chunk(dev, use_reserver, &bi, hot);
		if(chunk>max_chunkid) max_chunkid=chunk;
		set_time_hot(tags->obj_id,tags->chunk_id, now, ++cnt);
		
#else	/* Original version */
		chunk = yaffs_alloc_chunk(dev, use_reserver, &bi);
#endif
		if (chunk < 0) {
			/* no space */
			printk("\nfailed to alloc chunk!something wrong happend when write new chunk");
			break;
		}


		/* First check this chunk is erased, if it needs
		 * checking.  The checking policy (unless forced
		 * always on) is as follows:
		 *
		 * Check the first page we try to write in a block.
		 * If the check passes then we don't need to check any
		 * more.        If the check fails, we check again...
		 * If the block has been erased, we don't need to check.
		 *
		 * However, if the block has been prioritised for gc,
		 * then we think there might be something odd about
		 * this block and stop using it.
		 *
		 * Rationale: We should only ever see chunks that have
		 * not been erased if there was a partially written
		 * chunk due to power loss.  This checking policy should
		 * catch that case with very few checks and thus save a
		 * lot of checks that are most likely not needed.
		 *
		 * Mods to the above
		 * If an erase check fails or the write fails we skip the
		 * rest of the block.
		 */

		/* let's give it a try */
		attempts++;

		if (dev->param.always_check_erased)
			bi->skip_erased_check = 0;

		if (!bi->skip_erased_check) {
			erased_ok = yaffs_check_chunk_erased(dev, chunk);
			if (erased_ok != YAFFS_OK) {
				yaffs_trace(YAFFS_TRACE_ERROR,
				  "**>> yaffs chunk %d was not erased",
				  chunk);

				/* If not erased, delete this one,
				 * skip rest of block and
				 * try another chunk */
				 printk("\nit seems that here help initialize NAND\n");
				yaffs_chunk_del(dev, chunk, 1, __LINE__);
				yaffs_skip_rest_of_block(dev);
				continue;
			}
		}

		write_ok = yaffs_wr_chunk_tags_nand(dev, chunk, data, tags);

		if (!bi->skip_erased_check)
			write_ok =
			    yaffs_verify_chunk_written(dev, chunk, data, tags);

		if (write_ok != YAFFS_OK) {
			/* Clean up aborted write, skip to next block and
			 * try another chunk */
			yaffs_handle_chunk_wr_error(dev, chunk, erased_ok);
			continue;
		}

		bi->skip_erased_check = 1;

		/* Copy the data into the robustification buffer */
		yaffs_handle_chunk_wr_ok(dev, chunk, data, tags);

	} while (write_ok != YAFFS_OK &&
		 (yaffs_wr_attempts == 0 || attempts <= yaffs_wr_attempts));

	if (!write_ok)
		chunk = -1;

	if (attempts > 1) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"**>> yaffs write required %d attempts",
			attempts);
		dev->n_retried_writes += (attempts - 1);
	}

	return chunk;
}

/*
 * Block retiring for handling a broken block.
 */

static void yaffs_retire_block(struct yaffs_dev *dev, int flash_block)
{
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, flash_block);

	yaffs2_checkpt_invalidate(dev);

	yaffs2_clear_oldest_dirty_seq(dev, bi);

	if (yaffs_mark_bad(dev, flash_block) != YAFFS_OK) {
		if (yaffs_erase_block(dev, flash_block) != YAFFS_OK) {
			yaffs_trace(YAFFS_TRACE_ALWAYS,
				"yaffs: Failed to mark bad and erase block %d",
				flash_block);
		} else {
			struct yaffs_ext_tags tags;
			int chunk_id =
			    flash_block * dev->param.chunks_per_block;

			u8 *buffer = yaffs_get_temp_buffer(dev);

			memset(buffer, 0xff, dev->data_bytes_per_chunk);
			memset(&tags, 0, sizeof(tags));
			tags.seq_number = YAFFS_SEQUENCE_BAD_BLOCK;
			if (dev->tagger.write_chunk_tags_fn(dev, chunk_id -
							dev->chunk_offset,
							buffer,
							&tags) != YAFFS_OK)
				yaffs_trace(YAFFS_TRACE_ALWAYS,
					"yaffs: Failed to write bad block marker to block %d",
					flash_block);

			yaffs_release_temp_buffer(dev, buffer);
		}
	}

	bi->block_state = YAFFS_BLOCK_STATE_DEAD;
	bi->gc_prioritise = 0;
	bi->needs_retiring = 0;

	dev->n_retired_blocks++;
}

/*---------------- Name handling functions ------------*/

static void yaffs_load_name_from_oh(struct yaffs_dev *dev, YCHAR *name,
				    const YCHAR *oh_name, int buff_size)
{
#ifdef CONFIG_YAFFS_AUTO_UNICODE
	if (dev->param.auto_unicode) {
		if (*oh_name) {
			/* It is an ASCII name, do an ASCII to
			 * unicode conversion */
			const char *ascii_oh_name = (const char *)oh_name;
			int n = buff_size - 1;
			while (n > 0 && *ascii_oh_name) {
				*name = *ascii_oh_name;
				name++;
				ascii_oh_name++;
				n--;
			}
		} else {
			strncpy(name, oh_name + 1, buff_size - 1);
		}
	} else {
#else
	(void) dev;
	{
#endif
		strncpy(name, oh_name, buff_size - 1);
	}
}

static void yaffs_load_oh_from_name(struct yaffs_dev *dev, YCHAR *oh_name,
				    const YCHAR *name)
{
#ifdef CONFIG_YAFFS_AUTO_UNICODE

	int is_ascii;
	const YCHAR *w;

	if (dev->param.auto_unicode) {

		is_ascii = 1;
		w = name;

		/* Figure out if the name will fit in ascii character set */
		while (is_ascii && *w) {
			if ((*w) & 0xff00)
				is_ascii = 0;
			w++;
		}

		if (is_ascii) {
			/* It is an ASCII name, so convert unicode to ascii */
			char *ascii_oh_name = (char *)oh_name;
			int n = YAFFS_MAX_NAME_LENGTH - 1;
			while (n > 0 && *name) {
				*ascii_oh_name = *name;
				name++;
				ascii_oh_name++;
				n--;
			}
		} else {
			/* Unicode name, so save starting at the second YCHAR */
			*oh_name = 0;
			strncpy(oh_name + 1, name, YAFFS_MAX_NAME_LENGTH - 2);
		}
	} else {
#else
	dev = dev;
    {
#endif
		strncpy(oh_name, name, YAFFS_MAX_NAME_LENGTH - 1);
	}
}

static u16 yaffs_calc_name_sum(const YCHAR *name)
{
	u16 sum = 0;
	u16 i = 1;

	if (!name)
		return 0;

	while ((*name) && i < (YAFFS_MAX_NAME_LENGTH / 2)) {

		/* 0x1f mask is case insensitive */
		sum += ((*name) & 0x1f) * i;
		i++;
		name++;
	}
	return sum;
}


void yaffs_set_obj_name(struct yaffs_obj *obj, const YCHAR * name)
{
	memset(obj->short_name, 0, sizeof(obj->short_name));

	if (name && !name[0]) {
		yaffs_fix_null_name(obj, obj->short_name,
				YAFFS_SHORT_NAME_LENGTH);
		name = obj->short_name;
	} else if (name &&
		strnlen(name, YAFFS_SHORT_NAME_LENGTH + 1) <=
		YAFFS_SHORT_NAME_LENGTH)  {
		strcpy(obj->short_name, name);
	}

	obj->sum = yaffs_calc_name_sum(name);
}

void yaffs_set_obj_name_from_oh(struct yaffs_obj *obj,
				const struct yaffs_obj_hdr *oh)
{
#ifdef CONFIG_YAFFS_AUTO_UNICODE
	YCHAR tmp_name[YAFFS_MAX_NAME_LENGTH + 1];
	memset(tmp_name, 0, sizeof(tmp_name));
	yaffs_load_name_from_oh(obj->my_dev, tmp_name, oh->name,
				YAFFS_MAX_NAME_LENGTH + 1);
	yaffs_set_obj_name(obj, tmp_name);
#else
	yaffs_set_obj_name(obj, oh->name);
#endif
}

loff_t yaffs_max_file_size(struct yaffs_dev *dev)
{
	if (sizeof(loff_t) < 8)
		return YAFFS_MAX_FILE_SIZE_32;
	else
		return ((loff_t) YAFFS_MAX_CHUNK_ID) * dev->data_bytes_per_chunk;
}

/*-------------------- TNODES -------------------

 * List of spare tnodes
 * The list is hooked together using the first pointer
 * in the tnode.
 */

struct yaffs_tnode *yaffs_get_tnode(struct yaffs_dev *dev)
{
	struct yaffs_tnode *tn = yaffs_alloc_raw_tnode(dev);

	if (tn) {
		memset(tn, 0, dev->tnode_size);
		dev->n_tnodes++;
	}

	dev->checkpoint_blocks_required = 0;	/* force recalculation */

	return tn;
}

/* FreeTnode frees up a tnode and puts it back on the free list */
static void yaffs_free_tnode(struct yaffs_dev *dev, struct yaffs_tnode *tn)
{
	yaffs_free_raw_tnode(dev, tn);
	dev->n_tnodes--;
	dev->checkpoint_blocks_required = 0;	/* force recalculation */
}

static void yaffs_deinit_tnodes_and_objs(struct yaffs_dev *dev)
{
	yaffs_deinit_raw_tnodes_and_objs(dev);
	dev->n_obj = 0;
	dev->n_tnodes = 0;
}

static void yaffs_load_tnode_0(struct yaffs_dev *dev, struct yaffs_tnode *tn,
			unsigned pos, unsigned val)
{
	u32 *map = (u32 *) tn;
	u32 bit_in_map;
	u32 bit_in_word;
	u32 word_in_map;
	u32 mask;

	pos &= YAFFS_TNODES_LEVEL0_MASK;
	val >>= dev->chunk_grp_bits;

	bit_in_map = pos * dev->tnode_width;
	word_in_map = bit_in_map / 32;
	bit_in_word = bit_in_map & (32 - 1);

	mask = dev->tnode_mask << bit_in_word;

	map[word_in_map] &= ~mask;
	map[word_in_map] |= (mask & (val << bit_in_word));

	if (dev->tnode_width > (32 - bit_in_word)) {
		bit_in_word = (32 - bit_in_word);
		word_in_map++;
		mask =
		    dev->tnode_mask >> bit_in_word;
		map[word_in_map] &= ~mask;
		map[word_in_map] |= (mask & (val >> bit_in_word));
	}
}

u32 yaffs_get_group_base(struct yaffs_dev *dev, struct yaffs_tnode *tn,
			 unsigned pos)
{
	u32 *map = (u32 *) tn;
	u32 bit_in_map;
	u32 bit_in_word;
	u32 word_in_map;
	u32 val;

	pos &= YAFFS_TNODES_LEVEL0_MASK;

	bit_in_map = pos * dev->tnode_width;
	word_in_map = bit_in_map / 32;
	bit_in_word = bit_in_map & (32 - 1);

	val = map[word_in_map] >> bit_in_word;

	if (dev->tnode_width > (32 - bit_in_word)) {
		bit_in_word = (32 - bit_in_word);
		word_in_map++;
		val |= (map[word_in_map] << bit_in_word);
	}

	val &= dev->tnode_mask;
	val <<= dev->chunk_grp_bits;

	return val;
}

/* ------------------- End of individual tnode manipulation -----------------*/

/* ---------Functions to manipulate the look-up tree (made up of tnodes) ------
 * The look up tree is represented by the top tnode and the number of top_level
 * in the tree. 0 means only the level 0 tnode is in the tree.
 */

/* FindLevel0Tnode finds the level 0 tnode, if one exists. */
struct yaffs_tnode *yaffs_find_tnode_0(struct yaffs_dev *dev,
				       struct yaffs_file_var *file_struct,
				       u32 chunk_id)
{
	struct yaffs_tnode *tn = file_struct->top;
	u32 i;
	int required_depth;
	int level = file_struct->top_level;

	(void) dev;

	/* Check sane level and chunk Id */
	if (level < 0 || level > YAFFS_TNODES_MAX_LEVEL)
		return NULL;

	if (chunk_id > YAFFS_MAX_CHUNK_ID)
		return NULL;

	/* First check we're tall enough (ie enough top_level) */

	i = chunk_id >> YAFFS_TNODES_LEVEL0_BITS;
	required_depth = 0;
	while (i) {
		i >>= YAFFS_TNODES_INTERNAL_BITS;
		required_depth++;
	}

	if (required_depth > file_struct->top_level)
		return NULL;	/* Not tall enough, so we can't find it */

	/* Traverse down to level 0 */
	while (level > 0 && tn) {
		tn = tn->internal[(chunk_id >>
				   (YAFFS_TNODES_LEVEL0_BITS +
				    (level - 1) *
				    YAFFS_TNODES_INTERNAL_BITS)) &
				  YAFFS_TNODES_INTERNAL_MASK];
		level--;
	}

	return tn;
}

/* add_find_tnode_0 finds the level 0 tnode if it exists,
 * otherwise first expands the tree.
 * This happens in two steps:
 *  1. If the tree isn't tall enough, then make it taller.
 *  2. Scan down the tree towards the level 0 tnode adding tnodes if required.
 *
 * Used when modifying the tree.
 *
 *  If the tn argument is NULL, then a fresh tnode will be added otherwise the
 *  specified tn will be plugged into the ttree.
 */

struct yaffs_tnode *yaffs_add_find_tnode_0(struct yaffs_dev *dev,
					   struct yaffs_file_var *file_struct,
					   u32 chunk_id,
					   struct yaffs_tnode *passed_tn)
{
	int required_depth;
	int i;
	int l;
	struct yaffs_tnode *tn;
	u32 x;

	/* Check sane level and page Id */
	if (file_struct->top_level < 0 ||
	    file_struct->top_level > YAFFS_TNODES_MAX_LEVEL)
		return NULL;

	if (chunk_id > YAFFS_MAX_CHUNK_ID)
		return NULL;

	/* First check we're tall enough (ie enough top_level) */

	x = chunk_id >> YAFFS_TNODES_LEVEL0_BITS;
	required_depth = 0;
	while (x) {
		x >>= YAFFS_TNODES_INTERNAL_BITS;
		required_depth++;
	}

	if (required_depth > file_struct->top_level) {
		/* Not tall enough, gotta make the tree taller */
		for (i = file_struct->top_level; i < required_depth; i++) {

			tn = yaffs_get_tnode(dev);

			if (tn) {
				tn->internal[0] = file_struct->top;
				file_struct->top = tn;
				file_struct->top_level++;
			} else {
				yaffs_trace(YAFFS_TRACE_ERROR,
					"yaffs: no more tnodes");
				return NULL;
			}
		}
	}

	/* Traverse down to level 0, adding anything we need */

	l = file_struct->top_level;
	tn = file_struct->top;

	if (l > 0) {
		while (l > 0 && tn) {
			x = (chunk_id >>
			     (YAFFS_TNODES_LEVEL0_BITS +
			      (l - 1) * YAFFS_TNODES_INTERNAL_BITS)) &
			    YAFFS_TNODES_INTERNAL_MASK;

			if ((l > 1) && !tn->internal[x]) {
				/* Add missing non-level-zero tnode */
				tn->internal[x] = yaffs_get_tnode(dev);
				if (!tn->internal[x])
					return NULL;
			} else if (l == 1) {
				/* Looking from level 1 at level 0 */
				if (passed_tn) {
					/* If we already have one, release it */
					if (tn->internal[x])
						yaffs_free_tnode(dev,
							tn->internal[x]);
					tn->internal[x] = passed_tn;

				} else if (!tn->internal[x]) {
					/* Don't have one, none passed in */
					tn->internal[x] = yaffs_get_tnode(dev);
					if (!tn->internal[x])
						return NULL;
				}
			}

			tn = tn->internal[x];
			l--;
		}
	} else {
		/* top is level 0 */
		if (passed_tn) {
			memcpy(tn, passed_tn,
			       (dev->tnode_width * YAFFS_NTNODES_LEVEL0) / 8);
			yaffs_free_tnode(dev, passed_tn);
		}
	}

	return tn;
}

static int yaffs_tags_match(const struct yaffs_ext_tags *tags, int obj_id,
			    int chunk_obj)
{
	return (tags->chunk_id == (u32)chunk_obj &&
		tags->obj_id == (u32)obj_id &&
		!tags->is_deleted) ? 1 : 0;

}

static int yaffs_find_chunk_in_group(struct yaffs_dev *dev, int the_chunk,
					struct yaffs_ext_tags *tags, int obj_id,
					int inode_chunk)
{
	int j;

	for (j = 0; the_chunk && j < dev->chunk_grp_size; j++) {
		if (yaffs_check_chunk_bit
		    (dev, the_chunk / dev->param.chunks_per_block,
		     the_chunk % dev->param.chunks_per_block)) {

			if (dev->chunk_grp_size == 1)
				return the_chunk;
			else {
				yaffs_rd_chunk_tags_nand(dev, the_chunk, NULL,
							 tags);
				if (yaffs_tags_match(tags,
							obj_id, inode_chunk)) {
					/* found it; */
					return the_chunk;
				}
			}
		}
		the_chunk++;
	}
	return -1;
}

int yaffs_find_chunk_in_file(struct yaffs_obj *in, int inode_chunk,
				    struct yaffs_ext_tags *tags)
{
	/*Get the Tnode, then get the level 0 offset chunk offset */
	struct yaffs_tnode *tn;
	int the_chunk = -1;
	struct yaffs_ext_tags local_tags;
	int ret_val = -1;
	struct yaffs_dev *dev = in->my_dev;

	if (!tags) {
		/* Passed a NULL, so use our own tags space */
		tags = &local_tags;
	}

	tn = yaffs_find_tnode_0(dev, &in->variant.file_variant, inode_chunk);

	if (!tn)
		return ret_val;

	the_chunk = yaffs_get_group_base(dev, tn, inode_chunk);

	ret_val = yaffs_find_chunk_in_group(dev, the_chunk, tags, in->obj_id,
					      inode_chunk);
	return ret_val;
}

static int yaffs_find_del_file_chunk(struct yaffs_obj *in, int inode_chunk,
				     struct yaffs_ext_tags *tags)
{
	/* Get the Tnode, then get the level 0 offset chunk offset */
	struct yaffs_tnode *tn;
	int the_chunk = -1;
	struct yaffs_ext_tags local_tags;
	struct yaffs_dev *dev = in->my_dev;
	int ret_val = -1;

	if (!tags) {
		/* Passed a NULL, so use our own tags space */
		tags = &local_tags;
	}

	tn = yaffs_find_tnode_0(dev, &in->variant.file_variant, inode_chunk);

	if (!tn)
		return ret_val;

	the_chunk = yaffs_get_group_base(dev, tn, inode_chunk);

	ret_val = yaffs_find_chunk_in_group(dev, the_chunk, tags, in->obj_id,
					      inode_chunk);

	/* Delete the entry in the filestructure (if found) */
	if (ret_val != -1)
		yaffs_load_tnode_0(dev, tn, inode_chunk, 0);

	return ret_val;
}

int yaffs_put_chunk_in_file(struct yaffs_obj *in, int inode_chunk,
			    int nand_chunk, int in_scan)
{
	/* NB in_scan is zero unless scanning.
	 * For forward scanning, in_scan is > 0;
	 * for backward scanning in_scan is < 0
	 *
	 * nand_chunk = 0 is a dummy insert to make sure the tnodes are there.
	 */

	struct yaffs_tnode *tn;
	struct yaffs_dev *dev = in->my_dev;
	int existing_cunk;
	struct yaffs_ext_tags existing_tags;
	struct yaffs_ext_tags new_tags;
	unsigned existing_serial, new_serial;

	if (in->variant_type != YAFFS_OBJECT_TYPE_FILE) {
		/* Just ignore an attempt at putting a chunk into a non-file
		 * during scanning.
		 * If it is not during Scanning then something went wrong!
		 */
		if (!in_scan) {
			yaffs_trace(YAFFS_TRACE_ERROR,
				"yaffs tragedy:attempt to put data chunk into a non-file"
				);
			BUG();
		}

		yaffs_chunk_del(dev, nand_chunk, 1, __LINE__);
		return YAFFS_OK;
	}

	tn = yaffs_add_find_tnode_0(dev,
				    &in->variant.file_variant,
				    inode_chunk, NULL);
	if (!tn)
		return YAFFS_FAIL;

	if (!nand_chunk)
		/* Dummy insert, bail now */
		return YAFFS_OK;

	existing_cunk = yaffs_get_group_base(dev, tn, inode_chunk);

	if (in_scan != 0) {
		/* If we're scanning then we need to test for duplicates
		 * NB This does not need to be efficient since it should only
		 * happen when the power fails during a write, then only one
		 * chunk should ever be affected.
		 *
		 * Correction for YAFFS2: This could happen quite a lot and we
		 * need to think about efficiency! TODO
		 * Update: For backward scanning we don't need to re-read tags
		 * so this is quite cheap.
		 */

		if (existing_cunk > 0) {
			/* NB Right now existing chunk will not be real
			 * chunk_id if the chunk group size > 1
			 * thus we have to do a FindChunkInFile to get the
			 * real chunk id.
			 *
			 * We have a duplicate now we need to decide which
			 * one to use:
			 *
			 * Backwards scanning YAFFS2: The old one is what
			 * we use, dump the new one.
			 * YAFFS1: Get both sets of tags and compare serial
			 * numbers.
			 */

			if (in_scan > 0) {
				/* Only do this for forward scanning */
				yaffs_rd_chunk_tags_nand(dev,
							 nand_chunk,
							 NULL, &new_tags);

				/* Do a proper find */
				existing_cunk =
				    yaffs_find_chunk_in_file(in, inode_chunk,
							     &existing_tags);
			}

			if (existing_cunk <= 0) {
				/*Hoosterman - how did this happen? */

				yaffs_trace(YAFFS_TRACE_ERROR,
					"yaffs tragedy: existing chunk < 0 in scan"
					);

			}

			/* NB The deleted flags should be false, otherwise
			 * the chunks will not be loaded during a scan
			 */

			if (in_scan > 0) {
				new_serial = new_tags.serial_number;
				existing_serial = existing_tags.serial_number;
			}

			if ((in_scan > 0) &&
			    (existing_cunk <= 0 ||
			     ((existing_serial + 1) & 3) == new_serial)) {
				/* Forward scanning.
				 * Use new
				 * Delete the old one and drop through to
				 * update the tnode
				 */
				yaffs_chunk_del(dev, existing_cunk, 1,
						__LINE__);
			} else {
				/* Backward scanning or we want to use the
				 * existing one
				 * Delete the new one and return early so that
				 * the tnode isn't changed
				 */
				yaffs_chunk_del(dev, nand_chunk, 1, __LINE__);
				return YAFFS_OK;
			}
		}

	}

	if (existing_cunk == 0)
		in->n_data_chunks++;

	yaffs_load_tnode_0(dev, tn, inode_chunk, nand_chunk);

	return YAFFS_OK;
}

static void yaffs_soft_del_chunk(struct yaffs_dev *dev, int chunk)
{
	struct yaffs_block_info *the_block;
	unsigned block_no;

	yaffs_trace(YAFFS_TRACE_DELETION, "soft delete chunk %d", chunk);

	block_no = chunk / dev->param.chunks_per_block;
	the_block = yaffs_get_block_info(dev, block_no);
	if (the_block) {
		the_block->soft_del_pages++;
		dev->n_free_chunks++;
		yaffs2_update_oldest_dirty_seq(dev, block_no, the_block);
	}
}

/* SoftDeleteWorker scans backwards through the tnode tree and soft deletes all
 * the chunks in the file.
 * All soft deleting does is increment the block's softdelete count and pulls
 * the chunk out of the tnode.
 * Thus, essentially this is the same as DeleteWorker except that the chunks
 * are soft deleted.
 */

static int yaffs_soft_del_worker(struct yaffs_obj *in, struct yaffs_tnode *tn,
				 u32 level, int chunk_offset)
{
	int i;
	int the_chunk;
	int all_done = 1;
	struct yaffs_dev *dev = in->my_dev;

	if (!tn)
		return 1;

	if (level > 0) {
		for (i = YAFFS_NTNODES_INTERNAL - 1;
			all_done && i >= 0;
			i--) {
			if (tn->internal[i]) {
				all_done =
				    yaffs_soft_del_worker(in,
					tn->internal[i],
					level - 1,
					(chunk_offset <<
					YAFFS_TNODES_INTERNAL_BITS)
					+ i);
				if (all_done) {
					yaffs_free_tnode(dev,
						tn->internal[i]);
					tn->internal[i] = NULL;
				} else {
					/* Can this happen? */
				}
			}
		}
		return (all_done) ? 1 : 0;
	}

	/* level 0 */
	 for (i = YAFFS_NTNODES_LEVEL0 - 1; i >= 0; i--) {
		the_chunk = yaffs_get_group_base(dev, tn, i);
		if (the_chunk) {
			yaffs_soft_del_chunk(dev, the_chunk);
			yaffs_load_tnode_0(dev, tn, i, 0);
		}
	}
	return 1;
}

static void yaffs_remove_obj_from_dir(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev = obj->my_dev;
	struct yaffs_obj *parent;

	yaffs_verify_obj_in_dir(obj);
	parent = obj->parent;

	yaffs_verify_dir(parent);

	if (dev && dev->param.remove_obj_fn)
		dev->param.remove_obj_fn(obj);

	list_del_init(&obj->siblings);
	obj->parent = NULL;

	yaffs_verify_dir(parent);
}

void yaffs_add_obj_to_dir(struct yaffs_obj *directory, struct yaffs_obj *obj)
{
	if (!directory) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: Trying to add an object to a null pointer directory"
			);
		BUG();
		return;
	}
	if (directory->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: Trying to add an object to a non-directory"
			);
		BUG();
	}

	if (obj->siblings.prev == NULL) {
		/* Not initialised */
		BUG();
	}

	yaffs_verify_dir(directory);

	yaffs_remove_obj_from_dir(obj);

	/* Now add it */
	list_add(&obj->siblings, &directory->variant.dir_variant.children);
	obj->parent = directory;

	if (directory == obj->my_dev->unlinked_dir
	    || directory == obj->my_dev->del_dir) {
		obj->unlinked = 1;
		obj->my_dev->n_unlinked_files++;
		obj->rename_allowed = 0;
	}

	yaffs_verify_dir(directory);
	yaffs_verify_obj_in_dir(obj);
}

static int yaffs_change_obj_name(struct yaffs_obj *obj,
				 struct yaffs_obj *new_dir,
				 const YCHAR *new_name, int force, int shadows)
{
	int unlink_op;
	int del_op;
	struct yaffs_obj *existing_target;

	if (new_dir == NULL)
		new_dir = obj->parent;	/* use the old directory */

	if (new_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: yaffs_change_obj_name: new_dir is not a directory"
			);
		BUG();
	}

	unlink_op = (new_dir == obj->my_dev->unlinked_dir);
	del_op = (new_dir == obj->my_dev->del_dir);

	existing_target = yaffs_find_by_name(new_dir, new_name);

	/* If the object is a file going into the unlinked directory,
	 *   then it is OK to just stuff it in since duplicate names are OK.
	 *   else only proceed if the new name does not exist and we're putting
	 *   it into a directory.
	 */
	if (!(unlink_op || del_op || force ||
	      shadows > 0 || !existing_target) ||
	      new_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY)
		return YAFFS_FAIL;

	yaffs_set_obj_name(obj, new_name);
	obj->dirty = 1;
	yaffs_add_obj_to_dir(new_dir, obj);

	if (unlink_op)
		obj->unlinked = 1;

	/* If it is a deletion then we mark it as a shrink for gc  */
	if (yaffs_update_oh(obj, new_name, 0, del_op, shadows, NULL) >= 0)
		return YAFFS_OK;

	return YAFFS_FAIL;
}


static void yaffs_unhash_obj(struct yaffs_obj *obj)
{
	int bucket;
	struct yaffs_dev *dev = obj->my_dev;

	/* If it is still linked into the bucket list, free from the list */
	if (!list_empty(&obj->hash_link)) {
		list_del_init(&obj->hash_link);
		bucket = yaffs_hash_fn(obj->obj_id);
		dev->obj_bucket[bucket].count--;
	}
}

/*  FreeObject frees up a Object and puts it back on the free list */
static void yaffs_free_obj(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev;

	if (!obj) {
		BUG();
		return;
	}
	dev = obj->my_dev;
	yaffs_trace(YAFFS_TRACE_OS, "FreeObject %p inode %p",
		obj, obj->my_inode);
	if (obj->parent)
		BUG();
	if (!list_empty(&obj->siblings))
		BUG();

	if (obj->my_inode) {
		/* We're still hooked up to a cached inode.
		 * Don't delete now, but mark for later deletion
		 */
		obj->defered_free = 1;
		return;
	}

	yaffs_unhash_obj(obj);

	yaffs_free_raw_obj(dev, obj);
	dev->n_obj--;
	dev->checkpoint_blocks_required = 0;	/* force recalculation */
}

void yaffs_handle_defered_free(struct yaffs_obj *obj)
{
	if (obj->defered_free)
		yaffs_free_obj(obj);
}

static int yaffs_generic_obj_del(struct yaffs_obj *in)
{
	/* Iinvalidate the file's data in the cache, without flushing. */
	yaffs_invalidate_file_cache(in);

	if (in->my_dev->param.is_yaffs2 && in->parent != in->my_dev->del_dir) {
		/* Move to unlinked directory so we have a deletion record */
		yaffs_change_obj_name(in, in->my_dev->del_dir, _Y("deleted"), 0,
				      0);
	}

	yaffs_remove_obj_from_dir(in);
	yaffs_chunk_del(in->my_dev, in->hdr_chunk, 1, __LINE__);
	in->hdr_chunk = 0;

	yaffs_free_obj(in);
	return YAFFS_OK;

}

static void yaffs_soft_del_file(struct yaffs_obj *obj)
{
	if (!obj->deleted ||
	    obj->variant_type != YAFFS_OBJECT_TYPE_FILE ||
	    obj->soft_del)
		return;

	if (obj->n_data_chunks <= 0) {
		/* Empty file with no duplicate object headers,
		 * just delete it immediately */
		yaffs_free_tnode(obj->my_dev, obj->variant.file_variant.top);
		obj->variant.file_variant.top = NULL;
		yaffs_trace(YAFFS_TRACE_TRACING,
			"yaffs: Deleting empty file %d",
			obj->obj_id);
		yaffs_generic_obj_del(obj);
	} else {
		yaffs_soft_del_worker(obj,
				      obj->variant.file_variant.top,
				      obj->variant.
				      file_variant.top_level, 0);
		obj->soft_del = 1;
	}
}

/* Pruning removes any part of the file structure tree that is beyond the
 * bounds of the file (ie that does not point to chunks).
 *
 * A file should only get pruned when its size is reduced.
 *
 * Before pruning, the chunks must be pulled from the tree and the
 * level 0 tnode entries must be zeroed out.
 * Could also use this for file deletion, but that's probably better handled
 * by a special case.
 *
 * This function is recursive. For levels > 0 the function is called again on
 * any sub-tree. For level == 0 we just check if the sub-tree has data.
 * If there is no data in a subtree then it is pruned.
 */

static struct yaffs_tnode *yaffs_prune_worker(struct yaffs_dev *dev,
					      struct yaffs_tnode *tn, u32 level,
					      int del0)
{
	int i;
	int has_data;

	if (!tn)
		return tn;

	has_data = 0;

	if (level > 0) {
		for (i = 0; i < YAFFS_NTNODES_INTERNAL; i++) {
			if (tn->internal[i]) {
				tn->internal[i] =
				    yaffs_prune_worker(dev,
						tn->internal[i],
						level - 1,
						(i == 0) ? del0 : 1);
			}

			if (tn->internal[i])
				has_data++;
		}
	} else {
		int tnode_size_u32 = dev->tnode_size / sizeof(u32);
		u32 *map = (u32 *) tn;

		for (i = 0; !has_data && i < tnode_size_u32; i++) {
			if (map[i])
				has_data++;
		}
	}

	if (has_data == 0 && del0) {
		/* Free and return NULL */
		yaffs_free_tnode(dev, tn);
		tn = NULL;
	}
	return tn;
}

static int yaffs_prune_tree(struct yaffs_dev *dev,
			    struct yaffs_file_var *file_struct)
{
	int i;
	int has_data;
	int done = 0;
	struct yaffs_tnode *tn;

	if (file_struct->top_level < 1)
		return YAFFS_OK;

	file_struct->top =
	   yaffs_prune_worker(dev, file_struct->top, file_struct->top_level, 0);

	/* Now we have a tree with all the non-zero branches NULL but
	 * the height is the same as it was.
	 * Let's see if we can trim internal tnodes to shorten the tree.
	 * We can do this if only the 0th element in the tnode is in use
	 * (ie all the non-zero are NULL)
	 */

	while (file_struct->top_level && !done) {
		tn = file_struct->top;

		has_data = 0;
		for (i = 1; i < YAFFS_NTNODES_INTERNAL; i++) {
			if (tn->internal[i])
				has_data++;
		}

		if (!has_data) {
			file_struct->top = tn->internal[0];
			file_struct->top_level--;
			yaffs_free_tnode(dev, tn);
		} else {
			done = 1;
		}
	}

	return YAFFS_OK;
}

/*-------------------- End of File Structure functions.-------------------*/

/* alloc_empty_obj gets us a clean Object.*/
static struct yaffs_obj *yaffs_alloc_empty_obj(struct yaffs_dev *dev)
{
	struct yaffs_obj *obj = yaffs_alloc_raw_obj(dev);

	if (!obj)
		return obj;

	dev->n_obj++;

	/* Now sweeten it up... */

	memset(obj, 0, sizeof(struct yaffs_obj));
	obj->being_created = 1;

	obj->my_dev = dev;
	obj->hdr_chunk = 0;
	obj->variant_type = YAFFS_OBJECT_TYPE_UNKNOWN;
	INIT_LIST_HEAD(&(obj->hard_links));
	INIT_LIST_HEAD(&(obj->hash_link));
	INIT_LIST_HEAD(&obj->siblings);

	/* Now make the directory sane */
	if (dev->root_dir) {
		obj->parent = dev->root_dir;
		list_add(&(obj->siblings),
			 &dev->root_dir->variant.dir_variant.children);
	}

	/* Add it to the lost and found directory.
	 * NB Can't put root or lost-n-found in lost-n-found so
	 * check if lost-n-found exists first
	 */
	if (dev->lost_n_found)
		yaffs_add_obj_to_dir(dev->lost_n_found, obj);

	obj->being_created = 0;

	dev->checkpoint_blocks_required = 0;	/* force recalculation */

	return obj;
}

static int yaffs_find_nice_bucket(struct yaffs_dev *dev)
{
	int i;
	int l = 999;
	int lowest = 999999;

	/* Search for the shortest list or one that
	 * isn't too long.
	 */

	for (i = 0; i < 10 && lowest > 4; i++) {
		dev->bucket_finder++;
		dev->bucket_finder %= YAFFS_NOBJECT_BUCKETS;
		if (dev->obj_bucket[dev->bucket_finder].count < lowest) {
			lowest = dev->obj_bucket[dev->bucket_finder].count;
			l = dev->bucket_finder;
		}
	}

	return l;
}

static int yaffs_new_obj_id(struct yaffs_dev *dev)
{
	int bucket = yaffs_find_nice_bucket(dev);
	int found = 0;
	struct list_head *i;
	u32 n = (u32) bucket;

	/*
	 * Now find an object value that has not already been taken
	 * by scanning the list, incrementing each time by number of buckets.
	 */
	while (!found) {
		found = 1;
		n += YAFFS_NOBJECT_BUCKETS;
		list_for_each(i, &dev->obj_bucket[bucket].list) {
			/* Check if this value is already taken. */
			if (i && list_entry(i, struct yaffs_obj,
					    hash_link)->obj_id == n)
				found = 0;
		}
	}
	return n;
}

static void yaffs_hash_obj(struct yaffs_obj *in)
{
	int bucket = yaffs_hash_fn(in->obj_id);
	struct yaffs_dev *dev = in->my_dev;

	list_add(&in->hash_link, &dev->obj_bucket[bucket].list);
	dev->obj_bucket[bucket].count++;
}

struct yaffs_obj *yaffs_find_by_number(struct yaffs_dev *dev, u32 number)
{
	int bucket = yaffs_hash_fn(number);
	struct list_head *i;
	struct yaffs_obj *in;

	list_for_each(i, &dev->obj_bucket[bucket].list) {
		/* Look if it is in the list */
		in = list_entry(i, struct yaffs_obj, hash_link);
		if (in->obj_id == number) {
			/* Don't show if it is defered free */
			if (in->defered_free)
				return NULL;
			return in;
		}
	}

	return NULL;
}

static struct yaffs_obj *yaffs_new_obj(struct yaffs_dev *dev, int number,
				enum yaffs_obj_type type)
{
	struct yaffs_obj *the_obj = NULL;
	struct yaffs_tnode *tn = NULL;

	if (number < 0)
		number = yaffs_new_obj_id(dev);

	if (type == YAFFS_OBJECT_TYPE_FILE) {
		tn = yaffs_get_tnode(dev);
		if (!tn)
			return NULL;
	}

	the_obj = yaffs_alloc_empty_obj(dev);
	if (!the_obj) {
		if (tn)
			yaffs_free_tnode(dev, tn);
		return NULL;
	}

	the_obj->fake = 0;
	the_obj->rename_allowed = 1;
	the_obj->unlink_allowed = 1;
	the_obj->obj_id = number;
	yaffs_hash_obj(the_obj);
	the_obj->variant_type = type;
	yaffs_load_current_time(the_obj, 1, 1);

	switch (type) {
	case YAFFS_OBJECT_TYPE_FILE:
		the_obj->variant.file_variant.file_size = 0;
		the_obj->variant.file_variant.stored_size = 0;
		the_obj->variant.file_variant.shrink_size =
						yaffs_max_file_size(dev);
		the_obj->variant.file_variant.top_level = 0;
		the_obj->variant.file_variant.top = tn;
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		INIT_LIST_HEAD(&the_obj->variant.dir_variant.children);
		INIT_LIST_HEAD(&the_obj->variant.dir_variant.dirty);
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
	case YAFFS_OBJECT_TYPE_HARDLINK:
	case YAFFS_OBJECT_TYPE_SPECIAL:
		/* No action required */
		break;
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		/* todo this should not happen */
		break;
	}
	return the_obj;
}

static struct yaffs_obj *yaffs_create_fake_dir(struct yaffs_dev *dev,
					       int number, u32 mode)
{

	struct yaffs_obj *obj =
	    yaffs_new_obj(dev, number, YAFFS_OBJECT_TYPE_DIRECTORY);

	if (!obj)
		return NULL;

	obj->fake = 1;	/* it is fake so it might not use NAND */
	obj->rename_allowed = 0;
	obj->unlink_allowed = 0;
	obj->deleted = 0;
	obj->unlinked = 0;
	obj->yst_mode = mode;
	obj->my_dev = dev;
	obj->hdr_chunk = 0;	/* Not a valid chunk. */
	return obj;

}


static void yaffs_init_tnodes_and_objs(struct yaffs_dev *dev)
{
	int i;

	dev->n_obj = 0;
	dev->n_tnodes = 0;
	yaffs_init_raw_tnodes_and_objs(dev);

	for (i = 0; i < YAFFS_NOBJECT_BUCKETS; i++) {
		INIT_LIST_HEAD(&dev->obj_bucket[i].list);
		dev->obj_bucket[i].count = 0;
	}
}

struct yaffs_obj *yaffs_find_or_create_by_number(struct yaffs_dev *dev,
						 int number,
						 enum yaffs_obj_type type)
{
	struct yaffs_obj *the_obj = NULL;

	if (number > 0)
		the_obj = yaffs_find_by_number(dev, number);

	if (!the_obj)
		the_obj = yaffs_new_obj(dev, number, type);

	return the_obj;

}

YCHAR *yaffs_clone_str(const YCHAR *str)
{
	YCHAR *new_str = NULL;
	int len;

	if (!str)
		str = _Y("");

	len = strnlen(str, YAFFS_MAX_ALIAS_LENGTH);
	new_str = kmalloc((len + 1) * sizeof(YCHAR), GFP_NOFS);
	if (new_str) {
		strncpy(new_str, str, len);
		new_str[len] = 0;
	}
	return new_str;

}
/*
 *yaffs_update_parent() handles fixing a directories mtime and ctime when a new
 * link (ie. name) is created or deleted in the directory.
 *
 * ie.
 *   create dir/a : update dir's mtime/ctime
 *   rm dir/a:   update dir's mtime/ctime
 *   modify dir/a: don't update dir's mtimme/ctime
 *
 * This can be handled immediately or defered. Defering helps reduce the number
 * of updates when many files in a directory are changed within a brief period.
 *
 * If the directory updating is defered then yaffs_update_dirty_dirs must be
 * called periodically.
 */

static void yaffs_update_parent(struct yaffs_obj *obj)
{
	struct yaffs_dev *dev;

	if (!obj)
		return;
	dev = obj->my_dev;
	obj->dirty = 1;
	yaffs_load_current_time(obj, 0, 1);
	if (dev->param.defered_dir_update) {
		struct list_head *link = &obj->variant.dir_variant.dirty;

		if (list_empty(link)) {
			list_add(link, &dev->dirty_dirs);
			yaffs_trace(YAFFS_TRACE_BACKGROUND,
			  "Added object %d to dirty directories",
			   obj->obj_id);
		}

	} else {
		yaffs_update_oh(obj, NULL, 0, 0, 0, NULL);
	}
}

void yaffs_update_dirty_dirs(struct yaffs_dev *dev)
{
	struct list_head *link;
	struct yaffs_obj *obj;
	struct yaffs_dir_var *d_s;
	union yaffs_obj_var *o_v;

	yaffs_trace(YAFFS_TRACE_BACKGROUND, "Update dirty directories");

	while (!list_empty(&dev->dirty_dirs)) {
		link = dev->dirty_dirs.next;
		list_del_init(link);

		d_s = list_entry(link, struct yaffs_dir_var, dirty);
		o_v = list_entry(d_s, union yaffs_obj_var, dir_variant);
		obj = list_entry(o_v, struct yaffs_obj, variant);

		yaffs_trace(YAFFS_TRACE_BACKGROUND, "Update directory %d",
			obj->obj_id);

		if (obj->dirty)
			yaffs_update_oh(obj, NULL, 0, 0, 0, NULL);
	}
}

/*
 * Mknod (create) a new object.
 * equiv_obj only has meaning for a hard link;
 * alias_str only has meaning for a symlink.
 * rdev only has meaning for devices (a subset of special objects)
 */

static struct yaffs_obj *yaffs_create_obj(enum yaffs_obj_type type,
					  struct yaffs_obj *parent,
					  const YCHAR *name,
					  u32 mode,
					  u32 uid,
					  u32 gid,
					  struct yaffs_obj *equiv_obj,
					  const YCHAR *alias_str, u32 rdev)
{
	struct yaffs_obj *in;
	YCHAR *str = NULL;
	struct yaffs_dev *dev = parent->my_dev;

	/* Check if the entry exists.
	 * If it does then fail the call since we don't want a dup. */
	if (yaffs_find_by_name(parent, name))
		return NULL;

	if (type == YAFFS_OBJECT_TYPE_SYMLINK) {
		str = yaffs_clone_str(alias_str);
		if (!str)
			return NULL;
	}

	in = yaffs_new_obj(dev, -1, type);

	if (!in) {
		kfree(str);
		return NULL;
	}

	in->hdr_chunk = 0;
	in->valid = 1;
	in->variant_type = type;

	in->yst_mode = mode;

	yaffs_attribs_init(in, gid, uid, rdev);

	in->n_data_chunks = 0;

	yaffs_set_obj_name(in, name);
	in->dirty = 1;

	yaffs_add_obj_to_dir(parent, in);

	in->my_dev = parent->my_dev;

	switch (type) {
	case YAFFS_OBJECT_TYPE_SYMLINK:
		in->variant.symlink_variant.alias = str;
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		in->variant.hardlink_variant.equiv_obj = equiv_obj;
		in->variant.hardlink_variant.equiv_id = equiv_obj->obj_id;
		list_add(&in->hard_links, &equiv_obj->hard_links);
		break;
	case YAFFS_OBJECT_TYPE_FILE:
	case YAFFS_OBJECT_TYPE_DIRECTORY:
	case YAFFS_OBJECT_TYPE_SPECIAL:
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		/* do nothing */
		break;
	}

	if (yaffs_update_oh(in, name, 0, 0, 0, NULL) < 0) {
		/* Could not create the object header, fail */
		yaffs_del_obj(in);
		in = NULL;
	}

	if (in)
		yaffs_update_parent(parent);

	return in;
}

struct yaffs_obj *yaffs_create_file(struct yaffs_obj *parent,
				    const YCHAR *name, u32 mode, u32 uid,
				    u32 gid)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_FILE, parent, name, mode,
				uid, gid, NULL, NULL, 0);
}

struct yaffs_obj *yaffs_create_dir(struct yaffs_obj *parent, const YCHAR *name,
				   u32 mode, u32 uid, u32 gid)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_DIRECTORY, parent, name,
				mode, uid, gid, NULL, NULL, 0);
}

struct yaffs_obj *yaffs_create_special(struct yaffs_obj *parent,
				       const YCHAR *name, u32 mode, u32 uid,
				       u32 gid, u32 rdev)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_SPECIAL, parent, name, mode,
				uid, gid, NULL, NULL, rdev);
}

struct yaffs_obj *yaffs_create_symlink(struct yaffs_obj *parent,
				       const YCHAR *name, u32 mode, u32 uid,
				       u32 gid, const YCHAR *alias)
{
	return yaffs_create_obj(YAFFS_OBJECT_TYPE_SYMLINK, parent, name, mode,
				uid, gid, NULL, alias, 0);
}

/* yaffs_link_obj returns the object id of the equivalent object.*/
struct yaffs_obj *yaffs_link_obj(struct yaffs_obj *parent, const YCHAR * name,
				 struct yaffs_obj *equiv_obj)
{
	/* Get the real object in case we were fed a hard link obj */
	equiv_obj = yaffs_get_equivalent_obj(equiv_obj);

	if (yaffs_create_obj(YAFFS_OBJECT_TYPE_HARDLINK,
			parent, name, 0, 0, 0,
			equiv_obj, NULL, 0))
		return equiv_obj;

	return NULL;

}



/*---------------------- Block Management and Page Allocation -------------*/

static void yaffs_deinit_blocks(struct yaffs_dev *dev)
{
	if (dev->block_info_alt && dev->block_info)
		vfree(dev->block_info);
	else
		kfree(dev->block_info);

	dev->block_info_alt = 0;

	dev->block_info = NULL;

	if (dev->chunk_bits_alt && dev->chunk_bits)
		vfree(dev->chunk_bits);
	else
		kfree(dev->chunk_bits);
	dev->chunk_bits_alt = 0;
	dev->chunk_bits = NULL;
}

static int yaffs_init_blocks(struct yaffs_dev *dev)
{
	int n_blocks = dev->internal_end_block - dev->internal_start_block + 1;

	dev->block_info = NULL;
	dev->chunk_bits = NULL;
	dev->alloc_block = -1;	/* force it to get a new one */

#if defined(GC_Qlearning)
dev->alloc_block2 = -1;
dev->alloc_block3 = -1;
dev->alloc_block4 = -1;
#endif


	/* If the first allocation strategy fails, thry the alternate one */
	dev->block_info =
		kmalloc(n_blocks * sizeof(struct yaffs_block_info), GFP_NOFS);
	if (!dev->block_info) {
		dev->block_info =
		    vmalloc(n_blocks * sizeof(struct yaffs_block_info));
		dev->block_info_alt = 1;
	} else {
		dev->block_info_alt = 0;
	}

	if (!dev->block_info)
		goto alloc_error;

	/* Set up dynamic blockinfo stuff. Round up bytes. */
	dev->chunk_bit_stride = (dev->param.chunks_per_block + 7) / 8;
	dev->chunk_bits =
		kmalloc(dev->chunk_bit_stride * n_blocks, GFP_NOFS);
	if (!dev->chunk_bits) {
		dev->chunk_bits =
		    vmalloc(dev->chunk_bit_stride * n_blocks);
		dev->chunk_bits_alt = 1;
	} else {
		dev->chunk_bits_alt = 0;
	}
	if (!dev->chunk_bits)
		goto alloc_error;


	memset(dev->block_info, 0, n_blocks * sizeof(struct yaffs_block_info));
	memset(dev->chunk_bits, 0, dev->chunk_bit_stride * n_blocks);
	return YAFFS_OK;

alloc_error:
	yaffs_deinit_blocks(dev);
	return YAFFS_FAIL;
}


void yaffs_block_became_dirty(struct yaffs_dev *dev, int block_no)
{
	struct yaffs_block_info *bi = yaffs_get_block_info(dev, block_no);
	int erased_ok = 0;
	u32 i;

	/* If the block is still healthy erase it and mark as clean.
	 * If the block has had a data failure, then retire it.
	 */

	yaffs_trace(YAFFS_TRACE_GC | YAFFS_TRACE_ERASE,
		"yaffs_block_became_dirty block %d state %d %s",
		block_no, bi->block_state,
		(bi->needs_retiring) ? "needs retiring" : "");

	yaffs2_clear_oldest_dirty_seq(dev, bi);

	bi->block_state = YAFFS_BLOCK_STATE_DIRTY;

	/* If this is the block being garbage collected then stop gc'ing */
	if (block_no == (int)dev->gc_block)
		dev->gc_block = 0;

	/* If this block is currently the best candidate for gc
	 * then drop as a candidate */
	if (block_no == (int)dev->gc_dirtiest) {
		dev->gc_dirtiest = 0;
		dev->gc_pages_in_use = 0;
	}

	if (!bi->needs_retiring) {
		yaffs2_checkpt_invalidate(dev);
		erased_ok = yaffs_erase_block(dev, block_no);
		if (!erased_ok) {
			dev->n_erase_failures++;
			printk("\nerasure failed");
			yaffs_trace(YAFFS_TRACE_ERROR | YAFFS_TRACE_BAD_BLOCKS,
			  "**>> Erasure failed %d", block_no);
		}
	}

	/* Verify erasure if needed */
	if (erased_ok &&
	    ((yaffs_trace_mask & YAFFS_TRACE_ERASE) ||
	     !yaffs_skip_verification(dev))) {
		for (i = 0; i < dev->param.chunks_per_block; i++) {
			if (!yaffs_check_chunk_erased(dev,
				block_no * dev->param.chunks_per_block + i)) {
				yaffs_trace(YAFFS_TRACE_ERROR,
					">>Block %d erasure supposedly OK, but chunk %d not erased",
					block_no, i);
			}
		}
	}

	if (!erased_ok) {
		/* We lost a block of free space */
		dev->n_free_chunks -= dev->param.chunks_per_block;
		yaffs_retire_block(dev, block_no);
		yaffs_trace(YAFFS_TRACE_ERROR | YAFFS_TRACE_BAD_BLOCKS,
			"**>> Block %d retired", block_no);
		return;
	}

	/* Clean it up... */
	bi->block_state = YAFFS_BLOCK_STATE_EMPTY;
	bi->seq_number = 0;
	dev->n_erased_blocks++;
	bi->pages_in_use = 0;
	bi->soft_del_pages = 0;
	bi->has_shrink_hdr = 0;
	bi->skip_erased_check = 1;	/* Clean, so no need to check */
	bi->gc_prioritise = 0;
	bi->has_summary = 0;

	yaffs_clear_chunk_bits(dev, block_no);

/*add by hy 2017.12.15*/
#if defined(GC_Qlearning)
sum_block[block_no-1].sum = 0; 
sum_block[block_no-1].cnt = 0;
sum_block[block_no-1].chunk_seq = 0;
#endif
/*add end*/

	yaffs_trace(YAFFS_TRACE_ERASE, "Erased block %d", block_no);
}

static inline int yaffs_gc_process_chunk(struct yaffs_dev *dev,
					struct yaffs_block_info *bi,
					int old_chunk, u8 *buffer,int aver_uft,int start_new)
{
	int new_chunk;
	int mark_flash = 1;
	struct yaffs_ext_tags tags;
	struct yaffs_obj *object;
	int matching_chunk;
	int ret_val = YAFFS_OK;

	memset(&tags, 0, sizeof(tags));
	yaffs_rd_chunk_tags_nand(dev, old_chunk,
				 buffer, &tags);
	object = yaffs_find_by_number(dev, tags.obj_id);

	yaffs_trace(YAFFS_TRACE_GC_DETAIL,
		"Collecting chunk in block %d, %d %d %d ",
		dev->gc_chunk, tags.obj_id,
		tags.chunk_id, tags.n_bytes);

	if (object && !yaffs_skip_verification(dev)) {
		if (tags.chunk_id == 0)
			matching_chunk =
			    object->hdr_chunk;
		else if (object->soft_del)
			/* Defeat the test */
			matching_chunk = old_chunk;
		else
			matching_chunk =
			    yaffs_find_chunk_in_file
			    (object, tags.chunk_id,
			     NULL);

		if (old_chunk != matching_chunk)
			yaffs_trace(YAFFS_TRACE_ERROR,
				"gc: page in gc mismatch: %d %d %d %d",
				old_chunk,
				matching_chunk,
				tags.obj_id,
				tags.chunk_id);
	}

	if (!object) {
		yaffs_trace(YAFFS_TRACE_ERROR,
			"page %d in gc has no object: %d %d %d ",
			old_chunk,
			tags.obj_id, tags.chunk_id,
			tags.n_bytes);
	}

	if (object &&
	    object->deleted &&
	    object->soft_del && tags.chunk_id != 0) {
		/* Data chunk in a soft deleted file,
		 * throw it away.
		 * It's a soft deleted data chunk,
		 * No need to copy this, just forget
		 * about it and fix up the object.
		 */

		/* Free chunks already includes
		 * softdeleted chunks, how ever this
		 * chunk is going to soon be really
		 * deleted which will increment free
		 * chunks. We have to decrement free
		 * chunks so this works out properly.
		 */
		dev->n_free_chunks--;
		bi->soft_del_pages--;

		object->n_data_chunks--;
		if (object->n_data_chunks <= 0) {
			/* remeber to clean up obj */
			dev->gc_cleanup_list[dev->n_clean_ups] = tags.obj_id;
			dev->n_clean_ups++;
		}
		mark_flash = 0;
	} else if (object) {
		/* It's either a data chunk in a live
		 * file or an ObjectHeader, so we're
		 * interested in it.
		 * NB Need to keep the ObjectHeaders of
		 * deleted files until the whole file
		 * has been deleted off
		 */
		tags.serial_number++;
		dev->n_gc_copies++;

		if (tags.chunk_id == 0) {
			/* It is an object Id,
			 * We need to nuke the shrinkheader flags since its
			 * work is done.
			 * Also need to clean up shadowing.
			 * NB We don't want to do all the work of translating
			 * object header endianism back and forth so we leave
			 * the oh endian in its stored order.
			 */

			struct yaffs_obj_hdr *oh;
			oh = (struct yaffs_obj_hdr *) buffer;

			oh->is_shrink = 0;
			tags.extra_is_shrink = 0;
			oh->shadows_obj = 0;
			oh->inband_shadowed_obj_id = 0;
			tags.extra_shadows = 0;

			/* Update file size */
			if (object->variant_type == YAFFS_OBJECT_TYPE_FILE) {
				yaffs_oh_size_load(dev, oh,
				    object->variant.file_variant.stored_size, 1);
				tags.extra_file_size =
				    object->variant.file_variant.stored_size;
			}

			yaffs_verify_oh(object, oh, &tags, 1);
			new_chunk =
			    yaffs_write_new_chunk_for_migration(dev, (u8 *) oh, &tags, 1,aver_uft,start_new);
		} else {
			new_chunk =
			    yaffs_write_new_chunk_for_migration(dev, buffer, &tags, 1,aver_uft,start_new);
		}

		if (new_chunk < 0) {
			ret_val = YAFFS_FAIL;
			printk("error in write new chunk for migration");
		} else {

			/* Now fix up the Tnodes etc. */

			if (tags.chunk_id == 0) {
				/* It's a header */
				object->hdr_chunk = new_chunk;
				object->serial = tags.serial_number;
			} else {
				/* It's a data chunk */
				yaffs_put_chunk_in_file(object, tags.chunk_id,
							new_chunk, 0);
			}
		}
	}
	if (ret_val == YAFFS_OK)
		yaffs_chunk_del(dev, old_chunk, mark_flash, __LINE__);
	return ret_val;
}


static int yaffs_gc_block(struct yaffs_dev *dev, int block, int whole_block,int start)
{
	int old_chunk;
	int ret_val = YAFFS_OK;
	u32 i;
	int is_checkpt_block;
	int max_copies;
	int chunks_before = yaffs_get_erased_chunks(dev);
	int chunks_after;

//add by hy
    int pages_used = 0;
    int aver_uft = 0;
	int j = 0;
	struct yaffs_block_info *tmp_bi = 0;
	int full_block_cnt = 0;
//add end 

	struct yaffs_block_info *bi = yaffs_get_block_info(dev, block);

	is_checkpt_block = (bi->block_state == YAFFS_BLOCK_STATE_CHECKPOINT);

	yaffs_trace(YAFFS_TRACE_TRACING,
		"Collecting block %d, in use %d, shrink %d, whole_block %d",
		block, bi->pages_in_use, bi->has_shrink_hdr,
		whole_block);

	/*yaffs_verify_free_chunks(dev); */

	if (bi->block_state == YAFFS_BLOCK_STATE_FULL)
		bi->block_state = YAFFS_BLOCK_STATE_COLLECTING;

	bi->has_shrink_hdr = 0;	/* clear the flag so that the block can erase */

	dev->gc_disable = 1;

	yaffs_summary_gc(dev, block);

	if (is_checkpt_block || !yaffs_still_some_chunks(dev, block)) {
		yaffs_trace(YAFFS_TRACE_TRACING,
			"Collecting block %d that has no chunks in use",
			block);
		yaffs_block_became_dirty(dev, block);
	} else {

		u8 *buffer = yaffs_get_temp_buffer(dev);

		yaffs_verify_blk(dev, bi, block);

		max_copies = (whole_block) ? dev->param.chunks_per_block : 5;
		old_chunk = block * dev->param.chunks_per_block + dev->gc_chunk;
#ifdef GC_Qlearning 
/* started add by hy 2018.1.9 */
       //here we caculate the average_uft
        unsigned now = TIME_NOW;
		for(j = dev->internal_start_block; j <= dev->internal_end_block;  j++)
		{
           tmp_bi = yaffs_get_block_info(dev,j);
		   if (tmp_bi->block_state == YAFFS_BLOCK_STATE_FULL)
		   {
		     pages_used = tmp_bi->pages_in_use - tmp_bi->soft_del_pages;
		     aver_uft += (((now - time_alloc_block[j-1]) * pages_used) / dev->param.chunks_per_block);
			 full_block_cnt++;
		   }
		}
		aver_uft /= full_block_cnt;  //get average value
#endif


		for (/* init already done */ ;
		     ret_val == YAFFS_OK &&
		     dev->gc_chunk < dev->param.chunks_per_block &&
		     (bi->block_state == YAFFS_BLOCK_STATE_COLLECTING) &&
		     max_copies > 0;
		     dev->gc_chunk++, old_chunk++) {
			if (yaffs_check_chunk_bit(dev, block, dev->gc_chunk)) {
				/* Page is in use and might need to be copied */
				max_copies--;
				ret_val = yaffs_gc_process_chunk(dev, bi,
							old_chunk, buffer, aver_uft,start);
			}
		}
		yaffs_release_temp_buffer(dev, buffer);
	}

	yaffs_verify_collected_blk(dev, bi, block);

	if (bi->block_state == YAFFS_BLOCK_STATE_COLLECTING) {
		/*
		 * The gc did not complete. Set block state back to FULL
		 * because checkpointing does not restore gc.
		 */
		bi->block_state = YAFFS_BLOCK_STATE_FULL;
	} else {
		/* The gc completed. */
		/* Do any required cleanups */
		for (i = 0; i < dev->n_clean_ups; i++) {
			/* Time to delete the file too */
			struct yaffs_obj *object =
			    yaffs_find_by_number(dev, dev->gc_cleanup_list[i]);
			if (object) {
				yaffs_free_tnode(dev,
					  object->variant.file_variant.top);
				object->variant.file_variant.top = NULL;
				yaffs_trace(YAFFS_TRACE_GC,
					"yaffs: About to finally delete object %d",
					object->obj_id);
				yaffs_generic_obj_del(object);
				object->my_dev->n_deleted_files--;
			}

		}
		chunks_after = yaffs_get_erased_chunks(dev);
		if (chunks_before >= chunks_after)
			yaffs_trace(YAFFS_TRACE_GC,
				"gc did not increase free chunks before %d after %d",
				chunks_before, chunks_after);
		else {
			/* ========== 块恢复统计 ========== */
#ifdef DEBUG_QLGC
			ql_stats.chunks_recovered_total += (chunks_after - chunks_before);
#endif
		}
		dev->gc_block = 0;
		dev->gc_chunk = 0;
		dev->n_clean_ups = 0;
	}

	dev->gc_disable = 0;

	return ret_val;
}

/*
 * find_gc_block() selects the dirtiest block (or close enough)
 * for garbage collection.
 */

static unsigned yaffs_find_gc_block(struct yaffs_dev *dev,
				    int aggressive, int background)
{
	u32 i=0;
	u32 iterations;
	u32 selected = 0;
	int prioritised = 0;
	int prioritised_exist = 0;
	struct yaffs_block_info *bi;
	u32 threshold;

#ifdef GC_Qlearning
	int fagc_ok;
    int j=0;
	int index = 0;
	int valid_block_cnt=0;
	int min_earase_num = 999999;
	unsigned cwps=0;
	int max_cwps=0;
	int now =0;
#endif	

	(void) prioritised;

	/* First let's see if we need to grab a prioritised block */
	if (dev->has_pending_prioritised_gc && !aggressive) {
		dev->gc_dirtiest = 0;
		bi = dev->block_info;
		for (i = dev->internal_start_block;
		     i <= dev->internal_end_block && !selected; i++) {

			if (bi->gc_prioritise) {
				prioritised_exist = 1;
				if (bi->block_state == YAFFS_BLOCK_STATE_FULL &&
				    yaffs_block_ok_for_gc(dev, bi)) {
					selected = i;
					prioritised = 1;
				}
			}
			bi++;
		}

		/*
		 * If there is a prioritised block and none was selected then
		 * this happened because there is at least one old dirty block
		 * gumming up the works. Let's gc the oldest dirty block.
		 */

		if (prioritised_exist &&
		    !selected && dev->oldest_dirty_block > 0)
			selected = dev->oldest_dirty_block;

		if (!prioritised_exist)	/* None found, so we can clear this */
			dev->has_pending_prioritised_gc = 0;
	}

	/* If we're doing aggressive GC then we are happy to take a less-dirty
	 * block, and search harder.
	 * else (leasurely gc), then we only bother to do this if the
	 * block has only a few pages in use.
	 */

	if (!selected) {
		u32 pages_used;
		int n_blocks =
		    dev->internal_end_block - dev->internal_start_block + 1;
		if (aggressive) {
			threshold = dev->param.chunks_per_block;
			iterations = n_blocks;
		} else {
			u32 max_threshold;

			if (background)
				max_threshold = dev->param.chunks_per_block / 2;
			else
				max_threshold = dev->param.chunks_per_block / 8;

			if (max_threshold < YAFFS_GC_PASSIVE_THRESHOLD)
				max_threshold = YAFFS_GC_PASSIVE_THRESHOLD;

			threshold = background ? (dev->gc_not_done + 2) * 2 : 0;
			if (threshold < YAFFS_GC_PASSIVE_THRESHOLD)
				threshold = YAFFS_GC_PASSIVE_THRESHOLD;
			if (threshold > max_threshold)
				threshold = max_threshold;

			iterations = n_blocks / 16 + 1;
			if (iterations > 100)
				iterations = 100;
		}

#ifdef GC_Qlearning
	fagc_ok=fagc_check_erase_coldest(dev);
	// if(n_blk_erasure_max-n_blk_erasure_min > 200)
	// {
	// 	printk("\n right now Twl is %d, line %d",Twl, __LINE__);
	// }
	if(fagc_ok)
	{
	// if(n_blk_erasure_max-n_blk_erasure_min > 200)
	// {
	// 	printk("\n right now Twl is %d, line %d",Twl, __LINE__);
	// }
		extern int n_blk_erasures[BLOCK_NUMBER];

		for (i = 1; i <= BLOCK_NUMBER;  i++) {
			dev->gc_block_finder++;
			if (dev->gc_block_finder < dev->internal_start_block ||
			    dev->gc_block_finder > dev->internal_end_block)
				dev->gc_block_finder =
				    dev->internal_start_block;

			bi = yaffs_get_block_info(dev, dev->gc_block_finder);
       
			pages_used = bi->pages_in_use - bi->soft_del_pages;

			if (bi->block_state == YAFFS_BLOCK_STATE_FULL &&
			   n_blk_erasures[ dev->gc_block_finder-1]< min_earase_num/* &&
			    (dev->gc_dirtiest < 1 || pages_used <= dev->gc_pages_in_use)*/ ) 
			 { 
			    min_earase_num = n_blk_erasures[ dev->gc_block_finder-1];
				dev->gc_dirtiest =  dev->gc_block_finder;
				//dev->gc_pages_in_use = pages_used;
			 }
		}

		if (dev->gc_dirtiest > 0)
		{
          selected = dev->gc_dirtiest;	
		}
		else
		{
		    i =dev->gc_dirtiest;
			printk("something wrong here %d and dirti =  %d ",__LINE__,i);
		}
				
		 fagc_update_threshold();
		 
		 /* ========== 磨损均衡统计 ========== */
#ifdef DEBUG_QLGC
		 int wear_gap = n_blk_erasure_max - n_blk_erasure_min;
		 ql_stats.last_wear_gap = wear_gap;
		 if(wear_gap > ql_stats.max_wear_gap) {
		 	ql_stats.max_wear_gap = wear_gap;
		 }
#endif
	}
	else
	{
        now = TIME_NOW;
      	for (i = 0;i < BLOCK_NUMBER;i++) 
		{
			dev->gc_block_finder++;
			if (dev->gc_block_finder < dev->internal_start_block ||
			    dev->gc_block_finder > dev->internal_end_block)
				dev->gc_block_finder =
				    dev->internal_start_block;
			
			bi = yaffs_get_block_info(dev, dev->gc_block_finder);

			pages_used = bi->pages_in_use - bi->soft_del_pages;
			cwps=0;
			if (bi->block_state == YAFFS_BLOCK_STATE_FULL)
			{

		       
				if(pages_used == dev->param.chunks_per_block)
				{
					valid_block_cnt++;
				}
               
              cwps = sum_block[dev->gc_block_finder -1].sum 
                     + sum_block[dev->gc_block_finder -1].cnt * (now - sum_block[dev->gc_block_finder -1].chunk_seq);
               	if(cwps < sum_block[dev->gc_block_finder-1].sum)
				{
    			    printk("overflow happen here %d \r\n",__LINE__);
				}
		    }
			cwps = cwps * (dev->param.chunks_per_block - pages_used)
				/(dev->param.chunks_per_block + pages_used);   //fcy changed
            if(cwps>max_cwps)
            {
               max_cwps = cwps;
			   dev->gc_dirtiest = dev->gc_block_finder;
	    	}
      	}
		if(dev->gc_dirtiest>0)
		{
          selected = dev->gc_dirtiest;
		  fagc_update_alpha(valid_block_cnt);
		}
		else
		{
            printk("can't find victim block! \r\n");
		}
				
	}
#endif


#ifdef GC_RW
		if(1)
		{
			unsigned long rnd;
			extern u32 random32(void);
			extern int n_blk_erasures[BLOCK_NUMBER];
			static int dir[GC_RW_DIRS][2]={{0,1},{-1,0},{0,-1},{1,0}};
			int j,k;
			int cost[GC_RW_DIRS]={0},block[GC_RW_DIRS]={0};
			int row,col;
			int r,c;
			int b;
			int sum;

			rnd=random32();

			if(dev->gc_block_finder==0)
				dev->gc_block_finder=dev->internal_start_block+rnd%(dev->internal_end_block-dev->internal_start_block+1);

			for(j=0;j<GC_RW_DIRS;j++)
			{
				row=(dev->gc_block_finder-1)/COLS;
				col=(dev->gc_block_finder-1)%COLS;
				r=(row+dir[j][0]+ROWS)%ROWS;
				c=(col+dir[j][1]+COLS)%COLS;

				b=r*COLS+c+1;
				bi = yaffs_get_block_info(dev, b);
				if (bi->block_state == YAFFS_BLOCK_STATE_FULL) 
				{
#ifdef GC_RW_ERASURE
					cost[j]=1000-n_blk_erasures[b-1];
#endif
#ifdef GC_RW_PAGES
					pages_used = bi->pages_in_use - bi->soft_del_pages;
					cost[j]=dev->param.chunks_per_block-pages_used;
#endif

#ifdef GC_RW_CAT
					if(1)
					{
						int age;
						unsigned long now,last;
						unsigned long erased_number;

						now=jiffies;
						last=n_blk_age[b-1];	
						if(last==0)
							n_blk_age[b-1]=now;	
						age=gc_cb_get_age((long)now-(long)last);
						pages_used = bi->pages_in_use - bi->soft_del_pages;					

						erased_number=n_blk_erasures[dev->gc_block_finder-1]+1;

						cost[j] = ((dev->param.chunks_per_block-pages_used)*dev->param.chunks_per_block*age*1000/pages_used)/erased_number;
					}
#endif

#ifdef GC_RW_CB
					if(1)
					{
						int age;
						unsigned long now,last;
						now=jiffies;
						last=n_blk_age[b-1];	
						if(last==0)
							n_blk_age[b-1]=now;	
						age=gc_cb_get_age((long)now-(long)last);
						pages_used = bi->pages_in_use - bi->soft_del_pages;
						cost[j] = age*(dev->param.chunks_per_block-pages_used)*dev->param.chunks_per_block*2/(2*pages_used);
					}
#endif
					block[j]=b;
				}
			}
#if 0 /*Random select */
			gc_rw_get_freq(cost); /* added by ed. 2014.04.14 */

			for(i=1;i<GC_RW_DIRS;i++)
				cost[i] += cost[i-1];

			if( cost[GC_RW_DIRS-1]!=0 ) //All is zero.
			{
				rnd=rnd%cost[GC_RW_DIRS-1];
				for(i=0;i<GC_RW_DIRS && rnd>=cost[i];i++);
				dev->gc_block_finder = block[i];
				selected = dev->gc_block_finder;
			}
#endif
#if 1 /* Select max */
			rnd=cost[0];
			selected=block[0];
			dev->gc_block_finder = block[0];
			for(i=1;i<GC_RW_DIRS;i++)
			{
				if(cost[i]>rnd)
				{
					rnd=cost[i];
					dev->gc_block_finder = block[i];
					selected=block[i];
				}
			}
			if(rnd==0)
				selected=0;
#endif
			if(selected==0)
				dev->gc_block_finder=0;
		}
#endif
	}

	/*
	 * If nothing has been selected for a while, try the oldest dirty
	 * because that's gumming up the works.
	 */
#if !defined(YAFFS_GC_ED_MODIFIED) /* Original version. Modified by ed. 2024.04.14 */
	if (!selected && dev->param.is_yaffs2 &&
	    dev->gc_not_done >= (background ? 10 : 20)) {
		yaffs2_find_oldest_dirty_seq(dev);
		if (dev->oldest_dirty_block > 0) {
			selected = dev->oldest_dirty_block;
			dev->gc_dirtiest = selected;
			dev->oldest_dirty_gc_count++;
			bi = yaffs_get_block_info(dev, selected);
			dev->gc_pages_in_use =
			    bi->pages_in_use - bi->soft_del_pages;
		} else {
			dev->gc_not_done = 0;
		}
	}
#endif
	if (selected) {
		yaffs_trace(YAFFS_TRACE_GC,
			"GC Selected block %d with %d free, prioritised:%d",
			selected,
			dev->param.chunks_per_block - dev->gc_pages_in_use,
			prioritised);

		dev->n_gc_blocks++;
		if (background)
			dev->bg_gcs++;

		dev->gc_dirtiest = 0;
		dev->gc_pages_in_use = 0;
		dev->gc_not_done = 0;
		if (dev->refresh_skip > 0)
			dev->refresh_skip--;
	} else {
		dev->gc_not_done++;
		yaffs_trace(YAFFS_TRACE_GC,
			"GC none: finder %d skip %d threshold %d dirtiest %d using %d oldest %d%s",
			dev->gc_block_finder, dev->gc_not_done, threshold,
			dev->gc_dirtiest, dev->gc_pages_in_use,
			dev->oldest_dirty_block, background ? " bg" : "");
	}

	return selected;
}

/* New garbage collector
 * If we're very low on erased blocks then we do aggressive garbage collection
 * otherwise we do "leasurely" garbage collection.
 * Aggressive gc looks further (whole array) and will accept less dirty blocks.
 * Passive gc only inspects smaller areas and only accepts more dirty blocks.
 *
 * The idea is to help clear out space in a more spread-out manner.
 * Dunno if it really does anything useful.
 */
static int yaffs_check_gc(struct yaffs_dev *dev, int background)
{
	int aggressive = 0;
	int gc_ok = YAFFS_OK;
	int max_tries = 0;
	int min_erased;
	int erased_chunks;
	int checkpt_block_adjust;


#ifdef YAFFS_GC_ED_MODIFIED /* added by ed. 2014.04.10 */
	if(!background && !(yaffs_gc_urgency(dev)
#if defined(GC_Qlearning)
	    || fagc_check_erase_coldest(dev)   
#endif
 	  ) )
		return YAFFS_OK;
#endif

	if (dev->param.gc_control_fn &&
		(dev->param.gc_control_fn(dev) & 1) == 0)
		return YAFFS_OK;

	if (dev->gc_disable)
		/* Bail out so we don't get recursive gc */
		return YAFFS_OK;

	/* This loop should pass the first time.
	 * Only loops here if the collection does not increase space.
	 */
//addd by ry 2024.04.30
#ifdef GC_Qlearning
new_start=1;
int inter_copies=0;
#endif
//add end
	do {
		max_tries++;

		checkpt_block_adjust = yaffs_calc_checkpt_blocks_required(dev);

		min_erased =
		    dev->param.n_reserved_blocks + checkpt_block_adjust + 1;
		erased_chunks =
		    dev->n_erased_blocks * dev->param.chunks_per_block;

		/* If we need a block soon then do aggressive gc. */
		if (dev->n_erased_blocks < min_erased)
			aggressive = 1;
		else {
			if (!background
			    && erased_chunks > (dev->n_free_chunks / 4))
				break;

			if (dev->gc_skip > 20)
				dev->gc_skip = 20;
			if (erased_chunks < dev->n_free_chunks / 2 ||
			    dev->gc_skip < 1 || background)
				aggressive = 0;
			else {
				dev->gc_skip--;
				break;
			}
		}

		dev->gc_skip = 5;
#ifdef YAFFS_GC_ED_MODIFIED /* Added by Ed. 2014.03.31 */
		aggressive=1;
#endif

		/* If we don't already have a block being gc'd then see if we
		 * should start another */

		if (dev->gc_block < 1 && !aggressive) {
			dev->gc_block = yaffs2_find_refresh_block(dev);
			dev->gc_chunk = 0;
			dev->n_clean_ups = 0;
		}
		if (dev->gc_block < 1) {
			dev->gc_block =
			    yaffs_find_gc_block(dev, aggressive, background);
			dev->gc_chunk = 0;
			dev->n_clean_ups = 0;
		}

		if (dev->gc_block > 0) {
			dev->all_gcs++;

//add by ry to calculate reward according to number of gc_copies
#if defined(GC_Qlearning)
			struct yaffs_block_info *bi = yaffs_get_block_info(dev, dev->gc_block);
			int number_of_migrations = bi->pages_in_use-bi->soft_del_pages;
			//dev->gc_block即要选择的条目对应块
			if(Qlearning_count)
			{
				unsigned long ry_interval_time=jiffies-bat[dev->gc_block].time;
				bat[dev->gc_block].time=jiffies;
				// int reward=getreward(number_of_migrations,bi->soft_del_pages);
				int reward=getreward(number_of_migrations,bi->soft_del_pages,ry_interval_time);
				
				/* ========== 调试统计更新 ========== */
#ifdef DEBUG_QLGC
				ql_stats.gc_operations++;
				ql_stats.total_migrations += number_of_migrations;
				ql_stats.total_deleted_pages += bi->soft_del_pages;
				ql_stats.total_reward += reward;
				if(gc_ok == YAFFS_OK) ql_stats.gc_success_count++;
				else ql_stats.gc_fail_count++;
#endif

				inter_copies+=number_of_migrations;
				//空间换时间，更高效的查找算法
				current_node=searchInHashTable(dev->gc_block);
				struct HashNode* temp=current_node;
				unsigned num_node=0;
				while(current_node){
					num_node++;
					current_node=current_node->next;
				}
				current_node=temp;//回到头部
				while(current_node){
					// int cur_qvalue=updateQValue_r(QTable,current_node->state,current_node->action,reward);
					int cur_qvalue=updateQValue_r(QTable,current_node->state,current_node->action,reward/num_node);//平分奖励
					if(cur_qvalue<0 || cur_qvalue==2147483647){
						update_to_handle_overflow(QTable,cur_qvalue,current_node->state,current_node->action);//处理溢出影响
					}
					current_node=current_node->next;
				}
				deleteFromHashTable(dev->gc_block);
				
			}
#endif
//add end
			if (!aggressive)
				dev->passive_gc_count++;

			yaffs_trace(YAFFS_TRACE_GC,
				"yaffs: GC n_erased_blocks %d aggressive %d",
				dev->n_erased_blocks, aggressive);

			gc_ok = yaffs_gc_block(dev, dev->gc_block, aggressive,new_start);
			if(!gc_ok){
				printk("error in yaffs_gc_block");
			}
		}
    
		if (dev->n_erased_blocks < (int)dev->param.n_reserved_blocks &&
		    dev->gc_block > 0) {
			yaffs_trace(YAFFS_TRACE_GC,
				"yaffs: GC !!!no reclaim!!! n_erased_blocks %d after try %d block %d",
				dev->n_erased_blocks, max_tries,
				dev->gc_block);
		}
	} while ((dev->n_erased_blocks < (int)dev->param.n_reserved_blocks) &&
		 (dev->gc_block > 0) && (max_tries < 2));

//add by ry 2024.04.30
#if defined(GC_Qlearning)
if(Qlearning_count < num_trainings){
	Qlearning_count++;
}

/* ========== 详细的调试打印 ========== */
#ifdef DEBUG_QLGC
	/* 仅在训练期间进行详细打印 */
	if(Qlearning_count < num_trainings && Qlearning_count % 1000 == 0 && Qlearning_count > 0) {
		/* 1. Q-Learning 训练状态 */
		printk("\n[QL-GC-EPOCH] Training: %u/%u | epsilon: %u | Progress: %u%%\n",
			Qlearning_count, num_trainings, epsilon,
			(100 * Qlearning_count) / num_trainings);
		
		/* 2. 动作选择分布 */
		if(ql_stats.total_actions > 0) {
			printk("[QL-GC-ACTION] Distribution: HOT=%u(%u%%) MEDIUM=%u(%u%%) "
				"COLD=%u(%u%%) SUPER=%u(%u%%)\n",
				ql_stats.action_count[0], 100*ql_stats.action_count[0]/ql_stats.total_actions,
				ql_stats.action_count[1], 100*ql_stats.action_count[1]/ql_stats.total_actions,
				ql_stats.action_count[2], 100*ql_stats.action_count[2]/ql_stats.total_actions,
				ql_stats.action_count[3], 100*ql_stats.action_count[3]/ql_stats.total_actions);
		}
		
		/* 3. 状态空间覆盖率 */
		printk("[QL-GC-STATE] Visited states: %u | State-action pairs: %u | "
			"Q-table utilization: %u%%\n",
			ql_stats.unique_states_visited,
			ql_stats.state_action_pairs,
			(100 * ql_stats.state_action_pairs) / CAPACITY);
		
		/* 4. GC 效率统计 */
		if(ql_stats.gc_operations > 0) {
			u32 avg_migrations = ql_stats.total_migrations / ql_stats.gc_operations;
			u32 recovery_rate = ql_stats.gc_success_count > 0 ? 
				(100 * ql_stats.chunks_recovered_total) / (ql_stats.gc_operations * dev->param.chunks_per_block) : 0;
			printk("[QL-GC-PERF] GC ops: %u | Success rate: %u%% | "
				"Avg migrations: %u | Recovery rate: %u%%\n",
				ql_stats.gc_operations,
				(100 * ql_stats.gc_success_count) / ql_stats.gc_operations,
				avg_migrations,
				recovery_rate);
		}
		
		/* 5. 磨损均衡情况 */
		printk("[QL-GC-WEAR] Erasure gap: current=%d | max=%d | Twl=%d\n",
			ql_stats.last_wear_gap, ql_stats.max_wear_gap, Twl);
		
		/* 6. 哈希表健康度 */
		u32 load_factor = (100 * ql_stats.q_entries_used) / CAPACITY;
		printk("[QL-GC-HASH] Load factor: %u%% | Collisions: %u | Entries: %u/%u\n",
			load_factor,
			ql_stats.hash_collisions,
			ql_stats.q_entries_used, CAPACITY);
	}
#else
	/* 简化版本，只打印关键指标 - 仅训练期间打印 */
	if(Qlearning_count < num_trainings && Qlearning_count % 500 == 0){
		u32 explore_rate = (100 * epsilon) / num_trainings;
		printk("[QL-GC] Trained %d/%d | gc_copies: %d | epsilon: %u (探索: %u%%)\n",
			Qlearning_count, num_trainings, inter_copies, epsilon,
			explore_rate);
	}
	
	/* 训练完成时打印一次最终状态 */
	if(Qlearning_count == num_trainings) {
		printk("\n[QL-GC-FINAL] ===== 训练完成 =====\n");
		printk("[QL-GC-FINAL] Total iterations: %u\n", num_trainings);
		printk("[QL-GC-FINAL] Final epsilon: %u (永久探索: 5%%)\n", MIN_EPSILON);
		printk("[QL-GC-FINAL] Switching to production mode\n");
		printk("[QL-GC-FINAL] ====================\n\n");
	}
#endif

/* 改进 epsilon 衰减策略：
 * 线性衰减从 num_trainings 到 MIN_EPSILON
 * 确保训练完成后保留 MIN_EPSILON 的探索概率（约5%）
 * 这样可以应对运行时的环境变化
 */
#define MIN_EPSILON (num_trainings / 20)  /* 保留5%的探索概率 */

if(Qlearning_count < num_trainings) {
	/* 线性插值：从 num_trainings 衰减到 MIN_EPSILON */
	unsigned decay_range = num_trainings - MIN_EPSILON;
	epsilon = MIN_EPSILON + (decay_range * (num_trainings - Qlearning_count)) / num_trainings;
} else {
	/* 训练完成后，保持 MIN_EPSILON 的探索概率 */
	epsilon = MIN_EPSILON;
}

#endif
//add end

	return aggressive ? gc_ok : YAFFS_OK;
}

/*
 * yaffs_bg_gc()
 * Garbage collects. Intended to be called from a background thread.
 * Returns non-zero if at least half the free chunks are erased.
 */
int yaffs_bg_gc(struct yaffs_dev *dev, unsigned urgency)
{
	int erased_chunks = dev->n_erased_blocks * dev->param.chunks_per_block;

	(void) urgency;
	yaffs_trace(YAFFS_TRACE_BACKGROUND, "Background gc %u", urgency);

	yaffs_check_gc(dev, 0);//modified by hy.change from 1 to 0 2017.5.20
	return erased_chunks > dev->n_free_chunks / 2;
}

/*-------------------- Data file manipulation -----------------*/

static int yaffs_rd_data_obj(struct yaffs_obj *in, int inode_chunk, u8 * buffer)
{
	int nand_chunk = yaffs_find_chunk_in_file(in, inode_chunk, NULL);

	if (nand_chunk >= 0)
		return yaffs_rd_chunk_tags_nand(in->my_dev, nand_chunk,
						buffer, NULL);
	else {
		yaffs_trace(YAFFS_TRACE_NANDACCESS,
			"Chunk %d not found zero instead",
			nand_chunk);
		/* get sane (zero) data if you read a hole */
		memset(buffer, 0, in->my_dev->data_bytes_per_chunk);
		return 0;
	}

}

void yaffs_chunk_del(struct yaffs_dev *dev, int chunk_id, int mark_flash,
		     int lyn)
{
	int block;
	int page;
	struct yaffs_ext_tags tags;
	struct yaffs_block_info *bi;
    int index; 
	unsigned now;
	unsigned tmp;
	(void) lyn;
	if (chunk_id <= 0)
		return;

	dev->n_deletions++;
	block = chunk_id / dev->param.chunks_per_block;
	page = chunk_id % dev->param.chunks_per_block;

	if (!yaffs_check_chunk_bit(dev, block, page))
		yaffs_trace(YAFFS_TRACE_VERIFY,
			"Deleting invalid chunk %d", chunk_id);

	bi = yaffs_get_block_info(dev, block);

	yaffs2_update_oldest_dirty_seq(dev, block, bi);

	yaffs_trace(YAFFS_TRACE_DELETION,
		"line %d delete of chunk %d",
		lyn, chunk_id);

	if (!dev->param.is_yaffs2 && mark_flash &&
	    bi->block_state != YAFFS_BLOCK_STATE_COLLECTING) {

		memset(&tags, 0, sizeof(tags));
		tags.is_deleted = 1;
		yaffs_wr_chunk_tags_nand(dev, chunk_id, NULL, &tags);
		yaffs_handle_chunk_update(dev, chunk_id, &tags);
	} else {
		dev->n_unmarked_deletions++;
	}

	/* Pull out of the management area.
	 * If the whole block became dirty, this will kick off an erasure.
	 */
	if (bi->block_state == YAFFS_BLOCK_STATE_ALLOCATING ||
	    bi->block_state == YAFFS_BLOCK_STATE_FULL ||
	    bi->block_state == YAFFS_BLOCK_STATE_NEEDS_SCAN ||
	    bi->block_state == YAFFS_BLOCK_STATE_COLLECTING) {
		dev->n_free_chunks++;
		yaffs_clear_chunk_bit(dev, block, page);
#if defined(GC_Qlearning)
//index =  (block-1)*dev->param.chunks_per_block + page;
//age_pages[index] = chunk_seq;

now = TIME_NOW;

if(0 == sum_block[block-1].cnt)
{
   // printk("reset is ok for block %d !\r\n",block);
	sum_block[block-1].chunk_seq  = now;
	sum_block[block-1].sum 	= 0;  //gurantee sum get reseted
	sum_block[block-1].cnt 	= 1;
}
else
{
    tmp = sum_block[block-1].sum;
	sum_block[block-1].sum += (sum_block[block-1].cnt * (now - sum_block[block-1].chunk_seq));

	if(sum_block[block-1].sum < tmp)
	{
        printk("overflow happen here %d \r\n",__LINE__);
	}
	
	sum_block[block-1].chunk_seq = now;
	sum_block[block-1].cnt++;
}
#endif

		bi->pages_in_use--;

		if (bi->pages_in_use == 0 &&
		    !bi->has_shrink_hdr &&
		    bi->block_state != YAFFS_BLOCK_STATE_ALLOCATING &&
		    bi->block_state != YAFFS_BLOCK_STATE_NEEDS_SCAN) {
			yaffs_block_became_dirty(dev, block);
		}
	}
}

int yaffs_wr_data_obj(struct yaffs_obj *in, int inode_chunk,
			const u8 *buffer, int n_bytes, int use_reserve)
{
	/* Find old chunk Need to do this to get serial number
	 * Write new one and patch into tree.
	 * Invalidate old tags.
	 */

	int prev_chunk_id;
	struct yaffs_ext_tags prev_tags;
	int new_chunk_id;
	struct yaffs_ext_tags new_tags;
	struct yaffs_dev *dev = in->my_dev;
	loff_t endpos;

	yaffs_check_gc(dev, 0);

	/* Get the previous chunk at this location in the file if it exists.
	 * If it does not exist then put a zero into the tree. This creates
	 * the tnode now, rather than later when it is harder to clean up.
	 */
	prev_chunk_id = yaffs_find_chunk_in_file(in, inode_chunk, &prev_tags);
	if (prev_chunk_id < 1 &&
	    !yaffs_put_chunk_in_file(in, inode_chunk, 0, 0))
		return 0;

	/* Set up new tags */
	memset(&new_tags, 0, sizeof(new_tags));

	new_tags.chunk_id = inode_chunk;
	new_tags.obj_id = in->obj_id;
	new_tags.serial_number =
	    (prev_chunk_id > 0) ? prev_tags.serial_number + 1 : 1;
	new_tags.n_bytes = n_bytes;

	if (n_bytes < 1 || n_bytes > (int)dev->data_bytes_per_chunk) {
		yaffs_trace(YAFFS_TRACE_ERROR,
		  "Writing %d bytes to chunk!!!!!!!!!",
		   n_bytes);
		BUG();
	}

	/*
	 * If this is a data chunk and the write goes past the end of the stored
	 * size then update the stored_size.
	 */
	if (inode_chunk > 0) {
		endpos =  (inode_chunk - 1) * dev->data_bytes_per_chunk +
				n_bytes;
		if (in->variant.file_variant.stored_size < endpos)
			in->variant.file_variant.stored_size = endpos;
	}

	new_chunk_id =
	    yaffs_write_new_chunk(dev, buffer, &new_tags, use_reserve);

	if (new_chunk_id > 0) {
		yaffs_put_chunk_in_file(in, inode_chunk, new_chunk_id, 0);

		if (prev_chunk_id > 0)
			yaffs_chunk_del(dev, prev_chunk_id, 1, __LINE__);

		yaffs_verify_file_sane(in);
	}
	return new_chunk_id;
}



static int yaffs_do_xattrib_mod(struct yaffs_obj *obj, int set,
				const YCHAR *name, const void *value, int size,
				int flags)
{
	struct yaffs_xattr_mod xmod;
	int result;

	xmod.set = set;
	xmod.name = name;
	xmod.data = value;
	xmod.size = size;
	xmod.flags = flags;
	xmod.result = -ENOSPC;

	result = yaffs_update_oh(obj, NULL, 0, 0, 0, &xmod);

	if (result > 0)
		return xmod.result;
	else
		return -ENOSPC;
}

static int yaffs_apply_xattrib_mod(struct yaffs_obj *obj, char *buffer,
				   struct yaffs_xattr_mod *xmod)
{
	int retval = 0;
	int x_offs = sizeof(struct yaffs_obj_hdr);
	struct yaffs_dev *dev = obj->my_dev;
	int x_size = dev->data_bytes_per_chunk - sizeof(struct yaffs_obj_hdr);
	char *x_buffer = buffer + x_offs;

	if (xmod->set)
		retval =
		    nval_set(dev, x_buffer, x_size, xmod->name, xmod->data,
			     xmod->size, xmod->flags);
	else
		retval = nval_del(dev, x_buffer, x_size, xmod->name);

	obj->has_xattr = nval_hasvalues(dev, x_buffer, x_size);
	obj->xattr_known = 1;
	xmod->result = retval;

	return retval;
}

static int yaffs_do_xattrib_fetch(struct yaffs_obj *obj, const YCHAR *name,
				  void *value, int size)
{
	char *buffer = NULL;
	int result;
	struct yaffs_ext_tags tags;
	struct yaffs_dev *dev = obj->my_dev;
	int x_offs = sizeof(struct yaffs_obj_hdr);
	int x_size = dev->data_bytes_per_chunk - sizeof(struct yaffs_obj_hdr);
	char *x_buffer;
	int retval = 0;

	if (obj->hdr_chunk < 1)
		return -ENODATA;

	/* If we know that the object has no xattribs then don't do all the
	 * reading and parsing.
	 */
	if (obj->xattr_known && !obj->has_xattr) {
		if (name)
			return -ENODATA;
		else
			return 0;
	}

	buffer = (char *)yaffs_get_temp_buffer(dev);
	if (!buffer)
		return -ENOMEM;

	result =
	    yaffs_rd_chunk_tags_nand(dev, obj->hdr_chunk, (u8 *) buffer, &tags);

	if (result != YAFFS_OK)
		retval = -ENOENT;
	else {
		x_buffer = buffer + x_offs;

		if (!obj->xattr_known) {
			obj->has_xattr = nval_hasvalues(dev, x_buffer, x_size);
			obj->xattr_known = 1;
		}

		if (name)
			retval = nval_get(dev, x_buffer, x_size,
						name, value, size);
		else
			retval = nval_list(dev, x_buffer, x_size, value, size);
	}
	yaffs_release_temp_buffer(dev, (u8 *) buffer);
	return retval;
}

int yaffs_set_xattrib(struct yaffs_obj *obj, const YCHAR * name,
		      const void *value, int size, int flags)
{
	return yaffs_do_xattrib_mod(obj, 1, name, value, size, flags);
}

int yaffs_remove_xattrib(struct yaffs_obj *obj, const YCHAR * name)
{
	return yaffs_do_xattrib_mod(obj, 0, name, NULL, 0, 0);
}

int yaffs_get_xattrib(struct yaffs_obj *obj, const YCHAR * name, void *value,
		      int size)
{
	return yaffs_do_xattrib_fetch(obj, name, value, size);
}

int yaffs_list_xattrib(struct yaffs_obj *obj, char *buffer, int size)
{
	return yaffs_do_xattrib_fetch(obj, NULL, buffer, size);
}

static void yaffs_check_obj_details_loaded(struct yaffs_obj *in)
{
	u8 *buf;
	struct yaffs_obj_hdr *oh;
	struct yaffs_dev *dev;
	struct yaffs_ext_tags tags;
	int result;

	if (!in || !in->lazy_loaded || in->hdr_chunk < 1)
		return;

	dev = in->my_dev;
	buf = yaffs_get_temp_buffer(dev);

	result = yaffs_rd_chunk_tags_nand(dev, in->hdr_chunk, buf, &tags);

	if (result == YAFFS_FAIL) {
		yaffs_release_temp_buffer(dev, buf);
		return;
	}

	oh = (struct yaffs_obj_hdr *)buf;

	yaffs_do_endian_oh(dev, oh);

	in->lazy_loaded = 0;
	in->yst_mode = oh->yst_mode;
	yaffs_load_attribs(in, oh);
	yaffs_set_obj_name_from_oh(in, oh);

	if (in->variant_type == YAFFS_OBJECT_TYPE_SYMLINK)
		in->variant.symlink_variant.alias =
		    yaffs_clone_str(oh->alias);
	yaffs_release_temp_buffer(dev, buf);
}

/* UpdateObjectHeader updates the header on NAND for an object.
 * If name is not NULL, then that new name is used.
 *
 * We're always creating the obj header from scratch (except reading
 * the old name) so first set up in cpu endianness then run it through
 * endian fixing at the end.
 *
 * However, a twist: If there are xattribs we leave them as they were.
 *
 * Careful! The buffer holds the whole chunk. Part of the chunk holds the
 * object header and the rest holds the xattribs, therefore we use a buffer
 * pointer and an oh pointer to point to the same memory.
 */

int yaffs_update_oh(struct yaffs_obj *in, const YCHAR *name, int force,
		    int is_shrink, int shadows, struct yaffs_xattr_mod *xmod)
{

	struct yaffs_block_info *bi;
	struct yaffs_dev *dev = in->my_dev;
	int prev_chunk_id;
	int ret_val = 0;
	int result = 0;
	int new_chunk_id;
	struct yaffs_ext_tags new_tags;
	struct yaffs_ext_tags old_tags;
	const YCHAR *alias = NULL;
	u8 *buffer = NULL;
	YCHAR old_name[YAFFS_MAX_NAME_LENGTH + 1];
	struct yaffs_obj_hdr *oh = NULL;
	loff_t file_size = 0;

	strcpy(old_name, _Y("silly old name"));

	if (in->fake && in != dev->root_dir && !force && !xmod)
		return ret_val;

	yaffs_check_gc(dev, 0);
	yaffs_check_obj_details_loaded(in);

	buffer = yaffs_get_temp_buffer(in->my_dev);
	oh = (struct yaffs_obj_hdr *)buffer;

	prev_chunk_id = in->hdr_chunk;

	if (prev_chunk_id > 0) {
		/* Access the old obj header just to read the name. */
		result = yaffs_rd_chunk_tags_nand(dev, prev_chunk_id,
						  buffer, &old_tags);
		if (result == YAFFS_OK) {
			yaffs_verify_oh(in, oh, &old_tags, 0);
			memcpy(old_name, oh->name, sizeof(oh->name));

			/*
			* NB We only wipe the object header area because the rest of
			* the buffer might contain xattribs.
			*/
			memset(oh, 0xff, sizeof(*oh));
		}
	} else {
		memset(buffer, 0xff, dev->data_bytes_per_chunk);
	}

	oh->type = in->variant_type;
	oh->yst_mode = in->yst_mode;
	oh->shadows_obj = oh->inband_shadowed_obj_id = shadows;

	yaffs_load_attribs_oh(oh, in);

	if (in->parent)
		oh->parent_obj_id = in->parent->obj_id;
	else
		oh->parent_obj_id = 0;

	if (name && *name) {
		memset(oh->name, 0, sizeof(oh->name));
		yaffs_load_oh_from_name(dev, oh->name, name);
	} else if (prev_chunk_id > 0) {
		memcpy(oh->name, old_name, sizeof(oh->name));
	} else {
		memset(oh->name, 0, sizeof(oh->name));
	}

	oh->is_shrink = is_shrink;

	switch (in->variant_type) {
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		/* Should not happen */
		break;
	case YAFFS_OBJECT_TYPE_FILE:
		if (oh->parent_obj_id != YAFFS_OBJECTID_DELETED &&
		    oh->parent_obj_id != YAFFS_OBJECTID_UNLINKED)
			file_size = in->variant.file_variant.stored_size;
		yaffs_oh_size_load(dev, oh, file_size, 0);
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		oh->equiv_id = in->variant.hardlink_variant.equiv_id;
		break;
	case YAFFS_OBJECT_TYPE_SPECIAL:
		/* Do nothing */
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		/* Do nothing */
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		alias = in->variant.symlink_variant.alias;
		if (!alias)
			alias = _Y("no alias");
		strncpy(oh->alias, alias, YAFFS_MAX_ALIAS_LENGTH);
		oh->alias[YAFFS_MAX_ALIAS_LENGTH] = 0;
		break;
	}

	/* process any xattrib modifications */
	if (xmod)
		yaffs_apply_xattrib_mod(in, (char *)buffer, xmod);

	/* Tags */
	memset(&new_tags, 0, sizeof(new_tags));
	in->serial++;
	new_tags.chunk_id = 0;
	new_tags.obj_id = in->obj_id;
	new_tags.serial_number = in->serial;

	/* Add extra info for file header */
	new_tags.extra_available = 1;
	new_tags.extra_parent_id = oh->parent_obj_id;
	new_tags.extra_file_size = file_size;
	new_tags.extra_is_shrink = oh->is_shrink;
	new_tags.extra_equiv_id = oh->equiv_id;
	new_tags.extra_shadows = (oh->shadows_obj > 0) ? 1 : 0;
	new_tags.extra_obj_type = in->variant_type;

	/* Now endian swizzle the oh if needed. */
	yaffs_do_endian_oh(dev, oh);

	yaffs_verify_oh(in, oh, &new_tags, 1);

	/* Create new chunk in NAND */
	new_chunk_id =
	    yaffs_write_new_chunk(dev, buffer, &new_tags,
				  (prev_chunk_id > 0) ? 1 : 0);

	if (buffer)
		yaffs_release_temp_buffer(dev, buffer);

	if (new_chunk_id < 0)
		return new_chunk_id;

	in->hdr_chunk = new_chunk_id;

	if (prev_chunk_id > 0)
		yaffs_chunk_del(dev, prev_chunk_id, 1, __LINE__);

	if (!yaffs_obj_cache_dirty(in))
		in->dirty = 0;

	/* If this was a shrink, then mark the block
	 * that the chunk lives on */
	if (is_shrink) {
		bi = yaffs_get_block_info(in->my_dev,
					  new_chunk_id /
					  in->my_dev->param.chunks_per_block);
		bi->has_shrink_hdr = 1;
	}

	return new_chunk_id;
}

/*--------------------- File read/write ------------------------
 * Read and write have very similar structures.
 * In general the read/write has three parts to it
 * An incomplete chunk to start with (if the read/write is not chunk-aligned)
 * Some complete chunks
 * An incomplete chunk to end off with
 *
 * Curve-balls: the first chunk might also be the last chunk.
 */

int yaffs_file_rd(struct yaffs_obj *in, u8 * buffer, loff_t offset, int n_bytes)
{
	int chunk;
	u32 start;
	int n_copy;
	int n = n_bytes;
	int n_done = 0;
	struct yaffs_cache *cache;
	struct yaffs_dev *dev;

	dev = in->my_dev;

	while (n > 0) {
		yaffs_addr_to_chunk(dev, offset, &chunk, &start);
		chunk++;

		/* OK now check for the curveball where the start and end are in
		 * the same chunk.
		 */
		if ((start + n) < dev->data_bytes_per_chunk)
			n_copy = n;
		else
			n_copy = dev->data_bytes_per_chunk - start;

		cache = yaffs_find_chunk_cache(in, chunk);

		/* If the chunk is already in the cache or it is less than
		 * a whole chunk or we're using inband tags then use the cache
		 * (if there is caching) else bypass the cache.
		 */
		if (cache || n_copy != (int)dev->data_bytes_per_chunk ||
		    dev->param.inband_tags) {
			if (dev->param.n_caches > 0) {

				/* If we can't find the data in the cache,
				 * then load it up. */

				if (!cache) {
					cache =
					    yaffs_grab_chunk_cache(in->my_dev);
					cache->object = in;
					cache->chunk_id = chunk;
					cache->dirty = 0;
					cache->locked = 0;
					yaffs_rd_data_obj(in, chunk,
							  cache->data);
					cache->n_bytes = 0;
				}

				yaffs_use_cache(dev, cache, 0);

				cache->locked = 1;

				memcpy(buffer, &cache->data[start], n_copy);

				cache->locked = 0;
			} else {
				/* Read into the local buffer then copy.. */

				u8 *local_buffer =
				    yaffs_get_temp_buffer(dev);
				yaffs_rd_data_obj(in, chunk, local_buffer);

				memcpy(buffer, &local_buffer[start], n_copy);

				yaffs_release_temp_buffer(dev, local_buffer);
			}
		} else {
			/* A full chunk. Read directly into the buffer. */
			yaffs_rd_data_obj(in, chunk, buffer);
		}
		n -= n_copy;
		offset += n_copy;
		buffer += n_copy;
		n_done += n_copy;
	}
	return n_done;
}

int yaffs_do_file_wr(struct yaffs_obj *in, const u8 *buffer, loff_t offset,
		     int n_bytes, int write_through)
{

	int chunk;
	u32 start;
	int n_copy;
	int n = n_bytes;
	int n_done = 0;
	int n_writeback;
	loff_t start_write = offset;
	int chunk_written = 0;
	u32 n_bytes_read;
	loff_t chunk_start;
	struct yaffs_dev *dev;

	dev = in->my_dev;

	while (n > 0 && chunk_written >= 0) {
		yaffs_addr_to_chunk(dev, offset, &chunk, &start);

		if (((loff_t)chunk) *
		    dev->data_bytes_per_chunk + start != offset ||
		    start >= dev->data_bytes_per_chunk) {
			yaffs_trace(YAFFS_TRACE_ERROR,
				"AddrToChunk of offset %lld gives chunk %d start %d",
				(long  long)offset, chunk, start);
		}
		chunk++;	/* File pos to chunk in file offset */

		/* OK now check for the curveball where the start and end are in
		 * the same chunk.
		 */

		if ((start + n) < dev->data_bytes_per_chunk) {
			n_copy = n;

			/* Now calculate how many bytes to write back....
			 * If we're overwriting and not writing to then end of
			 * file then we need to write back as much as was there
			 * before.
			 */

			chunk_start = (((loff_t)(chunk - 1)) *
					dev->data_bytes_per_chunk);

			if (chunk_start > in->variant.file_variant.file_size)
				n_bytes_read = 0;	/* Past end of file */
			else
				n_bytes_read =
				    in->variant.file_variant.file_size -
				    chunk_start;

			if (n_bytes_read > dev->data_bytes_per_chunk)
				n_bytes_read = dev->data_bytes_per_chunk;

			n_writeback =
			    (n_bytes_read >
			     (start + n)) ? n_bytes_read : (start + n);

			if (n_writeback < 0 ||
			    n_writeback > (int)dev->data_bytes_per_chunk)
				BUG();

		} else {
			n_copy = dev->data_bytes_per_chunk - start;
			n_writeback = dev->data_bytes_per_chunk;
		}

		if (n_copy != (int)dev->data_bytes_per_chunk ||
		    !dev->param.cache_bypass_aligned ||
		    dev->param.inband_tags) {
			/* An incomplete start or end chunk (or maybe both
			 * start and end chunk), or we're using inband tags,
			 * or we're forcing writes through the cache,
			 * so we want to use the cache buffers.
			 */
			if (dev->param.n_caches > 0) {
				struct yaffs_cache *cache;

				/* If we can't find the data in the cache, then
				 * load the cache */
				cache = yaffs_find_chunk_cache(in, chunk);

				if (!cache &&
				    yaffs_check_alloc_available(dev, 1)) {
					cache = yaffs_grab_chunk_cache(dev);
					cache->object = in;
					cache->chunk_id = chunk;
					cache->dirty = 0;
					cache->locked = 0;
					yaffs_rd_data_obj(in, chunk,
							  cache->data);
				} else if (cache &&
					   !cache->dirty &&
					   !yaffs_check_alloc_available(dev,
									1)) {
					/* Drop the cache if it was a read cache
					 * item and no space check has been made
					 * for it.
					 */
					cache = NULL;
				}

				if (cache) {
					yaffs_use_cache(dev, cache, 1);
					cache->locked = 1;

					memcpy(&cache->data[start], buffer,
					       n_copy);

					cache->locked = 0;
					cache->n_bytes = n_writeback;

					if (write_through) {
						chunk_written =
						    yaffs_wr_data_obj
						    (cache->object,
						     cache->chunk_id,
						     cache->data,
						     cache->n_bytes, 1);
						cache->dirty = 0;
					}
				} else {
					chunk_written = -1;	/* fail write */
				}
			} else {
				/* An incomplete start or end chunk (or maybe
				 * both start and end chunk). Read into the
				 * local buffer then copy over and write back.
				 */

				u8 *local_buffer = yaffs_get_temp_buffer(dev);

				yaffs_rd_data_obj(in, chunk, local_buffer);
				memcpy(&local_buffer[start], buffer, n_copy);

				chunk_written =
				    yaffs_wr_data_obj(in, chunk,
						      local_buffer,
						      n_writeback, 0);

				yaffs_release_temp_buffer(dev, local_buffer);
			}
		} else {
			/* A full chunk. Write directly from the buffer. */

			chunk_written =
			    yaffs_wr_data_obj(in, chunk, buffer,
					      dev->data_bytes_per_chunk, 0);

			/* Since we've overwritten the cached data,
			 * we better invalidate it. */
			yaffs_invalidate_chunk_cache(in, chunk);
		}

		if (chunk_written >= 0) {
			n -= n_copy;
			offset += n_copy;
			buffer += n_copy;
			n_done += n_copy;
		}
	}

	/* Update file object */

	if ((start_write + n_done) > in->variant.file_variant.file_size)
		in->variant.file_variant.file_size = (start_write + n_done);

	in->dirty = 1;
	return n_done;
}

int yaffs_wr_file(struct yaffs_obj *in, const u8 *buffer, loff_t offset,
		  int n_bytes, int write_through)
{
	yaffs2_handle_hole(in, offset);
	return yaffs_do_file_wr(in, buffer, offset, n_bytes, write_through);
}

/* ---------------------- File resizing stuff ------------------ */

static void yaffs_prune_chunks(struct yaffs_obj *in, loff_t new_size)
{

	struct yaffs_dev *dev = in->my_dev;
	loff_t old_size = in->variant.file_variant.file_size;
	int i;
	int chunk_id;
	u32 dummy;
	int last_del;
	int start_del;

	if (old_size > 0)
		yaffs_addr_to_chunk(dev, old_size - 1, &last_del, &dummy);
	else
		last_del = 0;

	yaffs_addr_to_chunk(dev, new_size + dev->data_bytes_per_chunk - 1,
				&start_del, &dummy);
	last_del++;
	start_del++;

	/* Delete backwards so that we don't end up with holes if
	 * power is lost part-way through the operation.
	 */
	for (i = last_del; i >= start_del; i--) {
		/* NB this could be optimised somewhat,
		 * eg. could retrieve the tags and write them without
		 * using yaffs_chunk_del
		 */

		chunk_id = yaffs_find_del_file_chunk(in, i, NULL);

		if (chunk_id < 1)
			continue;

		if ((u32)chunk_id <
		    (dev->internal_start_block * dev->param.chunks_per_block) ||
		    (u32)chunk_id >=
		    ((dev->internal_end_block + 1) *
		      dev->param.chunks_per_block)) {
			yaffs_trace(YAFFS_TRACE_ALWAYS,
				"Found daft chunk_id %d for %d",
				chunk_id, i);
		} else {
			in->n_data_chunks--;
			yaffs_chunk_del(dev, chunk_id, 1, __LINE__);
		}
	}
}

void yaffs_resize_file_down(struct yaffs_obj *obj, loff_t new_size)
{
	int new_full;
	u32 new_partial;
	struct yaffs_dev *dev = obj->my_dev;

	yaffs_addr_to_chunk(dev, new_size, &new_full, &new_partial);

	yaffs_prune_chunks(obj, new_size);

	if (new_partial != 0) {
		int last_chunk = 1 + new_full;
		u8 *local_buffer = yaffs_get_temp_buffer(dev);

		/* Rewrite the last chunk with its new size and zero pad */
		yaffs_rd_data_obj(obj, last_chunk, local_buffer);
		memset(local_buffer + new_partial, 0,
		       dev->data_bytes_per_chunk - new_partial);

		yaffs_wr_data_obj(obj, last_chunk, local_buffer,
				  new_partial, 1);

		yaffs_release_temp_buffer(dev, local_buffer);
	}

	obj->variant.file_variant.file_size = new_size;
	obj->variant.file_variant.stored_size = new_size;

	yaffs_prune_tree(dev, &obj->variant.file_variant);
}

int yaffs_resize_file(struct yaffs_obj *in, loff_t new_size)
{
	struct yaffs_dev *dev = in->my_dev;
	loff_t old_size = in->variant.file_variant.file_size;

	yaffs_flush_file_cache(in, 1);
	yaffs_invalidate_file_cache(in);

	yaffs_check_gc(dev, 0);

	if (in->variant_type != YAFFS_OBJECT_TYPE_FILE)
		return YAFFS_FAIL;

	if (new_size == old_size)
		return YAFFS_OK;

	if (new_size > old_size) {
		yaffs2_handle_hole(in, new_size);
		in->variant.file_variant.file_size = new_size;
	} else {
		/* new_size < old_size */
		yaffs_resize_file_down(in, new_size);
	}

	/* Write a new object header to reflect the resize.
	 * show we've shrunk the file, if need be
	 * Do this only if the file is not in the deleted directories
	 * and is not shadowed.
	 */
	if (in->parent &&
	    !in->is_shadowed &&
	    in->parent->obj_id != YAFFS_OBJECTID_UNLINKED &&
	    in->parent->obj_id != YAFFS_OBJECTID_DELETED)
		yaffs_update_oh(in, NULL, 0, 0, 0, NULL);

	return YAFFS_OK;
}

int yaffs_flush_file(struct yaffs_obj *in,
		     int update_time,
		     int data_sync,
		     int discard_cache)
{
	if (!in->dirty)
		return YAFFS_OK;

	yaffs_flush_file_cache(in, discard_cache);

	if (data_sync)
		return YAFFS_OK;

	if (update_time)
		yaffs_load_current_time(in, 0, 0);

	return (yaffs_update_oh(in, NULL, 0, 0, 0, NULL) >= 0) ?
				YAFFS_OK : YAFFS_FAIL;
}


/* yaffs_del_file deletes the whole file data
 * and the inode associated with the file.
 * It does not delete the links associated with the file.
 */
static int yaffs_unlink_file_if_needed(struct yaffs_obj *in)
{
	int ret_val;
	int del_now = 0;
	struct yaffs_dev *dev = in->my_dev;

	if (!in->my_inode)
		del_now = 1;

	if (del_now) {
		ret_val =
		    yaffs_change_obj_name(in, in->my_dev->del_dir,
					  _Y("deleted"), 0, 0);
		yaffs_trace(YAFFS_TRACE_TRACING,
			"yaffs: immediate deletion of file %d",
			in->obj_id);
		in->deleted = 1;
		in->my_dev->n_deleted_files++;
		if (dev->param.disable_soft_del || dev->param.is_yaffs2)
			yaffs_resize_file(in, 0);
		yaffs_soft_del_file(in);
	} else {
		ret_val =
		    yaffs_change_obj_name(in, in->my_dev->unlinked_dir,
					  _Y("unlinked"), 0, 0);
	}
	return ret_val;
}

static int yaffs_del_file(struct yaffs_obj *in)
{
	int ret_val = YAFFS_OK;
	int deleted;	/* Need to cache value on stack if in is freed */
	struct yaffs_dev *dev = in->my_dev;

	if (dev->param.disable_soft_del || dev->param.is_yaffs2)
		yaffs_resize_file(in, 0);

	if (in->n_data_chunks > 0) {
		/* Use soft deletion if there is data in the file.
		 * That won't be the case if it has been resized to zero.
		 */
		if (!in->unlinked)
			ret_val = yaffs_unlink_file_if_needed(in);

		deleted = in->deleted;

		if (ret_val == YAFFS_OK && in->unlinked && !in->deleted) {
			in->deleted = 1;
			deleted = 1;
			in->my_dev->n_deleted_files++;
			yaffs_soft_del_file(in);
		}
		return deleted ? YAFFS_OK : YAFFS_FAIL;
	} else {
		/* The file has no data chunks so we toss it immediately */
		yaffs_free_tnode(in->my_dev, in->variant.file_variant.top);
		in->variant.file_variant.top = NULL;
		yaffs_generic_obj_del(in);

		return YAFFS_OK;
	}
}

int yaffs_is_non_empty_dir(struct yaffs_obj *obj)
{
	return (obj &&
		obj->variant_type == YAFFS_OBJECT_TYPE_DIRECTORY) &&
		!(list_empty(&obj->variant.dir_variant.children));
}

static int yaffs_del_dir(struct yaffs_obj *obj)
{
	/* First check that the directory is empty. */
	if (yaffs_is_non_empty_dir(obj))
		return YAFFS_FAIL;

	return yaffs_generic_obj_del(obj);
}

static int yaffs_del_symlink(struct yaffs_obj *in)
{
	kfree(in->variant.symlink_variant.alias);
	in->variant.symlink_variant.alias = NULL;

	return yaffs_generic_obj_del(in);
}

static int yaffs_del_link(struct yaffs_obj *in)
{
	/* remove this hardlink from the list associated with the equivalent
	 * object
	 */
	list_del_init(&in->hard_links);
	return yaffs_generic_obj_del(in);
}

int yaffs_del_obj(struct yaffs_obj *obj)
{
	int ret_val = -1;

	switch (obj->variant_type) {
	case YAFFS_OBJECT_TYPE_FILE:
		ret_val = yaffs_del_file(obj);
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		if (!list_empty(&obj->variant.dir_variant.dirty)) {
			yaffs_trace(YAFFS_TRACE_BACKGROUND,
				"Remove object %d from dirty directories",
				obj->obj_id);
			list_del_init(&obj->variant.dir_variant.dirty);
		}
		return yaffs_del_dir(obj);
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		ret_val = yaffs_del_symlink(obj);
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		ret_val = yaffs_del_link(obj);
		break;
	case YAFFS_OBJECT_TYPE_SPECIAL:
		ret_val = yaffs_generic_obj_del(obj);
		break;
	case YAFFS_OBJECT_TYPE_UNKNOWN:
		ret_val = 0;
		break;		/* should not happen. */
	}
	return ret_val;
}


static void yaffs_empty_dir_to_dir(struct yaffs_obj *from_dir,
				   struct yaffs_obj *to_dir)
{
	struct yaffs_obj *obj;
	struct list_head *lh;
	struct list_head *n;

	list_for_each_safe(lh, n, &from_dir->variant.dir_variant.children) {
		obj = list_entry(lh, struct yaffs_obj, siblings);
		yaffs_add_obj_to_dir(to_dir, obj);
	}
}

struct yaffs_obj *yaffs_retype_obj(struct yaffs_obj *obj,
				   enum yaffs_obj_type type)
{
	/* Tear down the old variant */
	switch (obj->variant_type) {
	case YAFFS_OBJECT_TYPE_FILE:
		/* Nuke file data */
		yaffs_resize_file(obj, 0);
		yaffs_free_tnode(obj->my_dev, obj->variant.file_variant.top);
		obj->variant.file_variant.top = NULL;
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		/* Put the children in lost and found. */
		yaffs_empty_dir_to_dir(obj, obj->my_dev->lost_n_found);
		if (!list_empty(&obj->variant.dir_variant.dirty))
			list_del_init(&obj->variant.dir_variant.dirty);
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		/* Nuke symplink data */
		kfree(obj->variant.symlink_variant.alias);
		obj->variant.symlink_variant.alias = NULL;
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		list_del_init(&obj->hard_links);
		break;
	default:
		break;
	}

	memset(&obj->variant, 0, sizeof(obj->variant));

	/*Set up new variant if the memset is not enough. */
	switch (type) {
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		INIT_LIST_HEAD(&obj->variant.dir_variant.children);
		INIT_LIST_HEAD(&obj->variant.dir_variant.dirty);
		break;
	case YAFFS_OBJECT_TYPE_FILE:
	case YAFFS_OBJECT_TYPE_SYMLINK:
	case YAFFS_OBJECT_TYPE_HARDLINK:
	default:
		break;
	}

	obj->variant_type = type;

	return obj;

}

static int yaffs_unlink_worker(struct yaffs_obj *obj)
{
	int del_now = 0;

	if (!obj)
		return YAFFS_FAIL;

	if (!obj->my_inode)
		del_now = 1;

	yaffs_update_parent(obj->parent);

	if (obj->variant_type == YAFFS_OBJECT_TYPE_HARDLINK) {
		return yaffs_del_link(obj);
	} else if (!list_empty(&obj->hard_links)) {
		/* Curve ball: We're unlinking an object that has a hardlink.
		 *
		 * This problem arises because we are not strictly following
		 * The Linux link/inode model.
		 *
		 * We can't really delete the object.
		 * Instead, we do the following:
		 * - Select a hardlink.
		 * - Unhook it from the hard links
		 * - Move it from its parent directory so that the rename works.
		 * - Rename the object to the hardlink's name.
		 * - Delete the hardlink
		 */

		struct yaffs_obj *hl;
		struct yaffs_obj *parent;
		int ret_val;
		YCHAR name[YAFFS_MAX_NAME_LENGTH + 1];

		hl = list_entry(obj->hard_links.next, struct yaffs_obj,
				hard_links);

		yaffs_get_obj_name(hl, name, YAFFS_MAX_NAME_LENGTH + 1);
		parent = hl->parent;

		list_del_init(&hl->hard_links);

		yaffs_add_obj_to_dir(obj->my_dev->unlinked_dir, hl);

		ret_val = yaffs_change_obj_name(obj, parent, name, 0, 0);

		if (ret_val == YAFFS_OK)
			ret_val = yaffs_generic_obj_del(hl);

		return ret_val;

	} else if (del_now) {
		switch (obj->variant_type) {
		case YAFFS_OBJECT_TYPE_FILE:
			return yaffs_del_file(obj);
			break;
		case YAFFS_OBJECT_TYPE_DIRECTORY:
			list_del_init(&obj->variant.dir_variant.dirty);
			return yaffs_del_dir(obj);
			break;
		case YAFFS_OBJECT_TYPE_SYMLINK:
			return yaffs_del_symlink(obj);
			break;
		case YAFFS_OBJECT_TYPE_SPECIAL:
			return yaffs_generic_obj_del(obj);
			break;
		case YAFFS_OBJECT_TYPE_HARDLINK:
		case YAFFS_OBJECT_TYPE_UNKNOWN:
		default:
			return YAFFS_FAIL;
		}
	} else if (yaffs_is_non_empty_dir(obj)) {
		return YAFFS_FAIL;
	} else {
		return yaffs_change_obj_name(obj, obj->my_dev->unlinked_dir,
						_Y("unlinked"), 0, 0);
	}
}

int yaffs_unlink_obj(struct yaffs_obj *obj)
{
	if (obj && obj->unlink_allowed)
		return yaffs_unlink_worker(obj);

	return YAFFS_FAIL;
}

int yaffs_unlinker(struct yaffs_obj *dir, const YCHAR *name)
{
	struct yaffs_obj *obj;

	obj = yaffs_find_by_name(dir, name);
	return yaffs_unlink_obj(obj);
}

/* Note:
 * If old_name is NULL then we take old_dir as the object to be renamed.
 */
int yaffs_rename_obj(struct yaffs_obj *old_dir, const YCHAR *old_name,
		     struct yaffs_obj *new_dir, const YCHAR *new_name)
{
	struct yaffs_obj *obj = NULL;
	struct yaffs_obj *existing_target = NULL;
	int force = 0;
	int result;
	struct yaffs_dev *dev;

	if (!old_dir || old_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		BUG();
		return YAFFS_FAIL;
	}
	if (!new_dir || new_dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		BUG();
		return YAFFS_FAIL;
	}

	dev = old_dir->my_dev;

#ifdef CONFIG_YAFFS_CASE_INSENSITIVE
	/* Special case for case insemsitive systems.
	 * While look-up is case insensitive, the name isn't.
	 * Therefore we might want to change x.txt to X.txt
	 */
	if (old_dir == new_dir &&
		old_name && new_name &&
		strcmp(old_name, new_name) == 0)
		force = 1;
#endif

	if (strnlen(new_name, YAFFS_MAX_NAME_LENGTH + 1) >
	    YAFFS_MAX_NAME_LENGTH)
		/* ENAMETOOLONG */
		return YAFFS_FAIL;

	if (old_name)
		obj = yaffs_find_by_name(old_dir, old_name);
	else{
		obj = old_dir;
		old_dir = obj->parent;
	}

	if (obj && obj->rename_allowed) {
		/* Now handle an existing target, if there is one */
		existing_target = yaffs_find_by_name(new_dir, new_name);
		if (yaffs_is_non_empty_dir(existing_target)) {
			return YAFFS_FAIL;	/* ENOTEMPTY */
		} else if (existing_target && existing_target != obj) {
			/* Nuke the target first, using shadowing,
			 * but only if it isn't the same object.
			 *
			 * Note we must disable gc here otherwise it can mess
			 * up the shadowing.
			 *
			 */
			dev->gc_disable = 1;
			yaffs_change_obj_name(obj, new_dir, new_name, force,
					      existing_target->obj_id);
			existing_target->is_shadowed = 1;
			yaffs_unlink_obj(existing_target);
			dev->gc_disable = 0;
		}

		result = yaffs_change_obj_name(obj, new_dir, new_name, 1, 0);

		yaffs_update_parent(old_dir);
		if (new_dir != old_dir)
			yaffs_update_parent(new_dir);

		return result;
	}
	return YAFFS_FAIL;
}

/*----------------------- Initialisation Scanning ---------------------- */

void yaffs_handle_shadowed_obj(struct yaffs_dev *dev, int obj_id,
			       int backward_scanning)
{
	struct yaffs_obj *obj;

	if (backward_scanning) {
		/* Handle YAFFS2 case (backward scanning)
		 * If the shadowed object exists then ignore.
		 */
		obj = yaffs_find_by_number(dev, obj_id);
		if (obj)
			return;
	}

	/* Let's create it (if it does not exist) assuming it is a file so that
	 * it can do shrinking etc.
	 * We put it in unlinked dir to be cleaned up after the scanning
	 */
	obj =
	    yaffs_find_or_create_by_number(dev, obj_id, YAFFS_OBJECT_TYPE_FILE);
	if (!obj)
		return;
	obj->is_shadowed = 1;
	yaffs_add_obj_to_dir(dev->unlinked_dir, obj);
	obj->variant.file_variant.shrink_size = 0;
	obj->valid = 1;		/* So that we don't read any other info. */
}

void yaffs_link_fixup(struct yaffs_dev *dev, struct list_head *hard_list)
{
	struct list_head *lh;
	struct list_head *save;
	struct yaffs_obj *hl;
	struct yaffs_obj *in;

	list_for_each_safe(lh, save, hard_list) {
		hl = list_entry(lh, struct yaffs_obj, hard_links);
		in = yaffs_find_by_number(dev,
					hl->variant.hardlink_variant.equiv_id);

		if (in) {
			/* Add the hardlink pointers */
			hl->variant.hardlink_variant.equiv_obj = in;
			list_add(&hl->hard_links, &in->hard_links);
		} else {
			/* Todo Need to report/handle this better.
			 * Got a problem... hardlink to a non-existant object
			 */
			hl->variant.hardlink_variant.equiv_obj = NULL;
			INIT_LIST_HEAD(&hl->hard_links);
		}
	}
}

static void yaffs_strip_deleted_objs(struct yaffs_dev *dev)
{
	/*
	 *  Sort out state of unlinked and deleted objects after scanning.
	 */
	struct list_head *i;
	struct list_head *n;
	struct yaffs_obj *l;

	if (dev->read_only)
		return;

	/* Soft delete all the unlinked files */
	list_for_each_safe(i, n,
			   &dev->unlinked_dir->variant.dir_variant.children) {
		l = list_entry(i, struct yaffs_obj, siblings);
		yaffs_del_obj(l);
	}

	list_for_each_safe(i, n, &dev->del_dir->variant.dir_variant.children) {
		l = list_entry(i, struct yaffs_obj, siblings);
		yaffs_del_obj(l);
	}
}

/*
 * This code iterates through all the objects making sure that they are rooted.
 * Any unrooted objects are re-rooted in lost+found.
 * An object needs to be in one of:
 * - Directly under deleted, unlinked
 * - Directly or indirectly under root.
 *
 * Note:
 *  This code assumes that we don't ever change the current relationships
 *  between directories:
 *   root_dir->parent == unlinked_dir->parent == del_dir->parent == NULL
 *   lost-n-found->parent == root_dir
 *
 * This fixes the problem where directories might have inadvertently been
 * deleted leaving the object "hanging" without being rooted in the
 * directory tree.
 */

static int yaffs_has_null_parent(struct yaffs_dev *dev, struct yaffs_obj *obj)
{
	return (obj == dev->del_dir ||
		obj == dev->unlinked_dir || obj == dev->root_dir);
}

static void yaffs_fix_hanging_objs(struct yaffs_dev *dev)
{
	struct yaffs_obj *obj;
	struct yaffs_obj *parent;
	int i;
	struct list_head *lh;
	struct list_head *n;
	int depth_limit;
	int hanging;

	if (dev->read_only)
		return;

	/* Iterate through the objects in each hash entry,
	 * looking at each object.
	 * Make sure it is rooted.
	 */

	for (i = 0; i < YAFFS_NOBJECT_BUCKETS; i++) {
		list_for_each_safe(lh, n, &dev->obj_bucket[i].list) {
			obj = list_entry(lh, struct yaffs_obj, hash_link);
			parent = obj->parent;

			if (yaffs_has_null_parent(dev, obj)) {
				/* These directories are not hanging */
				hanging = 0;
			} else if (!parent ||
				   parent->variant_type !=
				   YAFFS_OBJECT_TYPE_DIRECTORY) {
				hanging = 1;
			} else if (yaffs_has_null_parent(dev, parent)) {
				hanging = 0;
			} else {
				/*
				 * Need to follow the parent chain to
				 * see if it is hanging.
				 */
				hanging = 0;
				depth_limit = 100;

				while (parent != dev->root_dir &&
				       parent->parent &&
				       parent->parent->variant_type ==
				       YAFFS_OBJECT_TYPE_DIRECTORY &&
				       depth_limit > 0) {
					parent = parent->parent;
					depth_limit--;
				}
				if (parent != dev->root_dir)
					hanging = 1;
			}
			if (hanging) {
				yaffs_trace(YAFFS_TRACE_SCAN,
					"Hanging object %d moved to lost and found",
					obj->obj_id);
				yaffs_add_obj_to_dir(dev->lost_n_found, obj);
			}
		}
	}
}

/*
 * Delete directory contents for cleaning up lost and found.
 */
static void yaffs_del_dir_contents(struct yaffs_obj *dir)
{
	struct yaffs_obj *obj;
	struct list_head *lh;
	struct list_head *n;

	if (dir->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY)
		BUG();

	list_for_each_safe(lh, n, &dir->variant.dir_variant.children) {
		obj = list_entry(lh, struct yaffs_obj, siblings);
		if (obj->variant_type == YAFFS_OBJECT_TYPE_DIRECTORY)
			yaffs_del_dir_contents(obj);
		yaffs_trace(YAFFS_TRACE_SCAN,
			"Deleting lost_found object %d",
			obj->obj_id);
		yaffs_unlink_obj(obj);
	}
}

static void yaffs_empty_l_n_f(struct yaffs_dev *dev)
{
	yaffs_del_dir_contents(dev->lost_n_found);
}


struct yaffs_obj *yaffs_find_by_name(struct yaffs_obj *directory,
				     const YCHAR *name)
{
	int sum;
	struct list_head *i;
	YCHAR buffer[YAFFS_MAX_NAME_LENGTH + 1];
	struct yaffs_obj *l;

	if (!name)
		return NULL;

	if (!directory) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: yaffs_find_by_name: null pointer directory"
			);
		BUG();
		return NULL;
	}
	if (directory->variant_type != YAFFS_OBJECT_TYPE_DIRECTORY) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"tragedy: yaffs_find_by_name: non-directory"
			);
		BUG();
	}

	sum = yaffs_calc_name_sum(name);

	list_for_each(i, &directory->variant.dir_variant.children) {
		l = list_entry(i, struct yaffs_obj, siblings);

		if (l->parent != directory)
			BUG();

		yaffs_check_obj_details_loaded(l);

		/* Special case for lost-n-found */
		if (l->obj_id == YAFFS_OBJECTID_LOSTNFOUND) {
			if (!strcmp(name, YAFFS_LOSTNFOUND_NAME))
				return l;
		} else if (l->sum == sum || l->hdr_chunk <= 0) {
			/* LostnFound chunk called Objxxx
			 * Do a real check
			 */
			yaffs_get_obj_name(l, buffer,
				YAFFS_MAX_NAME_LENGTH + 1);
			if (!strncmp(name, buffer, YAFFS_MAX_NAME_LENGTH))
				return l;
		}
	}
	return NULL;
}

/* GetEquivalentObject dereferences any hard links to get to the
 * actual object.
 */

struct yaffs_obj *yaffs_get_equivalent_obj(struct yaffs_obj *obj)
{
	if (obj && obj->variant_type == YAFFS_OBJECT_TYPE_HARDLINK) {
		obj = obj->variant.hardlink_variant.equiv_obj;
		yaffs_check_obj_details_loaded(obj);
	}
	return obj;
}

/*
 *  A note or two on object names.
 *  * If the object name is missing, we then make one up in the form objnnn
 *
 *  * ASCII names are stored in the object header's name field from byte zero
 *  * Unicode names are historically stored starting from byte zero.
 *
 * Then there are automatic Unicode names...
 * The purpose of these is to save names in a way that can be read as
 * ASCII or Unicode names as appropriate, thus allowing a Unicode and ASCII
 * system to share files.
 *
 * These automatic unicode are stored slightly differently...
 *  - If the name can fit in the ASCII character space then they are saved as
 *    ascii names as per above.
 *  - If the name needs Unicode then the name is saved in Unicode
 *    starting at oh->name[1].

 */
static void yaffs_fix_null_name(struct yaffs_obj *obj, YCHAR *name,
				int buffer_size)
{
	/* Create an object name if we could not find one. */
	if (strnlen(name, YAFFS_MAX_NAME_LENGTH) == 0) {
		YCHAR local_name[20];
		YCHAR num_string[20];
		YCHAR *x = &num_string[19];
		unsigned v = obj->obj_id;
		num_string[19] = 0;
		while (v > 0) {
			x--;
			*x = '0' + (v % 10);
			v /= 10;
		}
		/* make up a name */
		strcpy(local_name, YAFFS_LOSTNFOUND_PREFIX);
		strcat(local_name, x);
		strncpy(name, local_name, buffer_size - 1);
	}
}

int yaffs_get_obj_name(struct yaffs_obj *obj, YCHAR *name, int buffer_size)
{
	memset(name, 0, buffer_size * sizeof(YCHAR));
	yaffs_check_obj_details_loaded(obj);
	if (obj->obj_id == YAFFS_OBJECTID_LOSTNFOUND) {
		strncpy(name, YAFFS_LOSTNFOUND_NAME, buffer_size - 1);
	} else if (obj->short_name[0]) {
		strcpy(name, obj->short_name);
	} else if (obj->hdr_chunk > 0) {
		int result;
		u8 *buffer = yaffs_get_temp_buffer(obj->my_dev);

		struct yaffs_obj_hdr *oh = (struct yaffs_obj_hdr *)buffer;

		memset(buffer, 0, obj->my_dev->data_bytes_per_chunk);

		if (obj->hdr_chunk > 0) {
			result = yaffs_rd_chunk_tags_nand(obj->my_dev,
				obj->hdr_chunk, buffer, NULL);
			if (result == YAFFS_OK)
				yaffs_load_name_from_oh(obj->my_dev, name,
					oh->name, buffer_size);
		}
		yaffs_release_temp_buffer(obj->my_dev, buffer);
	}

	yaffs_fix_null_name(obj, name, buffer_size);

	return strnlen(name, YAFFS_MAX_NAME_LENGTH);
}

loff_t yaffs_get_obj_length(struct yaffs_obj *obj)
{
	/* Dereference any hard linking */
	obj = yaffs_get_equivalent_obj(obj);

	if (obj->variant_type == YAFFS_OBJECT_TYPE_FILE)
		return obj->variant.file_variant.file_size;
	if (obj->variant_type == YAFFS_OBJECT_TYPE_SYMLINK) {
		if (!obj->variant.symlink_variant.alias)
			return 0;
		return strnlen(obj->variant.symlink_variant.alias,
				     YAFFS_MAX_ALIAS_LENGTH);
	} else {
		/* Only a directory should drop through to here */
		return obj->my_dev->data_bytes_per_chunk;
	}
}

int yaffs_get_obj_link_count(struct yaffs_obj *obj)
{
	int count = 0;
	struct list_head *i;

	if (!obj->unlinked)
		count++;	/* the object itself */

	list_for_each(i, &obj->hard_links)
	    count++;		/* add the hard links; */

	return count;
}

int yaffs_get_obj_inode(struct yaffs_obj *obj)
{
	obj = yaffs_get_equivalent_obj(obj);

	return obj->obj_id;
}

unsigned yaffs_get_obj_type(struct yaffs_obj *obj)
{
	obj = yaffs_get_equivalent_obj(obj);

	switch (obj->variant_type) {
	case YAFFS_OBJECT_TYPE_FILE:
		return DT_REG;
		break;
	case YAFFS_OBJECT_TYPE_DIRECTORY:
		return DT_DIR;
		break;
	case YAFFS_OBJECT_TYPE_SYMLINK:
		return DT_LNK;
		break;
	case YAFFS_OBJECT_TYPE_HARDLINK:
		return DT_REG;
		break;
	case YAFFS_OBJECT_TYPE_SPECIAL:
		if (S_ISFIFO(obj->yst_mode))
			return DT_FIFO;
		if (S_ISCHR(obj->yst_mode))
			return DT_CHR;
		if (S_ISBLK(obj->yst_mode))
			return DT_BLK;
		if (S_ISSOCK(obj->yst_mode))
			return DT_SOCK;
		return DT_REG;
		break;
	default:
		return DT_REG;
		break;
	}
}

YCHAR *yaffs_get_symlink_alias(struct yaffs_obj *obj)
{
	obj = yaffs_get_equivalent_obj(obj);
	if (obj->variant_type == YAFFS_OBJECT_TYPE_SYMLINK)
		return yaffs_clone_str(obj->variant.symlink_variant.alias);
	else
		return yaffs_clone_str(_Y(""));
}

/*--------------------------- Initialisation code -------------------------- */

static int yaffs_check_dev_fns(struct yaffs_dev *dev)
{
	struct yaffs_driver *drv = &dev->drv;
	struct yaffs_tags_handler *tagger = &dev->tagger;

	/* Common functions, gotta have */
	if (!drv->drv_read_chunk_fn ||
	    !drv->drv_write_chunk_fn ||
	    !drv->drv_erase_fn)
		return 0;

	if (dev->param.is_yaffs2 &&
	     (!drv->drv_mark_bad_fn  || !drv->drv_check_bad_fn))
		return 0;

	/* Install the default tags marshalling functions if needed. */
	yaffs_tags_compat_install(dev);
	yaffs_tags_marshall_install(dev);

	/* Check we now have the marshalling functions required. */
	if (!tagger->write_chunk_tags_fn ||
	    !tagger->read_chunk_tags_fn ||
	    !tagger->query_block_fn ||
	    !tagger->mark_bad_fn)
		return 0;

	return 1;
}

static int yaffs_create_initial_dir(struct yaffs_dev *dev)
{
	/* Initialise the unlinked, deleted, root and lost+found directories */
	dev->lost_n_found = NULL;
	dev->root_dir = NULL;
	dev->unlinked_dir = NULL;
	dev->del_dir = NULL;

	dev->unlinked_dir =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_UNLINKED, S_IFDIR);
	dev->del_dir =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_DELETED, S_IFDIR);
	dev->root_dir =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_ROOT,
				  YAFFS_ROOT_MODE | S_IFDIR);
	dev->lost_n_found =
	    yaffs_create_fake_dir(dev, YAFFS_OBJECTID_LOSTNFOUND,
				  YAFFS_LOSTNFOUND_MODE | S_IFDIR);

	if (dev->lost_n_found &&
		dev->root_dir &&
		dev->unlinked_dir &&
		dev->del_dir) {
			/* If lost-n-found is hidden then yank it out of the directory tree. */
			if (dev->param.hide_lost_n_found)
				list_del_init(&dev->lost_n_found->siblings);
			else
				yaffs_add_obj_to_dir(dev->root_dir, dev->lost_n_found);
		return YAFFS_OK;
	}
	return YAFFS_FAIL;
}

/* Low level init.
 * Typically only used by yaffs_guts_initialise, but also used by the
 * Low level yaffs driver tests.
 */

int yaffs_guts_ll_init(struct yaffs_dev *dev)
{


	yaffs_trace(YAFFS_TRACE_TRACING, "yaffs: yaffs_ll_init()");

	if (!dev) {
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"yaffs: Need a device"
			);
		return YAFFS_FAIL;
	}

	if (dev->ll_init)
		return YAFFS_OK;

	dev->internal_start_block = dev->param.start_block;
	dev->internal_end_block = dev->param.end_block;
	dev->block_offset = 0;
	dev->chunk_offset = 0;
	dev->n_free_chunks = 0;

	dev->gc_block = 0;

	if (dev->param.start_block == 0) {
		dev->internal_start_block = dev->param.start_block + 1;
		dev->internal_end_block = dev->param.end_block + 1;
		dev->block_offset = 1;
		dev->chunk_offset = dev->param.chunks_per_block;
	}

	/* Check geometry parameters. */

	if ((!dev->param.inband_tags && dev->param.is_yaffs2 &&
		dev->param.total_bytes_per_chunk < 1024) ||
		(!dev->param.is_yaffs2 &&
			dev->param.total_bytes_per_chunk < BLOCK_NUMBER) ||
		(dev->param.inband_tags && !dev->param.is_yaffs2) ||
		 dev->param.chunks_per_block < 2 ||
		 dev->param.n_reserved_blocks < 2 ||
		dev->internal_start_block <= 0 ||
		dev->internal_end_block <= 0 ||
		dev->internal_end_block <=
		(dev->internal_start_block + dev->param.n_reserved_blocks + 2)
		) {
		/* otherwise it is too small */
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"NAND geometry problems: chunk size %d, type is yaffs%s, inband_tags %d ",
			dev->param.total_bytes_per_chunk,
			dev->param.is_yaffs2 ? "2" : "",
			dev->param.inband_tags);
		return YAFFS_FAIL;
	}

	/* Sort out space for inband tags, if required */
	if (dev->param.inband_tags)
		dev->data_bytes_per_chunk =
		    dev->param.total_bytes_per_chunk -
		    sizeof(struct yaffs_packed_tags2_tags_only);
	else
		dev->data_bytes_per_chunk = dev->param.total_bytes_per_chunk;

	/* Got the right mix of functions? */
	if (!yaffs_check_dev_fns(dev)) {
		/* Function missing */
		yaffs_trace(YAFFS_TRACE_ALWAYS,
			"device function(s) missing or wrong");

		return YAFFS_FAIL;
	}

	if (!yaffs_init_tmp_buffers(dev))
		return YAFFS_FAIL;

	if (yaffs_init_nand(dev) != YAFFS_OK) {
		yaffs_trace(YAFFS_TRACE_ALWAYS, "InitialiseNAND failed");
		return YAFFS_FAIL;
	}

	dev->ll_init = 1;

	return YAFFS_OK;
}


int yaffs_guts_format_dev(struct yaffs_dev *dev)
{
	u32 i;
	enum yaffs_block_state state;
	u32 dummy;

	if(yaffs_guts_ll_init(dev) != YAFFS_OK)
		return YAFFS_FAIL;

	if(dev->is_mounted)
		return YAFFS_FAIL;

	for (i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		yaffs_query_init_block_state(dev, i, &state, &dummy);
		if (state != YAFFS_BLOCK_STATE_DEAD)
			yaffs_erase_block(dev, i);
	}

	return YAFFS_OK;
}

/*
 * If the dev is mounted r/w then the cleanup will happen during
 * yaffs_guts_initialise. However if the dev is mounted ro then
 * the cleanup will be dfered until yaffs is remounted r/w.
 */
void yaffs_guts_cleanup(struct yaffs_dev *dev)
{
	yaffs_strip_deleted_objs(dev);
	yaffs_fix_hanging_objs(dev);
	if (dev->param.empty_lost_n_found)
			yaffs_empty_l_n_f(dev);
}

int yaffs_guts_initialise(struct yaffs_dev *dev)
{
	int init_failed = 0;
	u32 x;
	u32 bits;
#if defined(GC_Qlearning)
	memset( hdt,0,sizeof(hdt));
    memset(sum_block,0,sizeof(sum_block));
	memset(time_alloc_block,0,sizeof(time_alloc_block));
	start_time = jiffies;
	QTable=initializeQTable(CAPACITY);
	if(!QTable) printk("failed to alloc space for QTable");
	else printk("succeed to initialize QTable");
#endif
	if(yaffs_guts_ll_init(dev) != YAFFS_OK)
		return YAFFS_FAIL;

	if (dev->is_mounted) {
		yaffs_trace(YAFFS_TRACE_ALWAYS, "device already mounted");
		return YAFFS_FAIL;
	}

	dev->is_mounted = 1;

	/* OK now calculate a few things for the device */

	/*
	 *  Calculate all the chunk size manipulation numbers:
	 */
	x = dev->data_bytes_per_chunk;
	/* We always use dev->chunk_shift and dev->chunk_div */
	dev->chunk_shift = calc_shifts(x);
	x >>= dev->chunk_shift;
	dev->chunk_div = x;
	/* We only use chunk mask if chunk_div is 1 */
	dev->chunk_mask = (1 << dev->chunk_shift) - 1;

	/*
	 * Calculate chunk_grp_bits.
	 * We need to find the next power of 2 > than internal_end_block
	 */

	x = dev->param.chunks_per_block * (dev->internal_end_block + 1);

	bits = calc_shifts_ceiling(x);

	/* Set up tnode width if wide tnodes are enabled. */
	if (!dev->param.wide_tnodes_disabled) {
		/* bits must be even so that we end up with 32-bit words */
		if (bits & 1)
			bits++;
		if (bits < 16)
			dev->tnode_width = 16;
		else
			dev->tnode_width = bits;
	} else {
		dev->tnode_width = 16;
	}

	dev->tnode_mask = (1 << dev->tnode_width) - 1;

	/* Level0 Tnodes are 16 bits or wider (if wide tnodes are enabled),
	 * so if the bitwidth of the
	 * chunk range we're using is greater than 16 we need
	 * to figure out chunk shift and chunk_grp_size
	 */

	if (bits <= dev->tnode_width)
		dev->chunk_grp_bits = 0;
	else
		dev->chunk_grp_bits = bits - dev->tnode_width;

	dev->tnode_size = (dev->tnode_width * YAFFS_NTNODES_LEVEL0) / 8;
	if (dev->tnode_size < sizeof(struct yaffs_tnode))
		dev->tnode_size = sizeof(struct yaffs_tnode);

	dev->chunk_grp_size = 1 << dev->chunk_grp_bits;

	if (dev->param.chunks_per_block < dev->chunk_grp_size) {
		/* We have a problem because the soft delete won't work if
		 * the chunk group size > chunks per block.
		 * This can be remedied by using larger "virtual blocks".
		 */
		yaffs_trace(YAFFS_TRACE_ALWAYS, "chunk group too large");

		return YAFFS_FAIL;
	}

	/* Finished verifying the device, continue with initialisation */

	/* More device initialisation */
	dev->all_gcs = 0;
	dev->passive_gc_count = 0;
	dev->oldest_dirty_gc_count = 0;
	dev->bg_gcs = 0;
	dev->gc_block_finder = 0;
	dev->buffered_block = -1;
	dev->doing_buffered_block_rewrite = 0;
	dev->n_deleted_files = 0;
	dev->n_bg_deletions = 0;
	dev->n_unlinked_files = 0;
	dev->n_ecc_fixed = 0;
	dev->n_ecc_unfixed = 0;
	dev->n_tags_ecc_fixed = 0;
	dev->n_tags_ecc_unfixed = 0;
	dev->n_erase_failures = 0;
	dev->n_erased_blocks = 0;
	dev->gc_disable = 0;
	dev->has_pending_prioritised_gc = 1; /* Assume the worst for now,
					      * will get fixed on first GC */
	INIT_LIST_HEAD(&dev->dirty_dirs);
	dev->oldest_dirty_seq = 0;
	dev->oldest_dirty_block = 0;

	yaffs_endian_config(dev);

	/* Initialise temporary caches. */
	dev->gc_cleanup_list = NULL;

	if (!init_failed)
		init_failed = yaffs_cache_init(dev) < 0;

	dev->cache_hits = 0;

	if (!init_failed) {
		dev->gc_cleanup_list =
		    kmalloc(dev->param.chunks_per_block * sizeof(u32),
					GFP_NOFS);
		if (!dev->gc_cleanup_list)
			init_failed = 1;
	}

	if (dev->param.is_yaffs2)
		dev->param.use_header_file_size = 1;

	if (!init_failed && !yaffs_init_blocks(dev))
		init_failed = 1;

	yaffs_init_tnodes_and_objs(dev);

	if (!init_failed && !yaffs_create_initial_dir(dev))
		init_failed = 1;

	if (!init_failed && dev->param.is_yaffs2 &&
		!dev->param.disable_summary &&
		!yaffs_summary_init(dev))
		init_failed = 1;

	if (!init_failed) {
		/* Now scan the flash. */
		if (dev->param.is_yaffs2) {
			if (yaffs2_checkpt_restore(dev)) {
				yaffs_check_obj_details_loaded(dev->root_dir);
				yaffs_trace(YAFFS_TRACE_CHECKPOINT |
					YAFFS_TRACE_MOUNT,
					"yaffs: restored from checkpoint"
					);
			} else {

				/* Clean up the mess caused by an aborted
				 * checkpoint load then scan backwards.
				 */
				yaffs_deinit_blocks(dev);

				yaffs_deinit_tnodes_and_objs(dev);

				dev->n_erased_blocks = 0;
				dev->n_free_chunks = 0;
				dev->alloc_block = -1;
				dev->alloc_page = -1;
#if defined(GC_Qlearning)
				dev->alloc_block2 = -1;
				dev->alloc_page2 = -1;
				dev->alloc_block3 = -1;
				dev->alloc_page3 = -1;
				dev->alloc_block4 = -1;
				dev->alloc_page4 = -1;

				dev->alloc_block_finder = 1;
				dev->alloc_block_finder2 = 1;
				dev->alloc_block_finder3 = 1;
				dev->alloc_block_finder4 = 1;
#endif

				dev->n_deleted_files = 0;
				dev->n_unlinked_files = 0;
				dev->n_bg_deletions = 0;

				if (!init_failed && !yaffs_init_blocks(dev))
					init_failed = 1;

				yaffs_init_tnodes_and_objs(dev);

				if (!init_failed
				    && !yaffs_create_initial_dir(dev))
					init_failed = 1;

				if (!init_failed && !yaffs2_scan_backwards(dev))
					init_failed = 1;
			}
		} else if (!yaffs1_scan(dev)) {
			init_failed = 1;
		}

		yaffs_guts_cleanup(dev);
	}

	if (init_failed) {
		/* Clean up the mess */
		yaffs_trace(YAFFS_TRACE_TRACING,
		  "yaffs: yaffs_guts_initialise() aborted.");

		yaffs_deinitialise(dev);
		return YAFFS_FAIL;
	}

	/* Zero out stats */
	dev->n_page_reads = 0;
	dev->n_page_writes = 0;
	dev->n_erasures = 0;
	dev->n_gc_copies = 0;
	dev->n_retried_writes = 0;

	dev->n_retired_blocks = 0;

	yaffs_verify_free_chunks(dev);
	yaffs_verify_blocks(dev);

	/* Clean up any aborted checkpoint data */
	if (!dev->is_checkpointed && dev->blocks_in_checkpt > 0)
		yaffs2_checkpt_invalidate(dev);

	yaffs_trace(YAFFS_TRACE_TRACING,
	  "yaffs: yaffs_guts_initialise() done.");
	return YAFFS_OK;
}

void yaffs_deinitialise(struct yaffs_dev *dev)
{
	if (dev->is_mounted) {
		u32 i;

		yaffs_deinit_blocks(dev);
		yaffs_deinit_tnodes_and_objs(dev);
		yaffs_summary_deinit(dev);
		yaffs_cache_deinit(dev);

		kfree(dev->gc_cleanup_list);

		for (i = 0; i < YAFFS_N_TEMP_BUFFERS; i++) {
			kfree(dev->temp_buffer[i].buffer);
			dev->temp_buffer[i].buffer = NULL;
		}

		kfree(dev->checkpt_buffer);
		dev->checkpt_buffer = NULL;
		kfree(dev->checkpt_block_list);
		dev->checkpt_block_list = NULL;
#ifdef GC_Qlearning
	freeQTable(QTable);
	freeHashBTable();
#endif
		dev->ll_init = 0;
		dev->is_mounted = 0;

		yaffs_deinit_nand(dev);
	}
}

int yaffs_count_free_chunks(struct yaffs_dev *dev)
{
	int n_free = 0;
	u32 b;
	struct yaffs_block_info *blk;

	blk = dev->block_info;
	for (b = dev->internal_start_block; b <= dev->internal_end_block; b++) {
		switch (blk->block_state) {
		case YAFFS_BLOCK_STATE_EMPTY:
		case YAFFS_BLOCK_STATE_ALLOCATING:
		case YAFFS_BLOCK_STATE_COLLECTING:
		case YAFFS_BLOCK_STATE_FULL:
			n_free +=
			    (dev->param.chunks_per_block - blk->pages_in_use +
			     blk->soft_del_pages);
			break;
		default:
			break;
		}
		blk++;
	}
	return n_free;
}

int yaffs_get_n_free_chunks(struct yaffs_dev *dev)
{
	/* This is what we report to the outside world */
	int n_free;
	int n_dirty_caches;
	int blocks_for_checkpt;

	n_free = dev->n_free_chunks;
	n_free += dev->n_deleted_files;

	/* Now count and subtract the number of dirty chunks in the cache. */
	n_dirty_caches = yaffs_count_dirty_caches(dev);
	n_free -= n_dirty_caches;

	n_free -=
	    ((dev->param.n_reserved_blocks + 1) * dev->param.chunks_per_block);

	/* Now figure checkpoint space and report that... */
	blocks_for_checkpt = yaffs_calc_checkpt_blocks_required(dev);

	n_free -= (blocks_for_checkpt * dev->param.chunks_per_block);

	if (n_free < 0)
		n_free = 0;

	return n_free;
}

/*
 * Marshalling functions to get the appropriate time values saved
 * and restored to/from obj headers.
 *
 * Note that the WinCE time fields are used to store the 32-bit values.
 */

static void yaffs_oh_time_load(u32 *yst_time, u32 *win_time, YTIME_T timeval)
{
	u32 upper;
	u32 lower;

	lower = timeval & 0xffffffff;
	/* we have to use #defines here insted of an if statement
	otherwise the compiler throws an error saying that
	right shift count >= width of type when we are using 32 bit time.
	*/
	#ifdef CONFIG_YAFFS_USE_32_BIT_TIME_T
                upper = 0;
        #else
                upper = (timeval >> 32) & 0xffffffff;
        #endif

	*yst_time = lower;
	win_time[0] = lower;
	win_time[1] = upper;
}

static YTIME_T yaffs_oh_time_fetch(const u32 *yst_time, const u32 *win_time)
{
	u32 upper;
	u32 lower;

	if (win_time[1] == 0xffffffff) {
		upper = 0;
		lower = *yst_time;
	} else {
		upper = win_time[1];
		lower = win_time[0];
	}
	if (sizeof(YTIME_T) > sizeof(u32)) {
		u64 ret;
		ret = (((u64)upper) << 32) | lower;
		return (YTIME_T) ret;

	} else
		return (YTIME_T) lower;
}

YTIME_T yaffs_oh_ctime_fetch(struct yaffs_obj_hdr *oh)
{
	return yaffs_oh_time_fetch(&oh->yst_ctime, oh->win_ctime);
}

YTIME_T yaffs_oh_mtime_fetch(struct yaffs_obj_hdr *oh)
{
	return yaffs_oh_time_fetch(&oh->yst_mtime, oh->win_mtime);
}

YTIME_T yaffs_oh_atime_fetch(struct yaffs_obj_hdr *oh)
{
	return yaffs_oh_time_fetch(&oh->yst_atime, oh->win_atime);
}

void yaffs_oh_ctime_load(struct yaffs_obj *obj, struct yaffs_obj_hdr *oh)
{
	yaffs_oh_time_load(&oh->yst_ctime, oh->win_ctime, obj->yst_ctime);
}

void yaffs_oh_mtime_load(struct yaffs_obj *obj, struct yaffs_obj_hdr *oh)
{
	yaffs_oh_time_load(&oh->yst_mtime, oh->win_mtime, obj->yst_mtime);
}

void yaffs_oh_atime_load(struct yaffs_obj *obj, struct yaffs_obj_hdr *oh)
{
	yaffs_oh_time_load(&oh->yst_atime, oh->win_atime, obj->yst_atime);
}


/*
 * Marshalling functions to get loff_t file sizes into and out of
 * object headers.
 */
void yaffs_oh_size_load(struct yaffs_dev *dev,
			struct yaffs_obj_hdr *oh,
			loff_t fsize,
			int do_endian)
{
	oh->file_size_low = FSIZE_LOW(fsize);

	oh->file_size_high = FSIZE_HIGH(fsize);

	if (do_endian) {
		yaffs_do_endian_u32(dev, &oh->file_size_low);
		yaffs_do_endian_u32(dev, &oh->file_size_high);
	}
}

loff_t yaffs_oh_to_size(struct yaffs_dev *dev, struct yaffs_obj_hdr *oh,
			int do_endian)
{
	loff_t retval;


	if (sizeof(loff_t) >= 8 && ~(oh->file_size_high)) {
		u32 low = oh->file_size_low;
		u32 high = oh->file_size_high;

		if (do_endian) {
			yaffs_do_endian_u32 (dev, &low);
			yaffs_do_endian_u32 (dev, &high);
		}
		retval = FSIZE_COMBINE(high, low);
	} else {
		u32 low = oh->file_size_low;

		if (do_endian)
			yaffs_do_endian_u32(dev, &low);
		retval = (loff_t)low;
	}

	return retval;
}


void yaffs_count_blocks_by_state(struct yaffs_dev *dev, int bs[10])
{
	u32 i;
	struct yaffs_block_info *bi;
	int s;

	for(i = 0; i < 10; i++)
		bs[i] = 0;

	for(i = dev->internal_start_block; i <= dev->internal_end_block; i++) {
		bi = yaffs_get_block_info(dev, i);
		s = bi->block_state;
		if(s > YAFFS_BLOCK_STATE_DEAD || s < YAFFS_BLOCK_STATE_UNKNOWN)
			bs[0]++;
		else
			bs[s]++;
	}
}
/* Added by Ed. 2014.03.24 */
#ifdef YAFFS_GC_ED_MODIFIED
void yaffs_count_blocks_by_erasures(struct yaffs_dev *dev, int *bs)
{
	int i;
	int e;
	int min=0x7fffffff,max=0;
	extern int n_blk_erasures[BLOCK_NUMBER];

	for(i = 0; i < BLOCK_NUMBER; i++)
		bs[i] = 0;

	for(i = dev->internal_start_block-1; i < dev->internal_end_block; i++) {
		e=n_blk_erasures[i];
		bs[i]=e;
		if(e>max) max=e;
		if(e<min) min=e;
	}
	bs[BLOCK_NUMBER]=min;
	bs[BLOCK_NUMBER+1]=max;
}
#endif
//modified by ed .change 512 to BLOCK_NUMBER(4096) 2024.04.15