# IMU 健身动作识别 BP 神经网络训练、验证、测试与 ESP32-S3 移植方案

> 适用项目：ESP32-S3 可编程手表 + 六轴 IMU + PC 端上位机
> 适用数据集：`G1ow9711/IMU_Datasrt` 仓库中的 `imu_dataset_for_final`
> 目标：训练一个可移植到 ESP32-S3 的轻量级 BP 神经网络模型，独立测试集准确率目标设为 **95% 以上**。
> 重要说明：95% 以上必须以本地实际运行后的 **独立测试集 accuracy** 和 **混淆矩阵** 为准，不能只看训练集或验证集。本文给出的脚本会把 95% 作为部署门槛；未达到时不建议导出 ESP32 头文件。

---

## 1. 数据集现状与建模思路

根据仓库说明，数据集中包含三轴陀螺仪、三轴加速度计数据，共 11 类动作：

| 英文文件夹名 | 中文含义 | 备注 |
|---|---|---|
| `walk` | 行走 | 周期性动作 |
| `trot` | 慢跑 | 周期性动作，运动幅值较大 |
| `sit` | 静坐 | 静态动作，可作为休息段参考 |
| `wave` | 挥手 | 手臂局部动作 |
| `squat` | 深蹲 | 健身计数动作 |
| `jumping_jack` | 开合跳 | 高动态周期动作 |
| `good_morning` | 早安式弯腰 | 躯干屈伸类动作 |
| `lunge` | 箭步蹲 | 单腿动作 |
| `jumping_lunge` | 跳跃箭步蹲 | 高动态动作 |
| `jumping_squat` | 跳跃深蹲 | 高动态动作 |
| `tuck_jump` | 抬高跳 | 高动态动作 |

每个数据文件为 `N 行 × 8 列`。前 3 列是陀螺仪 `gx, gy, gz`，第 4～6 列是加速度计 `ax, ay, az`，最后 2 列是时间戳占位，目前为 0。采样率为 25Hz。加速度计原始值换算方式为 `raw / 4096 = g`，陀螺仪原始值换算方式为 `raw / 16.4 = °/s`。

本项目最终需要在 ESP32-S3 上实时运行，因此不建议直接使用大型 CNN、LSTM 或 Transformer。推荐采用：

```text
原始六轴 IMU 数据 → 固定长度滑动窗口 → 手工特征提取 → 标准化 → 小型 BP 神经网络 → 动作类别 → 状态机计数
```

这样做的优点是：

1. 模型小，适合 ESP32-S3 部署。
2. 推理只需要几千到几万次浮点乘加，实时性足够。
3. 特征提取逻辑可以在 Python 和 ESP32 端保持一致。
4. 比端侧 LSTM/CNN 更容易解释、调试和复现。

---

## 2. 训练目标与验收标准

### 2.1 最终目标

模型最终目标不是“训练集准确率 95%”，而是：

```text
独立测试集准确率 test_accuracy ≥ 95%
```

同时需要满足：

```text
宏平均 F1-score macro_f1 ≥ 95%
```

原因是本项目有 11 个动作类别，如果只看 overall accuracy，可能会出现某些动作识别很好、某些动作识别很差的问题。宏平均 F1-score 会更关注每个类别的平均表现。

### 2.2 验收文件

训练完成后至少保存以下文件：

```text
outputs/
├── best_model.pt                 # PyTorch 最佳模型权重
├── scaler_and_config.npz          # 特征标准化均值、标准差、类别名、窗口长度
├── training_report.json           # 训练、验证、测试指标
├── confusion_matrix.png           # 测试集混淆矩阵图片
└── esp32_bp_model.h               # 达到 95% 后导出的 ESP32 头文件
```

### 2.3 不能使用的数据划分方式

不能把同一个原始 IMU 文件切出来的多个窗口同时放进训练集和测试集。这样会导致数据泄漏，测试准确率虚高。

错误做法：

```text
先把所有文件切成窗口 → 再随机划分窗口
```

正确做法：

```text
先按原始 txt 文件划分训练 / 验证 / 测试 → 再分别切窗口
```

本文代码会按照“原始文件级别”划分，避免同一段采集数据同时出现在训练和测试中。

---

## 3. PyCharm 项目结构设计

为了可复现，同时避免代码文件太多，建议整个训练工程控制在 3 个核心文件以内。

```text
IMU_BP_Project/
├── imu_dataset_for_final/         # 从 GitHub 下载后的数据集文件夹
│   ├── squat/
│   ├── jumping_jack/
│   ├── walk/
│   └── ...
├── train_export.py                # 文件 1：训练、验证、测试、导出全部放这里
├── requirements.txt               # 文件 2：Python 依赖版本
└── esp32_bp_model.h               # 文件 3：训练成功后自动生成，复制到 ESP32 工程
```

其中：

- `train_export.py` 是唯一主要 Python 脚本。
- `requirements.txt` 用来固定依赖版本，保证复现。
- `esp32_bp_model.h` 由训练脚本自动生成，不需要手写权重数组。

---

## 4. PyCharm 操作流程

### 4.1 新建项目

1. 打开 PyCharm。
2. 选择 `New Project`。
3. 项目类型选择 `Pure Python`。
4. 项目路径建议设置为：

```text
D:\IMU_BP_Project
```

5. Python 解释器建议选择虚拟环境 `venv`。

### 4.2 放置数据集

将 GitHub 仓库中的 `imu_dataset_for_final` 文件夹复制到项目根目录，使结构变成：

```text
D:\IMU_BP_Project\imu_dataset_for_final
```

不要只复制某几个动作文件夹，否则类别数量会发生变化，训练结果不可复现。

### 4.3 安装依赖

在 PyCharm 底部 Terminal 中执行：

```bash
pip install -r requirements.txt
```

### 4.4 运行训练

在 PyCharm 中右键 `train_export.py`，选择：

```text
Run 'train_export'
```

或者在 Terminal 中执行：

```bash
python train_export.py
```

---

## 5. 训练、验证、测试总体方案

### 5.1 数据预处理

对每个 txt 文件执行以下步骤：

```text
读取 txt → 解析前 6 列 → 原始值单位换算 → 滑动窗口切分 → 休息段过滤 → 特征提取
```

单位换算：

```text
陀螺仪：gx, gy, gz = raw / 16.4，单位 °/s
加速度：ax, ay, az = raw / 4096.0，单位 g
```

### 5.2 滑动窗口

由于采样率为 25Hz，推荐测试 3 种窗口长度：

| 窗口秒数 | 样本点数 | 说明 |
|---|---:|---|
| 1.5 秒 | 38 点 | 响应快，但动作周期信息略少 |
| 2.0 秒 | 50 点 | 推荐默认值，实时性和稳定性平衡较好 |
| 2.5 秒 | 62 点 | 信息更多，但显示延迟稍大 |

步长统一设置为 0.5 秒，即大约 12 点。这样每 0.5 秒给出一次动作识别结果，适合上位机实时动画显示。

### 5.3 休息段过滤

数据集中部分高强度动作文件存在：

```text
做动作 → 休息 → 做动作
```

如果把中间休息段也标成该动作，会造成模型混淆。解决方法：

1. 用 `sit` 类训练文件估计静止状态的运动强度阈值。
2. 对非 `sit` 类动作，过滤掉运动强度低于阈值的窗口。
3. 对 `sit` 类动作，保留低运动强度窗口，去掉明显的起身或晃动窗口。

运动强度可以定义为：

```text
motion_score = std(acc_mag) + std(gyro_mag) / 200
```

其中：

```text
acc_mag  = sqrt(ax² + ay² + az²)
gyro_mag = sqrt(gx² + gy² + gz²)
```

### 5.4 特征提取

为了 ESP32-S3 移植方便，不使用复杂频域特征，优先使用时域特征。

对以下 8 组序列提取特征：

```text
gx, gy, gz, ax, ay, az, gyro_mag, acc_mag
```

每组序列提取 10 个特征：

```text
mean           均值
std            标准差
min            最小值
max            最大值
range          极差
rms            均方根
mean_abs       绝对值均值
energy         平均能量
mean_abs_diff  相邻点平均绝对差
zcr            去均值后的过零率
```

因此总特征维度为：

```text
8 组序列 × 10 个特征 = 80 维
```

### 5.5 模型结构

推荐 BP 神经网络结构：

```text
80 输入 → 96 隐藏层 → 64 隐藏层 → 32 隐藏层 → 11 输出
```

激活函数使用 ReLU，训练阶段可以使用 Dropout 防止过拟合，导出到 ESP32 时 Dropout 自动失效。

该模型参数规模约为：

```text
80×96 + 96×64 + 64×32 + 32×11 ≈ 16224 个权重
```

