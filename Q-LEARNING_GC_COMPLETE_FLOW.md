# YAFFS Q-Learning GC 完整流程总结

## 一、宏观流程概览

```
启动 → 初始化Q表和离散化器 → 正常文件操作 → 触发GC → 迭代训练 → 完成训练 → 生产模式
                                            ↓
                                    开始处理受害块
                                            ↓
                                    遍历受害块中的有效页
                                            ↓
                                    每页执行：获取状态→选择动作→分配块
                                            ↓
                                    块满后计算奖励→更新Q值
                                            ↓
                                    重复直到完成GC
```

## 二、详细工作流程（从处理第一个受害块开始）

### 第1阶段：GC触发条件评估

**场景**：Flash存储空间紧张，触发垃圾回收

```
yaffs_check_gc() called
  ↓
评估是否需要GC：
- n_erased_blocks < n_reserved_blocks？
- 需要回收多少空间？
  ↓
选择受害块（被标记为gc_block）
```

### 第2阶段：初始化GC上下文

**代码位置**：yaffs_guts.c L1658-1695

```c
#if defined(GC_Qlearning)
unsigned now = jiffies;              /* 当前时间戳 */
unsigned time = 0;
unsigned cnt = 0;
struct State* current_state = NULL;
enum Action current_action;
int hot = 0;
int new_start = 1;                   /* 标记这是一个新的GC周期 */
#endif
```

**作用**：为这个受害块的GC准备Q-Learning上下文

### 第3阶段：处理受害块中的有效页（核心循环）

**代码位置**：yaffs_guts.c L1650-1750

#### 步骤3.1：读取有效页的元数据

```c
/* 从受害块中读取一个有效页的标签信息 */
yaffs_rd_chunk_tags_nand(dev, chunk, NULL, &tags);

/* tags包含：
   - obj_id: 文件ID
   - chunk_id: 块内页号
   - size: 数据大小
   - serial_number: 版本号
*/
```

#### 步骤3.2：获取页面的访问热度历史

```c
/* 从热度追踪表(HDT)获取该页的历史信息 */
get_time_hot(tags->obj_id, tags->chunk_id, &time, &cnt);

/* 返回值：
   - time: 上次访问时间
   - cnt: 访问计数
*/
```

#### 步骤3.3：计算UFT（Update First Time）

```c
/* UFT = (当前时间 - 上次访问时间) / 访问计数 */
int uft = (cnt > 0) ? (now - time) / cnt : (now - time);

/* UFT含义：
   - 小UFT：最近经常被访问（热数据）
   - 大UFT：长期未被访问（冷数据）
*/
```

#### 步骤3.4：获取状态（关键！这里用到了离散化器）

```c
struct State* current_state = get_usable_state(tags, aver_uft);

/* 内部调用流程：
   1. 调用 discretize_uft_value(uft)
   2. 离散化器根据UFT值分配到某个分组（0-63）
   3. 返回该分组ID作为状态
*/

/* 状态的含义：
   状态0-63代表不同的"页面热度等级"
   - 状态0-10: 热页面（经常访问）
   - 状态11-40: 温页面（偶尔访问）
   - 状态41-63: 冷页面（很少访问）
*/
```

**离散化器工作原理**：
```
第1个UFT值 → 初始化分组0 → 返回状态ID = 0
第2-100个UFT值 → 扩展分组0的范围
第101个UFT值（如果超过2*min）→ 分裂！创建分组1
后续UFT值 → 分配到分组0或分组1 → 返回对应的状态ID
```

#### 步骤3.5：更新Q值（从上一个页面的迁移）

**前提条件**：这不是第一个页面（`new_start == 0`）

```c
if (!new_start) {
    /* 计算从上一个页面到当前页面的Q值更新 */
    int cur_qvalue = updateQValue_q(
        QTable,           /* Q表 */
        last_state,       /* 上一页的状态 */
        current_state,    /* 当前页的状态 */
        last_action       /* 上一页选择的动作 */
    );
    /* 无即时奖励，只有衰减的未来Q值 */
}
else {
    new_start = 0;        /* 标记：已不是第一个页面 */
}
```

