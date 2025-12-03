# 多分组状态下的查询与分配机制

## 场景：3个分组的离散器

```
当前离散器状态（已经过多次分裂）：

Group-0: [100, 1346]      样本数=150
Group-1: [1346, 4928]     样本数=120
Group-2: [4928, 18000]    样本数=130

总样本数：400
```

---

## 第一部分：二分查找的完整过程

### 查询示例1：UFT=500（应该归入Group-0）

**初始状态**：
```c
left = 0
right = 2  (group_count - 1)
best_group = 0
```

**第1次迭代**：
```c
mid = 0 + (2 - 0) / 2 = 1
group_min = groups[1].min_val = 1346
group_max = groups[1].max_val = 4928

检查条件：
if (500 >= 1346 && 500 <= 4928) {  // FALSE
    best_group = 1;
    break;
}
else if (500 < 1346) {  // TRUE ← 进入左半部分
    right = 1 - 1 = 0;
}
else {
    left = 1 + 1 = 2;
}

状态更新：left=0, right=0
```

**第2次迭代**：
```c
mid = 0 + (0 - 0) / 2 = 0
group_min = groups[0].min_val = 100
group_max = groups[0].max_val = 1346

检查条件：
if (500 >= 100 && 500 <= 1346) {  // TRUE ← 精确匹配！
    best_group = 0;
    break;  ← 循环退出
}
```

**结论**：
```
返回状态ID = 0
分配到Group-0
```

**二分查找过程可视化**：
```
     [100, 1346]     [1346, 4928]    [4928, 18000]
      Group-0         Group-1         Group-2
        mid=1
         |
    500 < 1346 → 向左搜索
         |
      [100, 1346]
       mid=0
        |
   500在范围内 → 找到！返回0
```

---

### 查询示例2：UFT=3000（应该归入Group-1）

**初始状态**：
```c
left = 0
right = 2
best_group = 0
```

**第1次迭代**：
```c
mid = 0 + (2 - 0) / 2 = 1
group_min = 1346
group_max = 4928

if (3000 >= 1346 && 3000 <= 4928) {  // TRUE ← 精确匹配！
    best_group = 1;
    break;  ← 循环退出
}
```

**结论**：
```
返回状态ID = 1
分配到Group-1
```

**效率优势**：仅需1次迭代！（相比线性查找需要2次）

---

### 查询示例3：UFT=15000（应该归入Group-2）

**初始状态**：
```c
left = 0
right = 2
best_group = 0
```

**第1次迭代**：
```c
mid = 1
group_min = 1346
group_max = 4928

if (15000 >= 1346 && 15000 <= 4928) {  // FALSE
    // 不匹配
}
else if (15000 < 1346) {  // FALSE
    right = 0;
}
else {  // TRUE ← 向右搜索
    left = 1 + 1 = 2;
}

状态更新：left=2, right=2
```

**第2次迭代**：
```c
mid = 2 + (2 - 2) / 2 = 2
group_min = 4928
group_max = 18000

if (15000 >= 4928 && 15000 <= 18000) {  // TRUE ← 找到！
    best_group = 2;
    break;
}
```

**结论**：
```
返回状态ID = 2
分配到Group-2
```

---

### 查询示例4：UFT=50（超出所有分组最小值 - 扩展逻辑）

**初始状态**：
```c
left = 0
right = 2
best_group = 0
```

**第1次迭代**：
```c
mid = 1
group_min = 1346
group_max = 4928

if (50 >= 1346 && 50 <= 4928) {  // FALSE
}
else if (50 < 1346) {  // TRUE ← 向左搜索
    right = 0;
}
```

**第2次迭代**：
```c
mid = 0
group_min = 100
group_max = 1346

if (50 >= 100 && 50 <= 1346) {  // FALSE
}
else if (50 < 100) {  // TRUE ← 继续向左
    right = -1;
}
```

**循环条件检查**：
```c
while (left <= right) {  // 0 <= -1 是FALSE
    // 循环退出
}
```

**范围检查逻辑**（分裂后的新增逻辑）：
```c
if (50 < groups[0].min_val ||      // 50 < 100 是TRUE
    50 > groups[0].max_val) {      // 50 > 1346 是FALSE
    
    if (left < (s32)group_count) {  // 0 < 3 是TRUE
        best_group = left = 0;  ← 选择第一个可扩展的分组
    }
}

/* 扩展Group-0的最小值 */
if (50 < groups[0].min_val) {
    groups[0].min_val = 50;  // [100, 1346] → [50, 1346]
}
groups[0].sample_count++;  // 150 → 151
```

**结论**：
```
返回状态ID = 0
分配到Group-0，并扩展其范围到[50, 1346]
```

---