使用 float32 存储大约 65KB，ESP32-S3 可以承受；如果后期需要降低存储和功耗，可进一步做 int8 量化。

---

## 6. 95% 以上准确率的实现策略

为了尽可能达到 95% 以上，方案中采用以下策略：

1. **按文件划分数据集**，避免窗口级随机划分造成数据泄漏。
2. **过滤休息段**，减少“动作文件中的静止窗口”污染标签。
3. **统一单位换算**，保证训练数据和 ESP32 实时数据尺度一致。
4. **多窗口长度实验**，自动比较 1.5 秒、2.0 秒、2.5 秒窗口效果。
5. **训练集数据增强**，只对训练集做轻微噪声、缩放、时间平移，验证集和测试集绝不增强。
6. **Early Stopping**，用验证集选择最佳模型，避免过拟合。
7. **独立测试集一次性评估**，最终报告只以测试集结果为准。
8. **低于 95% 不导出 ESP32 部署头文件**，防止把不合格模型移植到硬件端。

如果首次训练没有达到 95%，优先调整顺序如下：

```text
第一步：检查混淆矩阵，找出混淆动作
第二步：增大窗口到 2.5 秒
第三步：增加训练轮数或隐藏层神经元数量
第四步：重新采集混淆严重动作的数据
第五步：把静止段单独作为 rest 类，而不是混入动作类
```

---

## 7. ESP32-S3 移植设计

### 7.1 Python 端导出内容

训练完成并达到 95% 后，Python 会自动导出：

```text
esp32_bp_model.h
```

该头文件包含：

```text
类别名称
窗口长度
特征均值
特征标准差
BP 网络全部权重和偏置
特征提取函数
前向推理函数
softmax 置信度计算函数
```

### 7.2 ESP32 端实时流程

ESP32 端程序逻辑如下：

```text
1. 以 25Hz 或更高频率读取 QMI8658 六轴 IMU
2. 如果采样率高于 25Hz，先降采样或重新训练对应采样率模型
3. 保存最近 WINDOW_LEN 个点到环形缓冲区
4. 每隔 0.5 秒取出一个窗口
5. 调用 extract_features_from_window()
6. 调用 bp_predict_from_window()
7. 得到动作类别和置信度
8. 输入状态机计数算法
9. 通过 BLE 发给 PC 上位机
```

### 7.3 采样率一致性

数据集采样率是 25Hz。ESP32 实际采样时必须注意：

- 最简单方案：ESP32 也固定 25Hz 采样。
- 如果 ESP32 用 50Hz 或 100Hz 采样，需要先重新采集训练数据，或在端侧降采样到 25Hz。
- 不建议训练用 25Hz，实际推理直接用 100Hz，否则窗口内数据分布会变化，准确率会下降。

### 7.4 输入单位一致性

ESP32 端传入模型前，必须使用和 Python 完全一致的单位：

```cpp
gyro_dps = gyro_raw / 16.4f;
acc_g    = acc_raw  / 4096.0f;
```

如果你在 ESP32 驱动中已经直接拿到了 `g` 和 `°/s`，就不要重复除以比例系数。

---

## 8. 文件 1：`requirements.txt`

```txt
numpy==1.26.4
scikit-learn==1.5.2
matplotlib==3.9.2
torch==2.5.1
```

说明：

- 如果安装 `torch==2.5.1` 失败，可以先到 PyTorch 官网根据系统和 CUDA 情况选择安装命令。
- 仅使用 CPU 训练也可以，本项目模型很小，不依赖 GPU。
- 为了复现，建议不要随意升级依赖版本。

---

## 9. 文件 2：`train_export.py`

> 下面代码尽量采用单文件写法，所有关键步骤都有中文注释。
> 运行前请确认 `imu_dataset_for_final` 文件夹和 `train_export.py` 在同一级目录。