**Q值更新公式**（无奖励版）：
```
Q(s,a) ← Q(s,a) + α * (γ * max_Q(s',·) - Q(s,a))
其中：
  α = learningRate = 1
  γ = discountFactor = 5（固定乘以10再除以10）
  s = last_state
  a = last_action
  s' = current_state
  max_Q(s',·) = getbestqvalue(QTable, current_state)
```

#### 步骤3.6：选择动作（ε-贪心策略）

```c
enum Action current_action = getaction(QTable, current_state);

/* getaction() 内部流程：
   1. 生成随机数 rand ∈ [0, num_trainings)
   2. IF rand > epsilon:
        → 利用（Exploitation）：选择最优动作
        current_action = getbestaction(QTable, current_state)
      ELSE:
        → 探索（Exploration）：随机选择动作
        current_action = getrandomaction(QTable, current_state)
   3. 更新动作统计信息（DEBUG_QLGC时）
*/
```

**动作空间**（4个动作）：
```
ACTION_ZERO   (0) → HOT分配     (存储在Hot块中)
ACTION_ONE    (1) → MEDIUM分配  (存储在Medium块中)
ACTION_TWO    (2) → COLD分配    (存储在Cold块中)
ACTION_THREE  (3) → SUPER分配   (存储在Super块中)
```

**epsilon衰减策略**：
```
训练阶段（Qlearning_count < num_trainings）:
  epsilon = MIN_EPSILON + (decay_range * (num_trainings - Qlearning_count)) / num_trainings
  
  示例（num_trainings=50000）:
    Qlearning_count=0:     epsilon=50000  (100%探索）
    Qlearning_count=25000: epsilon=27500  (55%探索）
    Qlearning_count=50000: epsilon=2500   (5%探索）

训练完成（Qlearning_count >= num_trainings）:
  epsilon = MIN_EPSILON = 2500  (锁定5%探索）
```

#### 步骤3.7：根据动作分配块

```c
hot = enumtoint(current_action);  /* 将动作转换为块类型 */
chunk = yaffs_alloc_chunk(dev, use_reserver, &bi, hot);
/* 根据hot值分配：
   0 → 分配HOT块
   1 → 分配MEDIUM块
   2 → 分配COLD块
   3 → 分配SUPER块
*/
block = chunk / dev->param.chunks_per_block;  /* 获取所在块号 */
```

#### 步骤3.8：保存状态-动作-块的映射

```c
/* 使用哈希表记录这个状态-动作对被分配到的块 */
insertHashTable(current_state, current_action, block);

/* 哈希表作用：
   在块被填满并计算奖励时，
   可以快速找到该块中所有的状态-动作对
*/
```

#### 步骤3.9：更新页面访问历史

```c
set_time_hot(tags->obj_id, tags->chunk_id, now, ++cnt);
/* 记录这次访问信息到HDT表中，供下次计算UFT用 */
```

#### 步骤3.10：保存状态和动作用于下一次迭代

```c
last_state = current_state;    /* 保存当前状态 */
last_action = current_action;  /* 保存当前动作 */
/* 这些值将在下一个有效页被迁移时使用 */
```

#### 步骤3.11：写入新块

```c
/* 实际的数据迁移 */
write_ok = yaffs_wr_chunk_tags_nand(dev, chunk, data, tags);
yaffs_verify_chunk_written(dev, chunk, data, tags);

if (write_ok != YAFFS_OK) {
    /* 写入失败处理 */
    break;  /* 退出循环 */
}
```

### 第4阶段：块满后的奖励计算

**代码位置**：yaffs_guts.c L4300-4330

**触发条件**：当前受害块的所有有效页都被迁移完毕

```c
/* 计算GC效率指标 */
int number_of_migrations = /* 迁移的页数 */
int bi->soft_del_pages = /* 删除的无效页数 */
int ry_interval_time = /* 本次GC耗时 */

/* 计算奖励 */
int reward = getreward(number_of_migrations, bi->soft_del_pages, ry_interval_time);

/* getreward() 评估：
   ✓ 迁移页数少（高效）→ 奖励高
   ✓ 删除页数多（回收多）→ 奖励高
   ✓ 耗时短（速度快）→ 奖励高
   
   返回值：通常 > 0（成功的GC），可能 < 0（低效的GC）
*/
```