## 第二部分：5个页面处理流程（3个分组情况）

### 场景设置

```
受害块有5个有效页，UFT值分别为：
Page-1: UFT = 500   ← 应该在[100, 1346]内
Page-2: UFT = 3000  ← 应该在[1346, 4928]内
Page-3: UFT = 8000  ← 应该在[4928, 18000]内
Page-4: UFT = 50    ← 小于所有分组最小值，扩展
Page-5: UFT = 1500  ← 应该在[1346, 4928]内
```

### Page-1处理（UFT=500）

**二分查找过程**：
```
left=0, right=2
mid=1: 500 < 1346 → right=0
mid=0: 500在[100,1346]内 → best_group=0, break
```

**分配**：
```
Group-0.sample_count: 150 → 151
返回状态ID = 0
Hash表记录：(state=0, action=..., block_id=...)
```

### Page-2处理（UFT=3000）

**二分查找过程**：
```
left=0, right=2
mid=1: 3000在[1346,4928]内 → best_group=1, break ← 一次迭代成功！
```

**分配**：
```
Group-1.sample_count: 120 → 121
返回状态ID = 1
Hash表记录：(state=1, action=..., block_id=...)
```

### Page-3处理（UFT=8000）

**二分查找过程**：
```
left=0, right=2
mid=1: 8000 > 4928 → left=2
mid=2: 8000在[4928,18000]内 → best_group=2, break
```

**分配**：
```
Group-2.sample_count: 130 → 131
返回状态ID = 2
Hash表记录：(state=2, action=..., block_id=...)
```

### Page-4处理（UFT=50）

**二分查找过程**：
```
left=0, right=2
mid=1: 50 < 1346 → right=0
mid=0: 50 < 100 → right=-1
循环退出：left=0, right=-1
```

**范围检查和扩展**：
```c
if (50 < groups[0].min_val) {  // TRUE
    best_group = left = 0;
}

/* 扩展Group-0 */
groups[0].min_val = 50;  // [100, 1346] → [50, 1346]
groups[0].sample_count = 152
```

**分配**：
```
Group-0.sample_count: 151 → 152
返回状态ID = 0
Hash表记录：(state=0, action=..., block_id=...)
```

### Page-5处理（UFT=1500）

**二分查找过程**：
```
left=0, right=2
mid=1: 1500在[1346,4928]内 → best_group=1, break
```

**分配**：
```
Group-1.sample_count: 121 → 122
返回状态ID = 1
Hash表记录：(state=1, action=..., block_id=...)
```

---

## 受害块处理完成后的状态

### 离散器状态变化

```
处理前：
  Group-0: [100, 1346]       样本=150
  Group-1: [1346, 4928]      样本=120
  Group-2: [4928, 18000]     样本=130

处理后：
  Group-0: [50, 1346]        样本=152  ← min_val扩展，样本+2
  Group-1: [1346, 4928]      样本=122  ← 样本+2
  Group-2: [4928, 18000]     样本=131  ← 样本+1

总样本数：400 → 405
```

### Hash表记录

```
BTable[block_id] 链表：
  → (state=0, action=ACT_A)  ← Page-1和Page-4
  → (state=1, action=ACT_B)  ← Page-2和Page-5
  → (state=2, action=ACT_C)  ← Page-3
  → (state=0, action=ACT_D)  ← Page-4分配的不同action
  → ... (可能有链式结构处理碰撞)
```

### Q-Learning更新

```
GC完成后的奖励计算和分配：

reward = getreward(migrations, deleted_pages, time)

遍历Hash表中所有(state, action)对：
  ├─ (state=0, action=ACT_A): Q[0][ACT_A] += reward/n_nodes
  ├─ (state=1, action=ACT_B): Q[1][ACT_B] += reward/n_nodes
  ├─ (state=2, action=ACT_C): Q[2][ACT_C] += reward/n_nodes
  └─ (state=0, action=ACT_D): Q[0][ACT_D] += reward/n_nodes
```

---

## 第三部分：二分查找的性能对比

### 线性查找 vs 二分查找

```
查询UFT=3000时：

【线性查找】：
  i=0: 3000在[100,1346]内? NO
  i=1: 3000在[1346,4928]内? YES → 返回1
  迭代次数：2次

【二分查找】：
  mid=1: 3000在[1346,4928]内? YES → 返回1
  迭代次数：1次

性能提升：50%
```

### 规模扩展性能

```
分组数     线性查找(最坏)  二分查找(最坏)   提升
─────────────────────────────────────────
1         1             1             1.0×
2         2             1             2.0×
4         4             2             2.0×
8         8             3             2.7×
16        16            4             4.0×
32        32            5             6.4×
64        64            6             10.7×
```