```python
# 导入 os 模块，用于设置环境变量，增强实验复现性
import os

# 导入 random 模块，用于固定 Python 自带随机数
import random

# 导入 json 模块，用于保存训练报告
import json

# 从 pathlib 导入 Path，用于更清晰地处理文件路径
from pathlib import Path

# 导入 numpy，用于数值计算和 IMU 特征提取
import numpy as np

# 导入 torch，用于搭建和训练 BP 神经网络
import torch

# 从 torch 导入神经网络模块 nn
from torch import nn

# 从 torch 导入数据集和数据加载器
from torch.utils.data import TensorDataset, DataLoader

# 从 sklearn 导入训练测试划分函数
from sklearn.model_selection import train_test_split

# 从 sklearn 导入分类评估指标
from sklearn.metrics import accuracy_score, f1_score, classification_report, confusion_matrix

# 导入 matplotlib，用于保存混淆矩阵图片
import matplotlib.pyplot as plt


# =========================
# 1. 全局配置区
# =========================

# 设置随机种子，保证同一环境下尽量复现相同结果
SEED = 20260709

# 设置数据集文件夹名称，要求该文件夹和本脚本在同一级目录
DATASET_DIR = Path("imu_dataset_for_final")

# 设置输出文件夹，训练结果、报告、模型和 ESP32 头文件都会放到这里
OUTPUT_DIR = Path("outputs")

# 设置 IMU 数据采样率，仓库 ReadMe 中说明为 25Hz
SAMPLE_RATE = 25

# 设置窗口步长秒数，0.5 秒输出一次识别结果，适合实时显示
STEP_SECONDS = 0.5

# 设置需要自动尝试的窗口长度，单位为秒
WINDOW_SECONDS_LIST = [1.5, 2.0, 2.5]

# 设置训练集比例，剩余 30% 会继续划分为验证集和测试集
TRAIN_RATIO = 0.70

# 设置验证集比例，占全部原始文件的 15%
VAL_RATIO = 0.15

# 设置测试集比例，占全部原始文件的 15%
TEST_RATIO = 0.15

# 设置静态动作类别名称，用于估计休息段阈值
SIT_CLASS_NAME = "sit"

# 设置目标测试准确率，低于该值不建议部署到 ESP32
TARGET_TEST_ACC = 0.95

# 设置训练轮数上限，Early Stopping 可能会提前停止
MAX_EPOCHS = 350

# 设置 batch size，模型很小，64 通常足够稳定
BATCH_SIZE = 64

# 设置学习率，AdamW 常用初始值为 0.001
LEARNING_RATE = 1e-3

# 设置权重衰减，用于轻微正则化，降低过拟合风险
WEIGHT_DECAY = 1e-4

# 设置 Early Stopping 容忍轮数，如果验证集长期不提升就停止
PATIENCE = 45

# 设置 Dropout 概率，只在训练阶段生效，导出到 ESP32 时不需要 Dropout
DROPOUT = 0.10

# 设置训练集增强次数，1 表示每个训练窗口额外生成 1 个增强样本
AUGMENT_TIMES = 1

# 设置是否在低于 95% 时仍然导出 ESP32 头文件，默认 False 更安全
EXPORT_WHEN_BELOW_TARGET = False

# 设置六轴通道名称，注意顺序必须和数据读取后的顺序一致
CHANNEL_NAMES = ["gx", "gy", "gz", "ax", "ay", "az"]

# 设置单个序列要提取的特征名称
ONE_SERIES_FEATURES = [
    "mean",          # 均值
    "std",           # 标准差
    "min",           # 最小值
    "max",           # 最大值
    "range",         # 极差
    "rms",           # 均方根
    "mean_abs",      # 绝对值均值
    "energy",        # 平均能量
    "mean_abs_diff", # 相邻点平均绝对差
    "zcr",           # 去均值后的过零率
]

# 固定第一隐藏层神经元数量，便于 ESP32 端生成固定结构 C 代码
HIDDEN1 = 96

# 固定第二隐藏层神经元数量
HIDDEN2 = 64

# 固定第三隐藏层神经元数量
HIDDEN3 = 32


# =========================
# 2. 复现性设置函数
# =========================

# 定义固定随机种子的函数
def set_seed(seed: int) -> None:
    # 固定 Python 自带随机数种子
    random.seed(seed)
    # 固定 NumPy 随机数种子
    np.random.seed(seed)
    # 固定 PyTorch CPU 随机数种子
    torch.manual_seed(seed)
    # 如果有 GPU，则固定 PyTorch GPU 随机数种子
    torch.cuda.manual_seed_all(seed)
    # 设置 Python 哈希种子，减少字典等结构导致的随机差异
    os.environ["PYTHONHASHSEED"] = str(seed)
    # 让 PyTorch 尽可能使用确定性算法
    torch.use_deterministic_algorithms(True, warn_only=True)
    # 关闭 cuDNN 自动寻找最快算法，减少非确定性；CPU 训练时不影响
    torch.backends.cudnn.benchmark = False
    # 设置 cuDNN 尽量确定性；CPU 训练时不影响
    torch.backends.cudnn.deterministic = True


# =========================
# 3. 数据读取与单位换算
# =========================

# 定义读取单个 IMU txt 文件的函数
def read_imu_file(file_path: Path) -> np.ndarray:
    # 创建一个空列表，用于保存每一行解析后的数值
    rows = []
    # 以文本方式打开文件，errors="ignore" 可以跳过异常字符
    with open(file_path, "r", encoding="utf-8", errors="ignore") as f:
        # 逐行读取文件内容
        for line in f:
            # 去掉行首尾空白，并把逗号和制表符统一替换成空格
            line = line.strip().replace(",", " ").replace("\t", " ")
            # 如果当前行为空，则跳过
            if not line:
                continue
            # 按空格切分当前行
            parts = line.split()
            # 创建当前行的浮点数列表
            values = []
            # 遍历切分后的每个字符串
            for item in parts:
                # 尝试把字符串转成浮点数
                try:
                    # 转换成功就加入 values
                    values.append(float(item))
                # 如果遇到非数字内容，就跳过
                except ValueError:
                    # 继续处理下一个字段
                    continue
            # 只要当前行至少包含 6 个有效数值，就认为它包含六轴 IMU 数据
            if len(values) >= 6:
                # 如果不足 8 列，就用 0 补齐到 8 列
                while len(values) < 8:
                    # 添加一个 0 作为缺失时间戳占位
                    values.append(0.0)
                # 只保留前 8 列，防止异常多列影响后续处理
                rows.append(values[:8])
    # 如果文件没有任何有效数据，则抛出异常提醒用户检查数据文件
    if len(rows) == 0:
        # 抛出带文件名的错误，方便定位问题
        raise ValueError(f"文件没有读取到有效 IMU 数据: {file_path}")
    # 将列表转换为 float32 类型的 NumPy 数组，减少内存占用
    data = np.asarray(rows, dtype=np.float32)
    # 取前 3 列作为陀螺仪原始值
    gyro_raw = data[:, 0:3]
    # 取第 4 到第 6 列作为加速度计原始值
    acc_raw = data[:, 3:6]
    # 将陀螺仪原始值换算成 °/s
    gyro_dps = gyro_raw / 16.4
    # 将加速度计原始值换算成 g
    acc_g = acc_raw / 4096.0
    # 按顺序拼接为 gx, gy, gz, ax, ay, az
    imu = np.concatenate([gyro_dps, acc_g], axis=1)
    # 返回换算后的六轴 IMU 数据
    return imu.astype(np.float32)


# =========================
# 4. 数据集扫描与划分
# =========================

# 定义扫描数据集文件夹的函数
def scan_dataset(dataset_dir: Path):
    # 如果数据集文件夹不存在，则直接报错
    if not dataset_dir.exists():
        # 抛出错误并提示正确的文件夹位置
        raise FileNotFoundError(f"没有找到数据集文件夹: {dataset_dir.resolve()}")
    # 创建列表，用于保存每个 txt 文件路径和对应类别名
    records = []
    # 遍历数据集根目录下的所有子文件夹
    for class_dir in sorted(dataset_dir.iterdir()):
        # 只处理文件夹，不处理 ReadMe.txt 等普通文件
        if not class_dir.is_dir():
            # 跳过非文件夹
            continue
        # 获取当前文件夹名称作为动作类别名
        class_name = class_dir.name
        # 找到当前类别文件夹下所有 txt 文件
        txt_files = sorted(class_dir.glob("*.txt"))
        # 遍历当前类别下的所有 txt 文件
        for txt_file in txt_files:
            # 将文件路径和类别名保存为一条记录
            records.append((txt_file, class_name))
    # 如果没有扫描到任何数据文件，则报错
    if len(records) == 0:
        # 抛出错误提示用户检查数据集路径
        raise RuntimeError("没有扫描到任何 txt 数据文件，请检查数据集目录结构。")
    # 根据扫描到的类别名生成类别列表，并排序保证复现
    class_names = sorted(list(set(label for _, label in records)))
    # 生成类别名到数字标签的映射
    label_to_idx = {name: idx for idx, name in enumerate(class_names)}
    # 返回文件记录、类别列表和映射表
    return records, class_names, label_to_idx


# 定义安全的数据划分函数，按原始文件划分而不是按窗口划分
def split_records_by_file(records):
    # 取出每条记录的类别名，用于分层抽样
    labels = [label for _, label in records]
    # 第一次划分：70% 文件用于训练，30% 文件临时保留给验证和测试
    train_records, temp_records, train_labels, temp_labels = train_test_split(
        records, labels, test_size=(1.0 - TRAIN_RATIO), random_state=SEED, stratify=labels
    )
    # 第二次划分：临时集合一分为二，分别作为验证集和测试集
    val_records, test_records, val_labels, test_labels = train_test_split(
        temp_records, temp_labels, test_size=(TEST_RATIO / (VAL_RATIO + TEST_RATIO)), random_state=SEED, stratify=temp_labels
    )
    # 返回三个文件级别的数据集合
    return train_records, val_records, test_records


# =========================
# 5. 窗口切分与休息段判断
# =========================

# 定义把连续 IMU 数据切成固定窗口的函数
def slice_windows(imu: np.ndarray, window_len: int, step_len: int):
    # 如果数据长度小于窗口长度，则没有可用窗口
    if len(imu) < window_len:
        # 直接返回空生成器
        return
    # 从 0 开始，按 step_len 滑动窗口
    for start in range(0, len(imu) - window_len + 1, step_len):
        # 计算当前窗口结束位置
        end = start + window_len
        # 取出当前窗口数据
        yield imu[start:end]


# 定义计算窗口运动强度的函数，用于过滤休息段
def compute_motion_score(window: np.ndarray) -> float:
    # 取出三轴陀螺仪数据
    gyro = window[:, 0:3]
    # 取出三轴加速度数据
    acc = window[:, 3:6]
    # 计算每个采样点的加速度模长
    acc_mag = np.sqrt(np.sum(acc * acc, axis=1))
    # 计算每个采样点的陀螺仪模长
    gyro_mag = np.sqrt(np.sum(gyro * gyro, axis=1))
    # 用加速度模长标准差和陀螺仪模长标准差共同衡量运动强度
    score = float(np.std(acc_mag) + np.std(gyro_mag) / 200.0)
    # 返回运动强度分数
    return score


# 定义根据训练集中的 sit 类估计休息段阈值的函数
def estimate_rest_threshold(train_records, window_len: int, step_len: int) -> float:
    # 创建列表，用于保存 sit 类窗口的运动强度
    sit_scores = []
    # 遍历训练集文件记录
    for file_path, label in train_records:
        # 只使用 sit 类来估计静止阈值
        if label != SIT_CLASS_NAME:
            # 非 sit 类直接跳过
            continue
        # 读取当前 sit 文件
        imu = read_imu_file(file_path)
        # 对当前文件切窗口
        for window in slice_windows(imu, window_len, step_len):
            # 计算当前窗口运动强度并保存
            sit_scores.append(compute_motion_score(window))
    # 如果训练集中没有 sit 类窗口，则使用经验阈值
    if len(sit_scores) == 0:
        # 返回一个偏保守的默认阈值
        return 0.06
    # 将列表转换为 NumPy 数组
    sit_scores = np.asarray(sit_scores, dtype=np.float32)
    # 使用 sit 类 95 分位数作为基础阈值，再乘以 1.15 留出余量
    threshold = float(np.percentile(sit_scores, 95) * 1.15)
    # 阈值不宜过小，否则会误保留大量休息段
    threshold = max(threshold, 0.035)
    # 返回最终休息阈值
    return threshold


# =========================
# 6. 特征提取
# =========================

# 定义对单个一维序列提取特征的函数
def extract_one_series_features(x: np.ndarray) -> list:
    # 将输入转换为 float32，保证数值类型一致
    x = x.astype(np.float32)
    # 计算均值
    mean = float(np.mean(x))
    # 计算标准差
    std = float(np.std(x))
    # 计算最小值
    min_v = float(np.min(x))
    # 计算最大值
    max_v = float(np.max(x))
    # 计算极差
    range_v = max_v - min_v
    # 计算均方根
    rms = float(np.sqrt(np.mean(x * x)))
    # 计算绝对值均值
    mean_abs = float(np.mean(np.abs(x)))
    # 计算平均能量
    energy = float(np.mean(x * x))
    # 如果序列长度大于 1，则可以计算相邻点差分
    if len(x) > 1:
        # 计算相邻点平均绝对差
        mean_abs_diff = float(np.mean(np.abs(np.diff(x))))
    # 如果序列长度不够，则差分特征设为 0
    else:
        # 设置差分特征为 0
        mean_abs_diff = 0.0
    # 将序列去均值，便于计算过零率
    x_centered = x - mean
    # 如果序列长度大于 1，则计算相邻点是否发生符号变化
    if len(x_centered) > 1:
        # 计算过零率
        zcr = float(np.mean((x_centered[:-1] * x_centered[1:]) < 0))
    # 如果序列长度不够，则过零率设为 0
    else:
        # 设置过零率为 0
        zcr = 0.0
    # 按固定顺序返回 10 个特征
    return [mean, std, min_v, max_v, range_v, rms, mean_abs, energy, mean_abs_diff, zcr]


# 定义提取单个 IMU 窗口 80 维特征的函数
def extract_features(window: np.ndarray) -> np.ndarray:
    # 创建列表，用于保存所有特征
    features = []
    # 先对原始六轴分别提取特征
    for axis_idx in range(6):
        # 取出当前轴的一维序列
        series = window[:, axis_idx]
        # 提取当前轴的 10 个特征并加入总特征列表
        features.extend(extract_one_series_features(series))
    # 取出三轴陀螺仪数据
    gyro = window[:, 0:3]
    # 取出三轴加速度数据
    acc = window[:, 3:6]
    # 计算陀螺仪模长序列
    gyro_mag = np.sqrt(np.sum(gyro * gyro, axis=1))
    # 计算加速度模长序列
    acc_mag = np.sqrt(np.sum(acc * acc, axis=1))
    # 提取陀螺仪模长的 10 个特征
    features.extend(extract_one_series_features(gyro_mag))
    # 提取加速度模长的 10 个特征
    features.extend(extract_one_series_features(acc_mag))
    # 转换为 float32 数组并返回
    return np.asarray(features, dtype=np.float32)


# 定义生成特征名的函数，便于报告和 ESP32 对齐
def build_feature_names() -> list:
    # 创建特征名列表
    names = []
    # 遍历六轴名称
    for name in CHANNEL_NAMES:
        # 遍历单序列特征名称
        for feat in ONE_SERIES_FEATURES:
            # 拼接成类似 gx_mean 的特征名
            names.append(f"{name}_{feat}")
    # 对陀螺仪模长补充特征名
    for feat in ONE_SERIES_FEATURES:
        # 拼接 gyro_mag 特征名
        names.append(f"gyro_mag_{feat}")
    # 对加速度模长补充特征名
    for feat in ONE_SERIES_FEATURES:
        # 拼接 acc_mag 特征名
        names.append(f"acc_mag_{feat}")
    # 返回全部特征名
    return names


# =========================
# 7. 训练集数据增强
# =========================

# 定义对原始窗口做轻量增强的函数，只用于训练集
def augment_window(window: np.ndarray) -> np.ndarray:
    # 复制窗口，避免修改原始数据
    aug = window.copy()
    # 生成六轴随机缩放系数，模拟佩戴松紧和个体差异
    scale = np.random.uniform(0.96, 1.04, size=(1, 6)).astype(np.float32)
    # 应用随机缩放
    aug = aug * scale
    # 定义六轴噪声标准差，陀螺仪单位为 °/s，加速度单位为 g
    noise_std = np.array([1.0, 1.0, 1.0, 0.01, 0.01, 0.01], dtype=np.float32)
    # 生成高斯噪声
    noise = np.random.normal(0.0, noise_std, size=aug.shape).astype(np.float32)
    # 加入噪声
    aug = aug + noise
    # 随机平移 -2 到 2 个采样点，模拟窗口起点变化
    shift = random.randint(-2, 2)
    # 沿时间轴滚动窗口
    aug = np.roll(aug, shift=shift, axis=0)
    # 返回增强后的窗口
    return aug.astype(np.float32)


# =========================
# 8. 根据文件列表构造特征数据集
# =========================

# 定义从原始文件记录构造 X、y 的函数
def build_xy_from_records(records, label_to_idx, window_len: int, step_len: int, rest_threshold: float, is_train: bool):
    # 创建特征列表
    X = []
    # 创建标签列表
    y = []
    # 创建每个窗口来源文件列表，便于排查问题
    source_files = []
    # 遍历所有文件记录
    for file_path, label_name in records:
        # 将类别名转换为数字标签
        label_idx = label_to_idx[label_name]
        # 读取当前文件的六轴 IMU 数据
        imu = read_imu_file(file_path)
        # 对当前文件进行滑动窗口切分
        for window in slice_windows(imu, window_len, step_len):
            # 计算当前窗口运动强度
            motion_score = compute_motion_score(window)
            # 如果当前类别不是 sit，且运动强度过低，则认为是动作文件中的休息段并过滤
            if label_name != SIT_CLASS_NAME and motion_score < rest_threshold:
                # 跳过当前休息窗口
                continue
            # 如果当前类别是 sit，但运动强度过高，则可能是采集开始或结束时的晃动，也过滤掉
            if label_name == SIT_CLASS_NAME and motion_score > rest_threshold * 4.0:
                # 跳过异常 sit 窗口
                continue
            # 对当前窗口提取 80 维特征
            feat = extract_features(window)
            # 保存特征
            X.append(feat)
            # 保存标签
            y.append(label_idx)
            # 保存来源文件名
            source_files.append(str(file_path))
            # 如果是训练集，则执行数据增强
            if is_train:
                # 按配置重复增强若干次
                for _ in range(AUGMENT_TIMES):
                    # 生成增强窗口
                    aug_win = augment_window(window)
                    # 对增强窗口提取特征
                    aug_feat = extract_features(aug_win)
                    # 保存增强特征
                    X.append(aug_feat)
                    # 保存同样的标签
                    y.append(label_idx)
                    # 保存来源文件名，并标记为增强样本
                    source_files.append(str(file_path) + "::aug")
    # 如果没有得到任何样本，则报错
    if len(X) == 0:
        # 抛出错误提示用户检查阈值或数据路径
        raise RuntimeError("构造数据集失败，没有得到任何窗口样本。")
    # 将特征列表转换为 NumPy 数组
    X = np.asarray(X, dtype=np.float32)
    # 将标签列表转换为 NumPy 数组
    y = np.asarray(y, dtype=np.int64)
    # 返回特征、标签和来源文件
    return X, y, source_files


# =========================
# 9. BP 神经网络模型定义
# =========================

# 定义轻量级 BP 神经网络
class BPNet(nn.Module):
    # 定义初始化函数
    def __init__(self, input_dim: int, num_classes: int):
        # 调用父类初始化函数
        super().__init__()
        # 定义第一全连接层
        self.fc1 = nn.Linear(input_dim, HIDDEN1)
        # 定义第二全连接层
        self.fc2 = nn.Linear(HIDDEN1, HIDDEN2)
        # 定义第三全连接层
        self.fc3 = nn.Linear(HIDDEN2, HIDDEN3)
        # 定义输出层
        self.fc4 = nn.Linear(HIDDEN3, num_classes)
        # 定义 ReLU 激活函数
        self.relu = nn.ReLU()
        # 定义 Dropout，训练时随机丢弃部分神经元，推理时自动关闭
        self.dropout = nn.Dropout(p=DROPOUT)

    # 定义前向传播函数
    def forward(self, x):
        # 第一层线性变换后接 ReLU
        x = self.relu(self.fc1(x))
        # 训练阶段使用 Dropout
        x = self.dropout(x)
        # 第二层线性变换后接 ReLU
        x = self.relu(self.fc2(x))
        # 训练阶段使用 Dropout
        x = self.dropout(x)
        # 第三层线性变换后接 ReLU
        x = self.relu(self.fc3(x))
        # 输出层直接输出 logits，不在这里做 softmax
        x = self.fc4(x)
        # 返回 logits
        return x


# =========================
# 10. 模型训练与评估函数
# =========================

# 定义标准化函数
def standardize_features(X: np.ndarray, mean: np.ndarray, std: np.ndarray) -> np.ndarray:
    # 使用训练集均值和标准差进行标准化
    return ((X - mean) / std).astype(np.float32)


# 定义评估函数
def evaluate_model(model, X: np.ndarray, y: np.ndarray, mean: np.ndarray, std: np.ndarray, device):
    # 将模型设置为评估模式，关闭 Dropout
    model.eval()
    # 对输入特征进行标准化
    X_std = standardize_features(X, mean, std)
    # 将 NumPy 特征转换成 PyTorch 张量
    X_tensor = torch.tensor(X_std, dtype=torch.float32, device=device)
    # 创建空列表保存预测标签
    all_pred = []
    # 创建空列表保存预测概率
    all_prob = []
    # 关闭梯度计算，节省内存和时间
    with torch.no_grad():
        # 前向传播得到 logits
        logits = model(X_tensor)
        # 对 logits 做 softmax 得到概率
        probs = torch.softmax(logits, dim=1)
        # 取最大概率对应的类别作为预测类别
        pred = torch.argmax(probs, dim=1)
        # 将预测类别转回 CPU NumPy 数组
        all_pred = pred.cpu().numpy()
        # 将概率转回 CPU NumPy 数组
        all_prob = probs.cpu().numpy()
    # 计算准确率
    acc = accuracy_score(y, all_pred)
    # 计算宏平均 F1
    macro_f1 = f1_score(y, all_pred, average="macro")
    # 返回准确率、宏平均 F1、预测标签和概率
    return acc, macro_f1, all_pred, all_prob


# 定义训练单个窗口长度实验的函数
def train_one_experiment(window_seconds: float, records, class_names, label_to_idx, device):
    # 将窗口秒数转换为采样点数
    window_len = int(round(window_seconds * SAMPLE_RATE))
    # 将步长秒数转换为采样点数
    step_len = int(round(STEP_SECONDS * SAMPLE_RATE))
    # 打印当前实验窗口长度
    print(f"\n========== 开始实验：窗口 {window_seconds:.1f}s，window_len={window_len}，step_len={step_len} ==========")
    # 按文件级别划分训练集、验证集和测试集
    train_records, val_records, test_records = split_records_by_file(records)
    # 使用训练集中的 sit 类估计休息段阈值
    rest_threshold = estimate_rest_threshold(train_records, window_len, step_len)
    # 打印休息段阈值
    print(f"休息段过滤阈值 rest_threshold = {rest_threshold:.6f}")
    # 构造训练集特征和标签，训练集允许数据增强
    X_train, y_train, train_sources = build_xy_from_records(train_records, label_to_idx, window_len, step_len, rest_threshold, is_train=True)
    # 构造验证集特征和标签，验证集不允许数据增强
    X_val, y_val, val_sources = build_xy_from_records(val_records, label_to_idx, window_len, step_len, rest_threshold, is_train=False)
    # 构造测试集特征和标签，测试集不允许数据增强
    X_test, y_test, test_sources = build_xy_from_records(test_records, label_to_idx, window_len, step_len, rest_threshold, is_train=False)
    # 打印三个集合样本数量
    print(f"训练样本数: {len(X_train)}，验证样本数: {len(X_val)}，测试样本数: {len(X_test)}")
    # 打印特征维度
    print(f"特征维度: {X_train.shape[1]}")
    # 用训练集计算特征均值
    mean = np.mean(X_train, axis=0).astype(np.float32)
    # 用训练集计算特征标准差
    std = np.std(X_train, axis=0).astype(np.float32)
    # 防止标准差过小导致除零
    std = np.where(std < 1e-6, 1.0, std).astype(np.float32)
    # 对训练集特征进行标准化
    X_train_std = standardize_features(X_train, mean, std)
    # 对验证集特征进行标准化
    X_val_std = standardize_features(X_val, mean, std)
    # 创建训练集 TensorDataset
    train_dataset = TensorDataset(torch.tensor(X_train_std, dtype=torch.float32), torch.tensor(y_train, dtype=torch.long))
    # 创建训练集 DataLoader
    train_loader = DataLoader(train_dataset, batch_size=BATCH_SIZE, shuffle=True)
    # 获取输入特征维度
    input_dim = X_train.shape[1]
    # 获取类别数量
    num_classes = len(class_names)
    # 创建 BP 神经网络模型并移动到 device
    model = BPNet(input_dim=input_dim, num_classes=num_classes).to(device)
    # 定义交叉熵损失函数
    criterion = nn.CrossEntropyLoss()
    # 定义 AdamW 优化器
    optimizer = torch.optim.AdamW(model.parameters(), lr=LEARNING_RATE, weight_decay=WEIGHT_DECAY)
    # 定义学习率调度器，当验证损失不下降时自动降低学习率
    scheduler = torch.optim.lr_scheduler.ReduceLROnPlateau(optimizer, mode="min", factor=0.5, patience=15)
    # 初始化最佳验证准确率
    best_val_acc = 0.0
    # 初始化最佳验证 F1
    best_val_f1 = 0.0
    # 初始化最佳验证损失
    best_val_loss = float("inf")
    # 初始化最佳模型参数
    best_state = None
    # 初始化早停计数器
    no_improve_count = 0
    # 将验证集张量提前放到 device
    X_val_tensor = torch.tensor(X_val_std, dtype=torch.float32, device=device)
    # 将验证集标签提前放到 device
    y_val_tensor = torch.tensor(y_val, dtype=torch.long, device=device)
    # 开始训练循环
    for epoch in range(1, MAX_EPOCHS + 1):
        # 设置模型为训练模式，启用 Dropout
        model.train()
        # 初始化当前 epoch 的总损失
        epoch_loss = 0.0
        # 遍历训练 DataLoader 中的每个 batch
        for batch_x, batch_y in train_loader:
            # 将 batch 特征移动到 device
            batch_x = batch_x.to(device)
            # 将 batch 标签移动到 device
            batch_y = batch_y.to(device)
            # 清空优化器中的历史梯度
            optimizer.zero_grad()
            # 前向传播得到 logits
            logits = model(batch_x)
            # 计算交叉熵损失
            loss = criterion(logits, batch_y)
            # 反向传播计算梯度
            loss.backward()
            # 更新模型参数
            optimizer.step()
            # 累加 batch 损失
            epoch_loss += float(loss.item()) * len(batch_x)
        # 计算当前 epoch 的平均训练损失
        epoch_loss = epoch_loss / len(train_dataset)
        # 设置模型为评估模式
        model.eval()
        # 关闭梯度计算，评估验证集
        with torch.no_grad():
            # 计算验证集 logits
            val_logits = model(X_val_tensor)
            # 计算验证集损失
            val_loss = float(criterion(val_logits, y_val_tensor).item())
            # 计算验证集预测标签
            val_pred = torch.argmax(val_logits, dim=1).cpu().numpy()
        # 计算验证集准确率
        val_acc = accuracy_score(y_val, val_pred)
        # 计算验证集宏平均 F1
        val_f1 = f1_score(y_val, val_pred, average="macro")
        # 把验证损失传给学习率调度器
        scheduler.step(val_loss)
        # 每 20 轮打印一次训练状态
        if epoch == 1 or epoch % 20 == 0:
            # 打印当前 epoch 的损失和指标
            print(f"epoch={epoch:03d} train_loss={epoch_loss:.4f} val_loss={val_loss:.4f} val_acc={val_acc:.4f} val_f1={val_f1:.4f}")
        # 判断当前模型是否优于历史最佳模型
        improved = (val_acc > best_val_acc) or (val_acc == best_val_acc and val_f1 > best_val_f1)
        # 如果验证指标提升，则保存当前模型
        if improved:
            # 更新最佳验证准确率
            best_val_acc = val_acc
            # 更新最佳验证 F1
            best_val_f1 = val_f1
            # 更新最佳验证损失
            best_val_loss = val_loss
            # 保存当前模型参数到 CPU，方便后续加载
            best_state = {k: v.cpu().clone() for k, v in model.state_dict().items()}
            # 重置早停计数器
            no_improve_count = 0
        # 如果没有提升，则增加早停计数器
        else:
            # 早停计数加 1
            no_improve_count += 1
        # 如果超过容忍轮数，则提前停止训练
        if no_improve_count >= PATIENCE:
            # 打印早停信息
            print(f"Early Stopping：验证集连续 {PATIENCE} 轮没有提升，停止训练。")
            # 退出训练循环
            break
    # 如果训练过程中保存过最佳模型，则加载最佳模型参数
    if best_state is not None:
        # 加载最佳模型参数
        model.load_state_dict(best_state)
    # 评估最佳模型在验证集上的表现
    val_acc, val_f1, val_pred, val_prob = evaluate_model(model, X_val, y_val, mean, std, device)
    # 评估最佳模型在测试集上的表现
    test_acc, test_f1, test_pred, test_prob = evaluate_model(model, X_test, y_test, mean, std, device)
    # 打印验证和测试结果
    print(f"窗口 {window_seconds:.1f}s 最佳验证 acc={val_acc:.4f}, f1={val_f1:.4f}；测试 acc={test_acc:.4f}, f1={test_f1:.4f}")
    # 把当前实验所有重要结果打包成字典
    result = {
        "window_seconds": window_seconds,
        "window_len": window_len,
        "step_len": step_len,
        "rest_threshold": rest_threshold,
        "model": model,
        "mean": mean,
        "std": std,
        "val_acc": float(val_acc),
        "val_f1": float(val_f1),
        "test_acc": float(test_acc),
        "test_f1": float(test_f1),
        "test_pred": test_pred,
        "test_prob": test_prob,
        "y_test": y_test,
        "train_records": train_records,
        "val_records": val_records,
        "test_records": test_records,
        "train_sample_count": int(len(X_train)),
        "val_sample_count": int(len(X_val)),
        "test_sample_count": int(len(X_test)),
        "input_dim": int(input_dim),
        "num_classes": int(num_classes),
    }
    # 返回当前实验结果
    return result


# =========================
# 11. 混淆矩阵保存
# =========================

# 定义保存混淆矩阵图片的函数
def save_confusion_matrix(y_true, y_pred, class_names, save_path: Path) -> None:
    # 计算混淆矩阵
    cm = confusion_matrix(y_true, y_pred, labels=list(range(len(class_names))))
    # 创建绘图画布
    fig, ax = plt.subplots(figsize=(11, 9))
    # 显示混淆矩阵图像
    im = ax.imshow(cm)
    # 添加颜色条
    fig.colorbar(im, ax=ax)
    # 设置 x 轴刻度
    ax.set_xticks(np.arange(len(class_names)))
    # 设置 y 轴刻度
    ax.set_yticks(np.arange(len(class_names)))
    # 设置 x 轴类别名称
    ax.set_xticklabels(class_names, rotation=45, ha="right")
    # 设置 y 轴类别名称
    ax.set_yticklabels(class_names)
    # 设置 x 轴标题
    ax.set_xlabel("Predicted label")
    # 设置 y 轴标题
    ax.set_ylabel("True label")
    # 设置图片标题
    ax.set_title("Test Confusion Matrix")
    # 遍历混淆矩阵每个格子
    for i in range(cm.shape[0]):
        # 遍历每一列
        for j in range(cm.shape[1]):
            # 在格子中写入数量
            ax.text(j, i, str(cm[i, j]), ha="center", va="center")
    # 自动调整布局，避免标签被遮挡
    fig.tight_layout()
    # 保存图片
    fig.savefig(save_path, dpi=200)
    # 关闭画布，释放内存
    plt.close(fig)


# =========================
# 12. 导出 ESP32 C 头文件
# =========================

# 定义把一维数组转成 C 语言数组字符串的函数
def c_array_1d(name: str, arr: np.ndarray, dtype: str = "float") -> str:
    # 把输入数组转成一维
    flat = arr.reshape(-1)
    # 把每个数格式化为 float 字符串
    values = ", ".join([f"{float(v):.8e}f" for v in flat])
    # 返回 C 语言数组定义
    return f"static const {dtype} {name}[{len(flat)}] = {{{values}}};\n"


# 定义把二维数组转成 C 语言数组字符串的函数
def c_array_2d(name: str, arr: np.ndarray, dtype: str = "float") -> str:
    # 获取二维数组形状
    rows, cols = arr.shape
    # 创建代码行列表
    lines = [f"static const {dtype} {name}[{rows}][{cols}] = {{"]
    # 遍历每一行
    for r in range(rows):
        # 格式化当前行的所有值
        row_values = ", ".join([f"{float(v):.8e}f" for v in arr[r]])
        # 添加当前行到代码列表
        lines.append(f"    {{{row_values}}},")
    # 添加数组结尾
    lines.append("};\n")
    # 返回拼接后的 C 代码字符串
    return "\n".join(lines)


# 定义导出 ESP32 头文件的函数
def export_esp32_header(best_result, class_names, feature_names, save_path: Path) -> None:
    # 从最佳结果中取出模型
    model = best_result["model"]
    # 设置模型为评估模式
    model.eval()
    # 从模型中取出第一层权重
    w1 = model.fc1.weight.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出第一层偏置
    b1 = model.fc1.bias.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出第二层权重
    w2 = model.fc2.weight.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出第二层偏置
    b2 = model.fc2.bias.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出第三层权重
    w3 = model.fc3.weight.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出第三层偏置
    b3 = model.fc3.bias.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出输出层权重
    w4 = model.fc4.weight.detach().cpu().numpy().astype(np.float32)
    # 从模型中取出输出层偏置
    b4 = model.fc4.bias.detach().cpu().numpy().astype(np.float32)
    # 从最佳结果中取出特征均值
    mean = best_result["mean"].astype(np.float32)
    # 从最佳结果中取出特征标准差
    std = best_result["std"].astype(np.float32)
    # 从最佳结果中取出窗口长度
    window_len = int(best_result["window_len"])
    # 从最佳结果中取出输入维度
    feature_dim = int(best_result["input_dim"])
    # 获取类别数量
    class_num = len(class_names)
    # 构造类别名称 C 字符串数组
    class_name_code = "static const char* CLASS_NAMES[CLASS_NUM] = {" + ", ".join([f'"{name}"' for name in class_names]) + "};\n"
    # 构造特征名称 C 字符串数组，主要用于调试，量产时可以删除以节省 Flash
    feature_name_code = "static const char* FEATURE_NAMES[FEATURE_DIM] = {" + ", ".join([f'"{name}"' for name in feature_names]) + "};\n"
    # 创建头文件代码列表
    code = []
    # 添加文件头注释
    code.append("// 本文件由 train_export.py 自动生成，请不要手动修改权重数组。")
    # 添加防重复包含宏开始
    code.append("#ifndef ESP32_BP_MODEL_H")
    # 定义防重复包含宏
    code.append("#define ESP32_BP_MODEL_H")
    # 引入 math.h，用于 sqrtf 和 expf
    code.append("#include <math.h>")
    # 引入 stdint.h，便于 ESP32 工程使用标准整数类型
    code.append("#include <stdint.h>")
    # 定义窗口长度
    code.append(f"#define WINDOW_LEN {window_len}")
    # 定义六轴通道数量
    code.append("#define AXIS_NUM 6")
    # 定义特征维度
    code.append(f"#define FEATURE_DIM {feature_dim}")
    # 定义类别数量
    code.append(f"#define CLASS_NUM {class_num}")
    # 定义第一隐藏层大小
    code.append(f"#define HIDDEN1 {HIDDEN1}")
    # 定义第二隐藏层大小
    code.append(f"#define HIDDEN2 {HIDDEN2}")
    # 定义第三隐藏层大小
    code.append(f"#define HIDDEN3 {HIDDEN3}\n")
    # 添加类别名称数组
    code.append(class_name_code)
    # 添加特征名称数组
    code.append(feature_name_code)
    # 添加特征均值数组
    code.append(c_array_1d("FEATURE_MEAN", mean))
    # 添加特征标准差数组
    code.append(c_array_1d("FEATURE_STD", std))
    # 添加第一层权重数组
    code.append(c_array_2d("W1", w1))
    # 添加第一层偏置数组
    code.append(c_array_1d("B1", b1))
    # 添加第二层权重数组
    code.append(c_array_2d("W2", w2))
    # 添加第二层偏置数组
    code.append(c_array_1d("B2", b2))
    # 添加第三层权重数组
    code.append(c_array_2d("W3", w3))
    # 添加第三层偏置数组
    code.append(c_array_1d("B3", b3))
    # 添加输出层权重数组
    code.append(c_array_2d("W4", w4))
    # 添加输出层偏置数组
    code.append(c_array_1d("B4", b4))
    # 添加 C 端特征提取和推理函数
    code.append(r'''
// 计算单个一维序列的 10 个特征，并追加到 feature 数组中
static inline void append_series_features(const float* x, int n, float* feature, int* idx) {
    float sum = 0.0f;
    float sum2 = 0.0f;
    float min_v = x[0];
    float max_v = x[0];
    for (int i = 0; i < n; i++) {
        float v = x[i];
        sum += v;
        sum2 += v * v;
        if (v < min_v) min_v = v;
        if (v > max_v) max_v = v;
    }
    float mean = sum / (float)n;
    float energy = sum2 / (float)n;
    float var = energy - mean * mean;
    if (var < 0.0f) var = 0.0f;
    float std = sqrtf(var);
    float range_v = max_v - min_v;
    float rms = sqrtf(energy);
    float mean_abs = 0.0f;
    float mean_abs_diff = 0.0f;
    float zcr_count = 0.0f;
    for (int i = 0; i < n; i++) {
        float v = x[i];
        mean_abs += fabsf(v);
        if (i > 0) {
            mean_abs_diff += fabsf(x[i] - x[i - 1]);
            float a = x[i - 1] - mean;
            float b = x[i] - mean;
            if (a * b < 0.0f) zcr_count += 1.0f;
        }
    }
    mean_abs = mean_abs / (float)n;
    if (n > 1) {
        mean_abs_diff = mean_abs_diff / (float)(n - 1);
        zcr_count = zcr_count / (float)(n - 1);
    }
    feature[(*idx)++] = mean;
    feature[(*idx)++] = std;
    feature[(*idx)++] = min_v;
    feature[(*idx)++] = max_v;
    feature[(*idx)++] = range_v;
    feature[(*idx)++] = rms;
    feature[(*idx)++] = mean_abs;
    feature[(*idx)++] = energy;
    feature[(*idx)++] = mean_abs_diff;
    feature[(*idx)++] = zcr_count;
}

// 从一个窗口中提取 80 维特征
// window 的顺序必须是：[采样点][gx, gy, gz, ax, ay, az]
// gx/gy/gz 单位必须是 °/s，ax/ay/az 单位必须是 g
static inline void extract_features_from_window(const float window[WINDOW_LEN][AXIS_NUM], float feature[FEATURE_DIM]) {
    int idx = 0;
    float temp[WINDOW_LEN];
    for (int axis = 0; axis < AXIS_NUM; axis++) {
        for (int i = 0; i < WINDOW_LEN; i++) {
            temp[i] = window[i][axis];
        }
        append_series_features(temp, WINDOW_LEN, feature, &idx);
    }
    for (int i = 0; i < WINDOW_LEN; i++) {
        float gx = window[i][0];
        float gy = window[i][1];
        float gz = window[i][2];
        temp[i] = sqrtf(gx * gx + gy * gy + gz * gz);
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);
    for (int i = 0; i < WINDOW_LEN; i++) {
        float ax = window[i][3];
        float ay = window[i][4];
        float az = window[i][5];
        temp[i] = sqrtf(ax * ax + ay * ay + az * az);
    }
    append_series_features(temp, WINDOW_LEN, feature, &idx);
}

// ReLU 激活函数
static inline float relu_float(float x) {
    return x > 0.0f ? x : 0.0f;
}

// 根据原始 80 维特征进行 BP 前向推理
static inline int bp_predict_from_features(const float feature_raw[FEATURE_DIM], float* confidence) {
    float x[FEATURE_DIM];
    float h1[HIDDEN1];
    float h2[HIDDEN2];
    float h3[HIDDEN3];
    float out[CLASS_NUM];
    for (int i = 0; i < FEATURE_DIM; i++) {
        x[i] = (feature_raw[i] - FEATURE_MEAN[i]) / FEATURE_STD[i];
    }
    for (int o = 0; o < HIDDEN1; o++) {
        float sum = B1[o];
        for (int i = 0; i < FEATURE_DIM; i++) sum += W1[o][i] * x[i];
        h1[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN2; o++) {
        float sum = B2[o];
        for (int i = 0; i < HIDDEN1; i++) sum += W2[o][i] * h1[i];
        h2[o] = relu_float(sum);
    }
    for (int o = 0; o < HIDDEN3; o++) {
        float sum = B3[o];
        for (int i = 0; i < HIDDEN2; i++) sum += W3[o][i] * h2[i];
        h3[o] = relu_float(sum);
    }
    float max_logit = -3.4028235e38f;
    for (int o = 0; o < CLASS_NUM; o++) {
        float sum = B4[o];
        for (int i = 0; i < HIDDEN3; i++) sum += W4[o][i] * h3[i];
        out[o] = sum;
        if (sum > max_logit) max_logit = sum;
    }
    float exp_sum = 0.0f;
    int best_idx = 0;
    float best_prob = 0.0f;
    for (int o = 0; o < CLASS_NUM; o++) {
        out[o] = expf(out[o] - max_logit);
        exp_sum += out[o];
    }
    for (int o = 0; o < CLASS_NUM; o++) {
        float prob = out[o] / exp_sum;
        if (prob > best_prob) {
            best_prob = prob;
            best_idx = o;
        }
    }
    if (confidence != 0) {
        *confidence = best_prob;
    }
    return best_idx;
}

// 根据一个完整 IMU 窗口直接预测动作类别
static inline int bp_predict_from_window(const float window[WINDOW_LEN][AXIS_NUM], float* confidence) {
    float feature[FEATURE_DIM];
    extract_features_from_window(window, feature);
    return bp_predict_from_features(feature, confidence);
}

#endif
''')
    # 将代码列表拼接成完整字符串
    final_code = "\n".join(code)
    # 写入头文件
    save_path.write_text(final_code, encoding="utf-8")


# =========================
# 13. 主函数
# =========================

# 定义主函数
def main():
    # 固定随机种子
    set_seed(SEED)
    # 创建输出文件夹
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    # 选择训练设备，有 GPU 就用 GPU，没有就用 CPU
    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    # 打印训练设备
    print(f"当前训练设备: {device}")
    # 扫描数据集
    records, class_names, label_to_idx = scan_dataset(DATASET_DIR)
    # 打印类别列表
    print("类别列表:", class_names)
    # 打印原始文件数量
    print("原始文件数量:", len(records))
    # 生成特征名
    feature_names = build_feature_names()
    # 打印特征维度
    print("特征维度:", len(feature_names))
    # 创建列表保存所有窗口长度实验的结果
    all_results = []
    # 遍历不同窗口长度
    for window_seconds in WINDOW_SECONDS_LIST:
        # 训练当前窗口长度对应的模型
        result = train_one_experiment(window_seconds, records, class_names, label_to_idx, device)
        # 保存当前实验结果
        all_results.append(result)
    # 按验证集准确率和验证集 F1 选择最佳实验，避免直接用测试集调参
    best_result = sorted(all_results, key=lambda r: (r["val_acc"], r["val_f1"]), reverse=True)[0]
    # 打印最佳窗口长度
    print("\n========== 最佳实验 ==========")
    # 打印最佳窗口长度和指标
    print(f"best_window={best_result['window_seconds']}s val_acc={best_result['val_acc']:.4f} test_acc={best_result['test_acc']:.4f}")
    # 获取测试集真实标签
    y_test = best_result["y_test"]
    # 获取测试集预测标签
    test_pred = best_result["test_pred"]
    # 生成分类报告字典
    cls_report = classification_report(y_test, test_pred, target_names=class_names, output_dict=True, zero_division=0)
    # 生成分类报告文本
    cls_report_text = classification_report(y_test, test_pred, target_names=class_names, zero_division=0)
    # 打印分类报告
    print("\n测试集分类报告:")
    # 打印分类报告文本
    print(cls_report_text)
    # 保存混淆矩阵图片
    save_confusion_matrix(y_test, test_pred, class_names, OUTPUT_DIR / "confusion_matrix.png")
    # 保存最佳模型权重
    torch.save(best_result["model"].state_dict(), OUTPUT_DIR / "best_model.pt")
    # 保存标准化参数和配置
    np.savez(
        OUTPUT_DIR / "scaler_and_config.npz",
        mean=best_result["mean"],
        std=best_result["std"],
        class_names=np.asarray(class_names),
        feature_names=np.asarray(feature_names),
        window_len=np.asarray([best_result["window_len"]]),
        step_len=np.asarray([best_result["step_len"]]),
        sample_rate=np.asarray([SAMPLE_RATE]),
    )
    # 构造训练报告字典
    report = {
        "seed": SEED,
        "sample_rate": SAMPLE_RATE,
        "target_test_acc": TARGET_TEST_ACC,
        "class_names": class_names,
        "feature_names": feature_names,
        "best_window_seconds": best_result["window_seconds"],
        "best_window_len": best_result["window_len"],
        "step_len": best_result["step_len"],
        "rest_threshold": best_result["rest_threshold"],
        "val_acc": best_result["val_acc"],
        "val_f1": best_result["val_f1"],
        "test_acc": best_result["test_acc"],
        "test_f1": best_result["test_f1"],
        "classification_report": cls_report,
        "all_experiments": [
            {
                "window_seconds": r["window_seconds"],
                "window_len": r["window_len"],
                "val_acc": r["val_acc"],
                "val_f1": r["val_f1"],
                "test_acc": r["test_acc"],
                "test_f1": r["test_f1"],
                "train_sample_count": r["train_sample_count"],
                "val_sample_count": r["val_sample_count"],
                "test_sample_count": r["test_sample_count"],
            }
            for r in all_results
        ],
    }
    # 将训练报告保存为 JSON 文件
    with open(OUTPUT_DIR / "training_report.json", "w", encoding="utf-8") as f:
        # 写入 JSON，ensure_ascii=False 保留中文
        json.dump(report, f, ensure_ascii=False, indent=2)
    # 判断是否达到 95% 测试准确率
    reached_target = best_result["test_acc"] >= TARGET_TEST_ACC and best_result["test_f1"] >= TARGET_TEST_ACC
    # 如果达到目标，或者用户允许低于目标也导出，则生成 ESP32 头文件
    if reached_target or EXPORT_WHEN_BELOW_TARGET:
        # 导出 ESP32 头文件
        export_esp32_header(best_result, class_names, feature_names, OUTPUT_DIR / "esp32_bp_model.h")
        # 打印导出成功信息
        print(f"ESP32 头文件已导出: {OUTPUT_DIR / 'esp32_bp_model.h'}")
    # 如果没有达到目标，则不导出部署头文件
    else:
        # 打印提醒信息
        print("未达到 test_acc ≥ 95% 且 macro_f1 ≥ 95% 的部署门槛，暂不导出 ESP32 头文件。")
        # 打印调优建议
        print("建议查看 outputs/confusion_matrix.png，针对混淆严重的动作增加数据或调整窗口长度。")
    # 打印输出目录
    print(f"全部结果已保存到: {OUTPUT_DIR.resolve()}")


# Python 脚本入口，只有直接运行本文件时才会执行 main()
if __name__ == "__main__":
    # 执行主函数
    main()
```