### 第5阶段：批量Q值更新

**代码位置**：yaffs_guts.c L4318-4328

```c
/* 找出本次GC中所有相关的状态-动作对 */
struct HashNode* current_node = searchInHashTable(dev->gc_block);

/* 统计这些状态-动作对的个数 */
unsigned num_node = 0;
while (current_node) {
    num_node++;
    current_node = current_node->next;
}

/* 遍历所有状态-动作对，用奖励更新Q值 */
current_node = temp;  /* 重新指向表头 */
while (current_node) {
    /* 平分奖励：reward / num_node */
    int cur_qvalue = updateQValue_r(
        QTable,
        current_node->state,
        current_node->action,
        reward / num_node  /* 奖励平均分配 */
    );
    
    /* 处理Q值溢出 */
    if (cur_qvalue < 0 || cur_qvalue == 2147483647) {
        update_to_handle_overflow(QTable, cur_qvalue, 
                                 current_node->state, 
                                 current_node->action);
    }
    
    current_node = current_node->next;
}

deleteFromHashTable(dev->gc_block);  /* 清空哈希表 */
```

**Q值更新公式**（有奖励版）：
```
Q(s,a) ← Q(s,a) + α * (reward - Q(s,a))
其中：
  α = learningRate = 1
  reward = 本次GC的奖励值 / 相关状态-动作对数
```

### 第6阶段：训练计数和epsilon衰减

**代码位置**：yaffs_guts.c L4361-4445

```c
#if defined(GC_Qlearning)
if (Qlearning_count < num_trainings) {
    Qlearning_count++;  /* 记录完成的GC轮次 */
}

/* ===== 阶段区分 ===== */
if (Qlearning_count < num_trainings) {
    /* 训练阶段 */
    
    /* epsilon线性衰减 */
    unsigned decay_range = num_trainings - MIN_EPSILON;
    epsilon = MIN_EPSILON + (decay_range * (num_trainings - Qlearning_count)) / num_trainings;
    
    /* 每1000次GC打印详细统计 */
    if (Qlearning_count % 1000 == 0) {
        printk("[QL-GC-EPOCH] Training: %u/%u | epsilon: %u | Progress: %u%%\n",
               Qlearning_count, num_trainings, epsilon,
               (100 * Qlearning_count) / num_trainings);
        printk("[QL-GC-STATE] Visited states: %u | State-action pairs: %u | "
               "Q-table utilization: %u%%\n",
               ql_stats.unique_states_visited,
               ql_stats.state_action_pairs,
               (100 * ql_stats.state_action_pairs) / CAPACITY);
    }
}
else {
    /* 生产模式：训练完成 */
    epsilon = MIN_EPSILON;  /* 保持5%探索 */
    
    if (Qlearning_count == num_trainings) {
        /* 仅打印一次"训练完成"消息 */
        printk("\n[QL-GC-FINAL] ===== 训练完成 =====\n");
        printk("[QL-GC-FINAL] Total iterations: %u\n", num_trainings);
        printk("[QL-GC-FINAL] Final epsilon: %u (永久探索: 5%%)\n", MIN_EPSILON);
        printk("[QL-GC-FINAL] Switching to production mode\n\n");
    }
}
#endif
```

## 三、完整时间线示例

### 场景：处理一个包含5个有效页的受害块