---

## 第四部分：分裂过程中的多分组场景

### 当前状态（3个分组）

```
Group-0: [100, 1346]     sample_count=160
Group-1: [1346, 4928]    sample_count=130
Group-2: [4928, 18000]   sample_count=140
```

### 情况1：Group-1达到分裂条件

```
Sample 100到达，Group-1的第100个样本：

分裂判断：
  ├─ sample_count >= 100? YES (130 >= 100)
  ├─ max >= 2*min? 4928 >= 2*1346=2692? YES
  └─ group_count < 64? YES (3 < 64)
  
执行分裂：
  split_point = geometric_mean(1346, 4928) ≈ 2600
  
  新分组Group-3：
    min_val = 2600
    max_val = 4928
    sample_count = 0
  
  Group-1调整：
    min_val = 1346
    max_val = 2600
    sample_count = 130（保持不变）

结果：
  Group-0: [100, 1346]     样本=160
  Group-1: [1346, 2600]    样本=130  ← 范围缩小
  Group-2: [2600, 4928]    样本=0    ← 新分组
  Group-3: [4928, 18000]   样本=140  ← 旧的Group-2改编号
```

### 情况2：后续查询的变化

```
新页面UFT=3500到达时：

二分查找（4个分组）：
  left=0, right=3
  mid=1: 3500在[1346,2600]内? NO，3500 > 2600
  left=2
  
  mid=2: 3500在[2600,4928]内? YES → best_group=2, break

返回状态ID=2（新分配目标）
Group-2.sample_count: 0 → 1
```

---

## 第五部分：边界情况

### 情况1：值在分裂点上（UFT=2600）

```
如果UFT恰好等于分裂点：

二分查找找到Group-1：
  if (2600 >= 1346 && 2600 <= 2600) {  // TRUE
      best_group = 1;
      break;
  }

返回状态ID=1
分配到Group-1（因为线性查找顺序）
```

### 情况2：值超出所有分组右边界

```
假设UFT=100000（远大于最大分组上限18000）：

二分查找：
  left经过多次迭代后变为3（group_count）
  
范围检查：
  if (100000 > groups[3].max_val) {  // TRUE
      if (left < group_count) {  // 3 < 4? 否
          best_group = left;
      }
      else {
          best_group = group_count - 1 = 3;  ← 扩展最后一个分组
      }
  }

/* 扩展Group-3 */
groups[3].max_val = 100000;  // [4928, 18000] → [4928, 100000]

返回状态ID=3
分配到Group-3，范围扩展
```

### 情况3：值小于所有分组左边界

```
假设UFT=10（远小于最小分组下限100）：

二分查找：
  right变为-1，left保持0
  
范围检查：
  if (10 < groups[0].min_val) {  // TRUE
      if (left < group_count) {  // 0 < 4? YES
          best_group = left = 0;  ← 扩展第一个分组
      }
  }

/* 扩展Group-0 */
groups[0].min_val = 10;  // [100, 1346] → [10, 1346]

返回状态ID=0
分配到Group-0，范围扩展
```

---

## 总结：多分组查询分配流程

### ✅ 关键特点

```
1. 二分查找高效
   ├─ O(log n) 时间复杂度
   ├─ n为分组数（最多64）
   └─ 实际最多需要6次比较

2. 精确匹配优先
   ├─ 值在范围内 → 立即返回
   ├─ 值超出范围 → 选择可扩展分组
   └─ 保证每个值被正确分配

3. 范围动态扩展
   ├─ 分组范围会随新值调整
   ├─ 超小值扩展最小值
   ├─ 超大值扩展最大值
   └─ 确保覆盖所有可能的UFT值

4. 自适应分裂
   ├─ 当单个分组范围过宽时分裂
   ├─ 使用几何平均确定分裂点
   ├─ 后续新值自动流向合适分组
   └─ 最终形成优化的状态空间

5. Q-Learning集成
   ├─ 状态ID直接来自分组ID
   ├─ Hash表追踪状态-动作对
   ├─ 批量更新所有Q值
   └─ 不同状态独立学习策略
```

### ✅ 执行流程总结

```
GC处理有效页
  ↓
get_usable_state(tags, aver_uft)
  ├─ 计算UFT = (now - time) / cnt
  └─ 调用get_uft_flag(uft)
      ↓
      discretize_uft_value(uft)
      ├─ 二分查找找合适分组
      ├─ 如需要，扩展分组范围
      ├─ 更新分组样本计数
      ├─ 检查是否需要分裂
      └─ 返回状态ID(0-63)
  ↓
状态ID用于Q表查询和动作选择
  ↓
结果记录在Hash表中
  ↓
GC完成后批量更新Q值
```