---

## 10. 文件 3：`esp32_bp_model.h`

这个文件由 `train_export.py` 自动生成，不建议手写。

ESP32 工程中只需要：

```cpp
#include "esp32_bp_model.h"
```

然后在采满一个窗口后调用：

```cpp
float confidence = 0.0f;
int action_id = bp_predict_from_window(window, &confidence);
const char* action_name = CLASS_NAMES[action_id];
```

其中 `window` 的格式必须是：

```cpp
float window[WINDOW_LEN][6];
```

每一行数据顺序必须是：

```cpp
gx, gy, gz, ax, ay, az
```

并且单位必须是：

```cpp
gyro: °/s
acc : g
```

如果 ESP32 读取的是原始整数，则需要先换算：

```cpp
window[i][0] = gx_raw / 16.4f;
window[i][1] = gy_raw / 16.4f;
window[i][2] = gz_raw / 16.4f;
window[i][3] = ax_raw / 4096.0f;
window[i][4] = ay_raw / 4096.0f;
window[i][5] = az_raw / 4096.0f;
```

如果 QMI8658 驱动已经输出物理单位，就不要重复换算。

---

## 11. ESP32 主程序调用示例

下面是 ESP32 端的伪代码，实际工程中需要结合你的 QMI8658 驱动、BLE 和屏幕显示代码。

