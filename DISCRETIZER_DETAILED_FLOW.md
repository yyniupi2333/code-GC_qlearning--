# 离散器具体执行流程详解

## 场景：处理一个受害块中的5个有效页

```
受害块中的5个有效页，UFT值分别为：
Page-1: UFT = 600
Page-2: UFT = 5000
Page-3: UFT = 100
Page-4: UFT = 18000
Page-5: UFT = 3000
```

---

## 第一步：处理 Page-1（UFT=600）

### 调用链
```
GC处理页面
  ↓
get_usable_state(tags, aver_uft)
  ↓
get_uft_flag(uft=600, aver_uft)
  ↓
discretize_uft_value(uft_value=600)
```

### discretize_uft_value(600) 执行过程

**状态检查**：
```c
if (uft_discretizer.total_samples == 0) {  // 第一个样本
    uft_discretizer.groups[0].min_val = 600;
    uft_discretizer.groups[0].max_val = 600;
    uft_discretizer.groups[0].sample_count = 1;
    uft_discretizer.total_samples = 1;
    return 0;
}
```

**离散器状态**：
```
AFTER Page-1:
  group_count = 1
  total_samples = 1
  groups[0] = {min=600, max=600, count=1}
  
返回状态ID = 0
```

**Q-Learning**：
```
state_id = 0
allocate_block(action_based_on_state_0)
record_in_hash_table(state=0, action=..., block_id=...)
```

---

## 第二步：处理 Page-2（UFT=5000）

### discretize_uft_value(5000) 执行过程

**状态检查**：
```c
if (uft_discretizer.total_samples == 0) {  // 不是第一个了
    // 跳过
}

uft_discretizer.total_samples++;  // 变为2
```

**二分查找**：
```
left = 0
right = (s32)uft_discretizer.group_count - 1 = 0

while (left <= right) {  // 0 <= 0，进入
    mid = 0 + (0 - 0) / 2 = 0
    group_min = groups[0].min_val = 600
    group_max = groups[0].max_val = 600
    
    if (5000 >= 600 && 5000 <= 600) {  // FALSE
        // 精确匹配失败
    }
    else if (5000 < 600) {  // FALSE
        right = -1
    }
    else {  // TRUE
        left = 1
    }
}
// 循环条件：1 <= 0 是FALSE，退出

best_group = 0（初始值未改）
```

**范围检查**：
```c
if (5000 < groups[0].min_val || 5000 > groups[0].max_val) {
    // 5000 > 600，条件成立
    
    if (left < (s32)group_count) {  // 1 < 1 是FALSE
        best_group = left;
    }
    else {  // TRUE
        best_group = group_count - 1 = 0
    }
}

// best_group = 0
```

**扩展分组范围**：
```c
if (groups[0].sample_count == 0) {  // 不是，是1
    // 跳过
}
else {
    if (5000 < groups[0].min_val) {  // 5000 < 600 是FALSE
        // 跳过
    }
    if (5000 > groups[0].max_val) {  // 5000 > 600 是TRUE
        groups[0].max_val = 5000;  // 扩展！
    }
}
groups[0].sample_count++;  // 变为2
```

**分裂检查**：
```c
if (groups[0].sample_count >= 100 &&      // 2 >= 100 是FALSE
    group_count < 64 &&
    groups[0].min_val > 0) {
    // 不满足分裂条件，跳过
}
```

**返回**：
```
discretizer_print_count++;  // 变为2
if (discretizer_print_count % 100 == 0) {  // 2 % 100 != 0
    // 不打印
}
return 0 % 64 = 0;
```

**离散器状态**：
```
AFTER Page-2:
  group_count = 1（不变）
  total_samples = 2
  groups[0] = {min=600, max=5000, count=2}
  
返回状态ID = 0
```

**Q-Learning**：
```
state_id = 0（仍然是同一个分组）
allocate_block(action_based_on_state_0)
record_in_hash_table(state=0, action=..., block_id=...)
```

---

## 第三步：处理 Page-3（UFT=100）

### discretize_uft_value(100) 执行过程

**状态检查**：
```c
uft_discretizer.total_samples++;  // 变为3
```

**二分查找**：
```
left = 0
right = 0

while (0 <= 0) {
    mid = 0
    group_min = 600
    group_max = 5000
    
    if (100 >= 600 && 100 <= 5000) {  // FALSE
        // 精确匹配失败
    }
    else if (100 < 600) {  // TRUE
        right = -1  // 向左搜索
    }
}
// 循环条件：0 <= -1 是FALSE，退出

best_group = 0
```

**范围检查**：
```c
if (100 < groups[0].min_val || 100 > groups[0].max_val) {
    // 100 < 600，条件成立
    
    if (left < group_count) {  // 0 < 1 是TRUE
        best_group = left = 0
    }
}

// best_group = 0
```

**扩展分组范围**：
```c
if (groups[0].sample_count == 0) {  // 否
}
else {
    if (100 < groups[0].min_val) {  // 100 < 600 是TRUE
        groups[0].min_val = 100;  // 扩展最小值！
    }
    if (100 > groups[0].max_val) {  // 100 > 5000 是FALSE
    }
}
groups[0].sample_count++;  // 变为3
```

