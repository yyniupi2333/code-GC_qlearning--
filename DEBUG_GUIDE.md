# Q-Learning 垃圾回收调试信息指南

## 概述

已为代码添加了全面的调试监控系统，可实时追踪强化学习 GC 的性能指标。

## 启用调试信息

调试信息默认已启用（在 `yaffs_guts.c` 中）：

```c
#define DEBUG_QLGC  /* 启用调试信息 */
```

**禁用调试信息**（生产环境）：
```c
// #define DEBUG_QLGC  /* 注释掉以禁用 */
```

## 监控统计结构体

```c
struct {
    /* 动作统计 */
    unsigned action_count[4];           /* 4种动作的计数 */
    unsigned total_actions;             /* 总动作数 */
    
    /* 状态统计 */
    unsigned unique_states_visited;     /* 访问的不同状态数 */
    unsigned state_action_pairs;        /* 状态-动作对数量 */
    
    /* Q值统计 */
    int max_qvalue, min_qvalue;         /* Q值范围 */
    int avg_qvalue, qvalue_variance;    /* Q值统计 */
    
    /* 奖励统计 */
    unsigned long total_reward;         /* 累积奖励 */
    unsigned gc_operations;             /* GC操作总次数 */
    unsigned total_migrations;          /* 总页面迁移数 */
    unsigned total_deleted_pages;       /* 总无效页数 */
    
    /* 哈希表碰撞统计 */
    unsigned hash_collisions;           /* 哈希冲突数 */
    unsigned q_entries_used;            /* 已使用的表项 */
    
    /* GC效率统计 */
    unsigned long chunks_recovered_total; /* 总恢复块数 */
    unsigned gc_success_count;          /* GC成功次数 */
    unsigned gc_fail_count;             /* GC失败次数 */
    
    /* 磨损均衡统计 */
    int last_wear_gap;                  /* 当前擦除差距 */
    int max_wear_gap;                   /* 最大擦除差距 */
} ql_stats;
```

## 输出信息详解

### 1. 训练进度 [QL-GC-EPOCH]
```
[QL-GC-EPOCH] Training: 1000/50000 | epsilon: 49000 | Progress: 2.0%
```
- **Training**: 当前轮数/总轮数
- **epsilon**: 探索率（初始大→逐步减小）
- **Progress**: 训练进度百分比

### 2. 动作分布 [QL-GC-ACTION]
```
[QL-GC-ACTION] Distribution: HOT=450(45.0%) MEDIUM=250(25.0%) COLD=200(20.0%) SUPER=100(10.0%)
```
- **HOT**: 热块分配（最小擦除数）
- **MEDIUM**: 普通块分配
- **COLD**: 冷块分配（最大擦除数）
- **SUPER**: 超级冷块分配

**含义**：如果 HOT 比例过高，说明策略偏向于写入新块；COLD 比例高说明策略倾向于平衡磨损。

### 3. 状态空间 [QL-GC-STATE]
```
[QL-GC-STATE] Visited states: 512 | State-action pairs: 240 | Q-table utilization: 93.8%
```
- **Visited states**: 发现了多少种不同的数据热度状态
- **State-action pairs**: 学到了多少个有效的状态-动作对应关系
- **Q-table utilization**: 表格填充率（越高越好，≈90% 最优）

### 4. GC效率 [QL-GC-PERF]
```
[QL-GC-PERF] GC ops: 125 | Success rate: 96.0% | Avg migrations: 18.4 | Recovery rate: 68.5%
```
- **GC ops**: 执行过多少次 GC 操作
- **Success rate**: GC 成功率（应 > 95%）
- **Avg migrations**: 平均每次 GC 迁移多少页面（越低越好）
- **Recovery rate**: 块恢复率（百分比，越高越好）

### 5. 磨损均衡 [QL-GC-WEAR]
```
[QL-GC-WEAR] Erasure gap: current=35 | max=128 | Twl=85
```
- **current**: 当前最大/最小擦除次数的差距
- **max**: 历史最大差距
- **Twl**: 穿戴均衡阈值（决定何时强制 GC）