```cpp
#include "esp32_bp_model.h"

float imu_window[WINDOW_LEN][6];
int write_index = 0;
int sample_count = 0;

void on_new_imu_sample(float gx, float gy, float gz, float ax, float ay, float az) {
    imu_window[write_index][0] = gx;
    imu_window[write_index][1] = gy;
    imu_window[write_index][2] = gz;
    imu_window[write_index][3] = ax;
    imu_window[write_index][4] = ay;
    imu_window[write_index][5] = az;

    write_index++;

    if (write_index >= WINDOW_LEN) {
        write_index = 0;
    }

    if (sample_count < WINDOW_LEN) {
        sample_count++;
    }
}

void run_inference_every_500ms() {
    if (sample_count < WINDOW_LEN) {
        return;
    }

    float ordered_window[WINDOW_LEN][6];

    for (int i = 0; i < WINDOW_LEN; i++) {
        int idx = (write_index + i) % WINDOW_LEN;
        for (int j = 0; j < 6; j++) {
            ordered_window[i][j] = imu_window[idx][j];
        }
    }

    float confidence = 0.0f;
    int action_id = bp_predict_from_window(ordered_window, &confidence);
    const char* action_name = CLASS_NAMES[action_id];

    // 后续可以把 action_name、confidence、count、freq 通过 BLE 发给 PC 上位机
}
```