**分裂检查**：
```c
if (groups[0].sample_count >= 100 &&  // 3 >= 100 是FALSE
    group_count < 64 &&
    groups[0].min_val > 0) {
    // 不分裂
}
```

**返回**：
```
discretizer_print_count++;  // 变为3
return 0;
```

**离散器状态**：
```
AFTER Page-3:
  group_count = 1（不变）
  total_samples = 3
  groups[0] = {min=100, max=5000, count=3}
  
返回状态ID = 0
```

---

## 第四步：处理 Page-4（UFT=18000）

### discretize_uft_value(18000) 执行过程

**二分查找** → **范围检查** → **扩展**：
```c
groups[0].max_val = 18000;  // 进一步扩展最大值
groups[0].sample_count++;   // 变为4
```

**分裂检查**：
```c
if (groups[0].sample_count >= 100 &&  // 4 >= 100 是FALSE
    group_count < 64 &&
    groups[0].min_val > 0) {
    // 仍然不分裂（样本不足）
}
```

**离散器状态**：
```
AFTER Page-4:
  group_count = 1（仍然不变）
  total_samples = 4
  groups[0] = {min=100, max=18000, count=4}
  
返回状态ID = 0
```

---

## 第五步：处理 Page-5（UFT=3000）

### discretize_uft_value(3000) 执行过程

**二分查找**：
```c
if (3000 >= 100 && 3000 <= 18000) {  // TRUE
    best_group = 0;
    break;  // 精确匹配成功
}
```

**扩展范围**：
```c
// 3000已经在范围内[100, 18000]，范围不需扩展
if (3000 < 100) {  // FALSE
}
if (3000 > 18000) {  // FALSE
}
groups[0].sample_count++;  // 变为5
```

**离散器状态**：
```
AFTER Page-5:
  group_count = 1（仍然只有1个分组）
  total_samples = 5
  groups[0] = {min=100, max=18000, count=5}
  
返回状态ID = 0
```

---

## 受害块处理总结

```
处理5个页面后：
├─ Page-1: UFT=600   → 初始化Group-0 [600,600]
├─ Page-2: UFT=5000  → 扩展到 [600,5000]
├─ Page-3: UFT=100   → 扩展到 [100,5000]
├─ Page-4: UFT=18000 → 扩展到 [100,18000]
└─ Page-5: UFT=3000  → 在范围内，不扩展

所有5个页面都返回状态ID=0（因为只有1个分组）

Hash表记录：
├─ (state=0, action=..., block_id=...)
├─ (state=0, action=..., block_id=...)
├─ (state=0, action=..., block_id=...)
├─ (state=0, action=..., block_id=...)
└─ (state=0, action=..., block_id=...)

当前分组范围：[100, 18000]
范围检查：18000 >= 2*100? → 18000 >= 200? YES，违反条件
但样本数=5 < 100，所以不分裂
```

---

## 继续到样本数=100时的分裂

```
假设继续处理95个新页面...

当样本数达到100时：

Sample 100:
if (groups[0].sample_count >= 100 &&     // TRUE
    uft_discretizer.group_count < 64 &&  // TRUE
    groups[0].min_val > 0) {              // TRUE
    
    s64 group_min = 100;
    s64 group_max = 18000;
    
    int violates_condition = (18000 >= 2 * 100);  // TRUE
    
    if (violates_condition) {
        /* 分裂执行 */
        s64 split_point = geometric_mean(100, 18000);
        
        计算过程：
        log2(100) ≈ 6.64
        log2(18000) ≈ 14.14
        avg = (6.64 + 14.14) / 2 = 10.39
        split_point = 2^10.39 ≈ 1346
        
        新分组：
        groups[1].min_val = 1346;
        groups[1].max_val = 18000;
        groups[1].sample_count = 0;
        
        旧分组调整：
        groups[0].max_val = 1346;  // [100, 1346]
        
        uft_discretizer.group_count++;  // 变为2
    }
}
```

---

## 关键流程特点

### ✅ **特点1：所有值都被分配**
```
没有"不属于任何分组"的值
每个新值要么精确匹配现有分组，要么扩展某个分组
```

### ✅ **特点2：分裂是基于范围宽度，不是新值出现**
```
分裂触发：样本数 >= 100 AND 范围过宽(max >= 2*min)
不是：当某个新UFT值出现时
```

### ✅ **特点3：分裂不会立即改变现有样本的状态ID**
```
Sample 100到达时分裂成[100,1346]和[1346,18000]
Sample 1~99的状态ID仍然是0
Sample 101+的新样本才会根据值落入新分组0或1
```

### ✅ **特点4：二分查找高效查找分组**
```
O(log group_count) 时间复杂度
64个分组时仅需6次比较
```

### ✅ **特点5：范围重叠存在**
```
分裂点1346同时在两个分组的边界
[100,1346]和[1346,18000]有重叠
但线性查找顺序保证精确分配
```