**评估**：
- gap < 50：很好的磨损均衡 ✅
- 50 ≤ gap < 100：一般 ⚠️
- gap ≥ 100：需要改进 ❌

### 6. 哈希表健康度 [QL-GC-HASH]
```
[QL-GC-HASH] Load factor: 0.94 | Collisions: 12 | Entries: 240/256
```
- **Load factor**: 表格装载因子（0.7-0.9 最优）
- **Collisions**: 哈希碰撞次数
- **Entries**: 已使用表项/总容量

## 关键指标解读

### 性能评估矩阵

| 指标 | 理想值 | 一般值 | 需改进 |
|------|------|------|--------|
| 动作分布均衡 | 接近 25% 各占 | 某种 >40% | 某种 >50% |
| GC 成功率 | >98% | 95-98% | <95% |
| 平均迁移数 | <15 | 15-25 | >25 |
| 恢复率 | >70% | 50-70% | <50% |
| 磨损差距 | <50 | 50-100 | >100 |
| Q表利用率 | 80-95% | 60-80% | <60% |

### 训练阶段识别

```
前期 (0-20%)          中期 (20-80%)         后期 (80-100%)
├─ epsilon 大         ├─ epsilon 中等       ├─ epsilon 小
├─ 大量探索           ├─ 探索+利用混合      ├─ 主要利用
├─ 动作分布杂乱       ├─ 逐渐聚焦           ├─ 高度聚焦
└─ Q值变化快          └─ Q值逐渐稳定      └─ Q值基本不变
```

## 实时监控脚本

查看实时日志：
```bash
# 查看最新的 QL-GC 日志
dmesg | tail -100 | grep "QL-GC"

# 提取训练进度
dmesg | grep "QL-GC-EPOCH"

# 监控 GC 效率趋势
watch 'dmesg | tail -20 | grep QL-GC-PERF'
```

## 故障诊断

### 问题1：动作分布极度不均衡

**症状**：某种动作占比 >80%

**原因**：
- 初始化有偏差
- 状态编码问题
- 奖励函数设计不当

**解决**：
- 检查 `get_uft_flag()` 是否正确映射热度
- 验证奖励函数对所有动作的评分
- 增加探索期时间

### 问题2：Q表利用率过低 (<60%)

**症状**：`Entries: 60/256`

**原因**：
- 访问的状态太少
- 数据工作集较小
- 哈希碰撞导致表项未被填满

**解决**：
- 增加状态粒度（UFT 分层）
- 使用更大的测试数据集
- 优化哈希函数

### 问题3：GC 成功率过低 (<95%)

**症状**：`Success rate: 88.0%`

**原因**：
- 块选择策略不当
- 页面迁移失败
- 硬件故障

**解决**：
- 检查块选择逻辑
- 查看 yaffs_write_new_chunk_for_migration 错误
- 验证 NAND 芯片健康状态

### 问题4：磨损差距过大 (>100)

**症状**：`Erasure gap: current=256`

**原因**：
- Twl 阈值设置过高
- 冷热数据分离不清
- 算法未正确学习

**解决**：
- 降低 INIT_TWL 值
- 改进热度评估（UFT 计算）
- 增加训练轮数

## 自定义监控

在代码中添加自定义监控：

```c
#ifdef DEBUG_QLGC
/* 示例：监控特定状态 */
if(state->uft_flag > 50) {  /* 冷数据 */
    printk("[CUSTOM] Cold data state detected: uft_flag=%u\n", state->uft_flag);
    ql_stats.cold_state_count++;
}
#endif
```

## 性能影响

启用调试信息的开销：
- **内存**: ~1-2 KB（统计结构体）
- **CPU**: 每次打印 ~0.5-1% 额外开销
- **日志**: ~500 字节/秒（打印频率 1000 次/秒）

建议生产环境下禁用详细打印，仅保留关键路径的统计。

---

**最后更新**：2025年12月3日
**调试系统版本**：1.0
