# Epsilon 衰减策略改进说明

## 问题
原始策略：`epsilon = num_trainings - Qlearning_count`
- 会导致 epsilon 最终衰减到 1
- 完全禁用探索性，无法应对运行时环境变化

## 改进方案

### 新的 epsilon 衰减曲线

```
epsilon
  ↑
  │  ╱───────────────────┐
  │ ╱                     │
  │╱                      │ MIN_EPSILON
  └───────────────────────┴─────→ Qlearning_count
  0   训练期间          训练完成   生产运行
```

### 数学模型

**训练期间** (Qlearning_count < num_trainings):
```
ε(t) = MIN_EPSILON + (num_trainings - MIN_EPSILON) × (num_trainings - t) / num_trainings

其中：
  - t = Qlearning_count (当前训练步数)
  - MIN_EPSILON = num_trainings / 20 = 50000 / 20 = 2500（5%的探索概率）
```

**训练完成后** (Qlearning_count ≥ num_trainings):
```
ε = MIN_EPSILON (保持不变)
```

### 具体示例

假设 `num_trainings = 50000`：

| 训练进度 | Qlearning_count | epsilon | 探索概率 | 利用概率 | 说明 |
|---------|-----------------|---------|---------|---------|------|
| 0% | 0 | 50000 | 100% | 0% | 完全探索 |
| 25% | 12500 | 37500 | 75% | 25% | 探索为主 |
| 50% | 25000 | 25000 | 50% | 50% | 平衡 |
| 75% | 37500 | 12500 | 25% | 75% | 利用为主 |
| 100% | 50000 | 2500 | 5% | 95% | 完成训练 |
| 训练后 | >50000 | 2500 | 5% | 95% | 生产运行（永久保留探索） |

## 为什么需要永久保留 MIN_EPSILON（5%）？

### 1. 适应运行时环境变化
- Flash 存储的磨损随时间变化
- 新块/坏块可能出现
- 系统负载模式可能改变
- 5%的探索概率足以发现最优策略的轻微偏移

### 2. 避免局部最优
- 完全利用可能陷入局部最优
- 即使已训练收敛，环境变化也需要适应

### 3. 鲁棒性考虑
- 应对突发的高负载或低负载情况
- 保持对新状态-动作对的发现能力

## 代码实现

```c
#define MIN_EPSILON (num_trainings / 20)  /* 保留5%的探索概率 */

if(Qlearning_count < num_trainings) {
    /* 线性插值：从 num_trainings 衰减到 MIN_EPSILON */
    unsigned decay_range = num_trainings - MIN_EPSILON;
    epsilon = MIN_EPSILON + (decay_range * (num_trainings - Qlearning_count)) / num_trainings;
} else {
    /* 训练完成后，保持 MIN_EPSILON 的探索概率 */
    epsilon = MIN_EPSILON;
}
```

## 关键参数调整

如需改变永久保留的探索概率，修改：
```c
#define MIN_EPSILON (num_trainings / K)  /* K 越小，探索概率越高 */
```

- `K = 20` → 5%探索（当前）
- `K = 10` → 10%探索（高探索）
- `K = 50` → 2%探索（高利用）

## 调试输出

在训练过程中会输出：
```
[QL-GC] Trained 10000/50000 | gc_copies: 1523 | epsilon: 40000
[QL-GC] Trained 25000/50000 | gc_copies: 2156 | epsilon: 25000
[QL-GC] Trained 50000/50000 | gc_copies: 2847 | epsilon: 2500  ← 锁定在 MIN_EPSILON
```

---
修复日期：2025年12月3日  
改进内容：保留永久探索概率，确保生产环境的鲁棒性