```
时刻1：选择受害块
├─ 受害块 = Block 100
├─ 其中有5个有效页（页序号: 10, 25, 40, 55, 70）
└─ new_start = 1

─────────────────────────────────────────

页面1：obj_id=1, chunk_id=10
├─ 计算UFT(1,10)
├─ 获取状态 → discretize_uft_value(UFT) → 状态ID=15（中等热度）
├─ 是否第一个页？是 → 跳过Q值更新
├─ 选择动作 → getaction(Q_table, 状态15) → ACTION_ONE
├─ 分配块 → 分配到MEDIUM块（块号：50）
├─ 保存映射 → HashTable[(15,1)] = Block50
├─ 保存状态 → last_state=15, last_action=ACTION_ONE
└─ 写入数据

─────────────────────────────────────────

页面2：obj_id=2, chunk_id=25
├─ 计算UFT(2,25) → 2000
├─ 获取状态 → discretize_uft_value(2000) → 状态ID=42（冷数据）
├─ 是否第一个页？否 → 更新Q值
│  └─ updateQValue_q(Q_table, 状态15, 状态42, ACTION_ONE)
│     Q(15,ACTION_ONE) ← Q(15,ACTION_ONE) + α*(γ*max_Q(42,·) - Q(15,ACTION_ONE))
├─ 选择动作 → getaction(Q_table, 状态42) → ACTION_THREE
├─ 分配块 → 分配到SUPER块（块号：200）
├─ 保存映射 → HashTable[(42,3)] = Block200
├─ 保存状态 → last_state=42, last_action=ACTION_THREE
└─ 写入数据

─────────────────────────────────────────

页面3,4,5：类似流程...

─────────────────────────────────────────

块满后：计算奖励
├─ 迁移页数 = 5
├─ 删除页数 = 10
├─ 耗时 = 50ms
├─ reward = getreward(5, 10, 50) = 100（举例）
├─ 查找本块相关的状态-动作对 = [(15,1), (42,3), ...]
├─ 奖励分配 = 100 / 5 = 20
└─ 批量更新Q值：
   Q(15,ACTION_ONE) ← Q(15,ACTION_ONE) + α*(20 - Q(15,ACTION_ONE))
   Q(42,ACTION_THREE) ← Q(42,ACTION_THREE) + α*(20 - Q(42,ACTION_THREE))
   ...

─────────────────────────────────────────

训练统计
├─ Qlearning_count++  (从50到51)
├─ epsilon更新 (从49999衰减到49950)
├─ ql_stats更新
│  ├─ gc_operations++
│  ├─ total_migrations += 5
│  ├─ total_reward += 100
│  └─ state_action_pairs可能增加（若新状态-动作对产生）
└─ 若Qlearning_count % 1000 == 0，打印进度
```

## 四、离散化器在此流程中的作用

```
UFT值序列：100, 150, 2000, 80, 3000

处理阶段：
├─ 样本1(UFT=100) → 分组0初始化[100,100] → 状态ID=0
├─ 样本2(UFT=150) → 扩展分组0[100,150] → 状态ID=0
├─ 样本3(UFT=2000) → 扩展分组0[100,2000] → 状态ID=0
├─ 样本4(UFT=80) → 扩展分组0[80,2000] → 状态ID=0
└─ 样本100+ → 检查分裂条件：2000 >= 2*80? 是！
              → 分裂点 = geometric_mean(80,2000) ≈ 400
              → 分组0调整为[80,400]
              → 创建分组1[400,2000]
              → 后续UFT值分配到0或1

结果：自适应创建了两个状态，对应两个Q值表项
```

## 五、状态空间演化

```
初始状态：1个分组，覆盖所有UFT值 → 64个Q表项
   ↓
运行过程中：根据UFT数据分布动态分裂
   ↓
最终状态：2-64个分组，每个对应一个状态ID
   ↓
效果：Q-Learning获得更细粒度的状态表示
```

## 六、关键数据结构

### State（状态）
```c
struct State {
    unsigned uft_flag;  /* 0-63之间的状态ID */
}
```

### QTableEntry（Q表项）
```c
struct QTableEntry {
    struct State* state;        /* 状态 */
    enum Action action;         /* 动作 */
    int qValue;                 /* Q值 */
}
```

### HashNode（哈希表节点）
```c
struct HashNode {
    struct State* state;
    enum Action action;
    unsigned block;             /* 被分配到的块号 */
    struct HashNode* next;      /* 链表指针，处理碰撞 */
}
```

## 七、关键参数

| 参数 | 值 | 含义 |
|------|-----|------|
| num_trainings | 50000 | 训练轮次（GC次数） |
| learningRate | 1 | 学习率 |
| discountFactor | 5 | 折扣因子 |
| CAPACITY | 256 | Q表大小（4动作×64状态） |
| MIN_EPSILON | 2500 | 最小探索率（5%） |
| MAX_GROUPS | 64 | 最多离散化分组数 |
| MIN_SAMPLES | 100 | 触发分裂的最少样本数 |