---

## 12. 与 PC 上位机动画联动的数据输出建议

ESP32 端每 0.5 秒给 PC 上位机发送一次识别结果即可：

```json
{
  "action": "squat",
  "confidence": 0.96,
  "count": 12,
  "freq": 0.55,
  "phase": 0.48
}
```

字段含义：

| 字段 | 含义 |
|---|---|
| `action` | 当前识别动作类别 |
| `confidence` | BP 模型 softmax 置信度 |
| `count` | 当前动作计数 |
| `freq` | 当前动作频率，单位 次/秒 |
| `phase` | 当前动作周期进度，0～1 |

上位机动画播放速度可设置为：

```text
动画倍率 = 当前动作频率 / 标准动作频率
```

如果 ESP32 状态机能输出 `phase`，PC 端动画可以直接跳到对应进度，效果会比单纯调速更自然。

---

## 13. 如果准确率低于 95%，具体排查方法

### 13.1 先看混淆矩阵

训练后打开：

```text
outputs/confusion_matrix.png
```

重点观察哪些动作互相混淆。例如：

```text
squat ↔ good_morning
jumping_squat ↔ tuck_jump
walk ↔ trot
lunge ↔ jumping_lunge
```

如果混淆集中在少数动作，说明整体方案没有问题，应针对混淆动作优化数据或计数逻辑。

### 13.2 检查休息段过滤是否合理

如果非 `sit` 类中混入大量静止窗口，会导致动作类别之间边界变差。

可以在代码中临时打印每个文件被过滤的窗口数量。如果某类动作被过滤过多，说明阈值太高；如果休息段大量保留，说明阈值太低。

### 13.3 检查采样率

训练数据是 25Hz。如果后期 ESP32 端用 50Hz 或 100Hz 数据直接推理，准确率会明显变化。

解决方式：

```text
方案 A：ESP32 固定 25Hz 采样
方案 B：ESP32 高采样率采集后降采样到 25Hz
方案 C：重新采集 50Hz 或 100Hz 数据并重新训练
```

### 13.4 检查佩戴位置

如果训练数据来自某一种佩戴方式，例如手腕方向固定，那么实际使用时手表方向变化会造成分布偏移。

改进方法：

```text
增加不同佩戴松紧、左右手、表盘朝向的数据
或者增加姿态不敏感特征，例如 acc_mag 和 gyro_mag
```

本文已经加入了 `acc_mag` 和 `gyro_mag` 特征，能减轻方向变化影响，但不能完全解决所有佩戴姿态差异。

### 13.5 必要时增加 rest 类

当前方案把 `sit` 作为静止类，同时过滤其他动作文件中的休息段。如果实际使用时需要识别“无动作/休息”，建议把休息段单独整理成：

```text
rest/
```

这样最终类别会从 11 类变为 12 类。ESP32 端如果识别到 `rest`，就暂停计数，PC 上位机显示休息状态。

---

## 14. 推荐的最终答辩表述

本系统采用 PC 端离线训练、ESP32-S3 端实时推理的轻量化设计。训练阶段基于公开 IMU 数据集，将六轴惯性数据按固定窗口分割，并提取均值、标准差、极值、均方根、能量、过零率等 80 维时域特征；随后构建三隐藏层 BP 神经网络完成 11 类健身动作分类。为了保证模型结果真实可靠，系统采用原始文件级别的数据划分方式，避免同一采集文件中的窗口同时出现在训练集和测试集中，同时针对高强度动作文件中的休息段设计运动强度过滤策略。训练完成后，模型参数、标准化参数和特征提取逻辑自动导出为 C/C++ 头文件，直接用于 ESP32-S3 端部署。端侧仅需完成滑动窗口缓存、特征计算和 BP 前向传播，即可输出动作类别和置信度，再结合动作状态机完成计数，并通过 BLE 发送至 PC 上位机进行动画化实时显示。

---

## 15. 版本管理建议

为了项目可复现，建议把以下内容一起提交到你自己的项目仓库：

```text
train_export.py
requirements.txt
training_report.json
confusion_matrix.png
best_model.pt
scaler_and_config.npz
esp32_bp_model.h
```

每次重新训练后，不要只说“准确率提高了”，而要记录：

```text
随机种子
窗口长度
步长
训练集文件数
验证集文件数
测试集文件数
验证准确率
测试准确率
测试 macro-F1
混淆矩阵
```

这样后期写论文、答辩、参赛材料时，结果更可信。

---

## 16. 参考资料

- 数据集仓库：<https://github.com/G1ow9711/IMU_Datasrt>
- PyTorch 可复现性说明：<https://docs.pytorch.org/docs/stable/notes/randomness.html>
- scikit-learn train_test_split 文档：<https://scikit-learn.org/stable/modules/generated/sklearn.model_selection.train_test_split.html>
- ESP-DL 官方说明：<https://components.espressif.com/components/espressif/esp-dl>
- 微雪 ESP32-S3-Touch-AMOLED-2.06 产品说明：<https://www.waveshare.com/esp32-s3-touch-amoled-2.06.htm>
