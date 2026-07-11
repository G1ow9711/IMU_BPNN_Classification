# IMU 健身动作 BP 神经网络识别

本项目使用六轴 IMU 数据识别 11 类健身动作，并将训练完成的 BP 神经网络导出为 ESP32-S3 可直接使用的 C 头文件。

项目严格采用以下技术路线：

```text
六轴 IMU -> 手工特征 -> 标准化 -> BP 全连接网络 -> C 头文件 -> ESP32-S3
```

不使用 CNN、RNN、LSTM 或 Transformer 作为部署模型。

## 目录结构

```text
IMU_BPNN_Classification/
├─ python/                 Python 训练、评估、导出与测试代码
├─ esp32/                  ESP32-S3 端代码和达标后生成的模型头文件
│  ├─ include/
│  └─ src/
├─ pc/                     上位机端代码与通信协议说明
├─ docs/                   原始方案、优化设计和实施记录
├─ README.md               中文项目说明
└─ .gitignore
```

数据集、虚拟环境、训练输出和本机缓存不会提交到仓库。

## 数据集

数据集来源：[G1ow9711/IMU_Datasrt](https://github.com/G1ow9711/IMU_Datasrt)。

将数据放到：

```text
IMU_Dataset/imu_dataset_for_final/
```

数据包含陀螺仪 `gx, gy, gz` 和加速度计 `ax, ay, az`，采样率为 25 Hz。

### 决赛 `jumping_squat` 会话

仓库额外提供 `python/finals_jumping_squat_manifest.json` 和校验脚本，用于接入根目录 `决赛/MATLAB/实测数据集/A类活动` 中的三次独立录制。数据本身仍不提交仓库。

| 文件 | SHA-256 | 行数 | 用途 |
|---|---|---:|---|
| `jumping_squat_scy1_20.txt` | `FE10A5B4...BDE0F379` | 2975 | 仅追加到训练集 |
| `jumping_squat_scy2_20.txt` | `E9A02819...6C3A52C9` | 2977 | 仅追加到训练集 |
| `jumping_squat_scy3_20.txt` | `4B4C5420...9C189111` | 2969 | 延迟外部盲测 |

准备数据：

```powershell
.\.venv\Scripts\python.exe python\prepare_finals_dataset.py `
  --source-dir "..\决赛\MATLAB\实测数据集\A类活动" `
  --output-dir IMU_Dataset\finals_jumping_squat
```

脚本先校验哈希、行数和重复内容，再复制为 `train/jumping_squat` 与 `external_holdout/jumping_squat`。基础 189 个文件先完成文件级划分，`scy1/scy2` 随后只追加到训练集；`scy3` 只允许在验证候选通过后加载。

## Python 环境

在项目根目录创建虚拟环境并安装依赖：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r python\requirements.txt
```

运行全部测试：

```powershell
.\.venv\Scripts\python.exe -m unittest discover -s python -p "test_*.py"
```

运行完整训练：

```powershell
.\.venv\Scripts\python.exe -u python\train_export.py `
  --dataset-dir IMU_Dataset\imu_dataset_for_final
```

训练会逐 epoch 输出总损失、交叉熵、跨文件监督对比损失、弱类 margin、验证准确率、宏平均 F1、弱类 F1、最差类别 F1 和早停状态。

![逐 epoch 可见训练过程](docs/训练过程截图.png)

## 特征与 BP 网络

当前特征维度为 264：

- 112 个全局统计特征；
- 48 个原始四阶段时序特征；
- 24 个峰值、频率、谱熵和自相关特征；
- 48 个窗口内标准化四阶段形状特征；
- 32 个分位数、偏度、峰度和最大跳变特征。

方向鲁棒特征包括重力方向上的垂直分量和垂直于重力的水平分量。训练增强使用六轴同步有限角度旋转、非循环时间变形和轻微传感器噪声。

BP 网络结构：

```text
264 -> 96 -> 64 -> 32 -> 11
```

训练时使用原始文件均衡采样、跨文件监督对比损失和弱混淆类别 margin；这些训练辅助项不会进入 ESP32 推理代码。

代码还保留实验性的跳跃动作形状专家开关：

```powershell
--enable-family-specialist
```

该专家只使用 94 个幅值不敏感特征。当前数据上的消融结果低于平铺 BP，因此默认关闭，不作为正式模型。

模型选择阶段可使用验证隔离模式：

```powershell
.\.venv\Scripts\python.exe -u python\train_export.py `
  --dataset-dir IMU_Dataset\imu_dataset_for_final `
  --validation-only `
  --window-seconds 2.5
```

该模式不构建测试窗口、不计算测试指标，也不导出 C 头文件；结果写入 `validation_report.json`。候选方案只有先超过既有验证基线，才允许进行一次正式测试评估。

加入决赛训练会话的验证隔离命令：

```powershell
.\.venv\Scripts\python.exe -u python\train_export.py `
  --dataset-dir IMU_Dataset\imu_dataset_for_final `
  --extra-train-dir IMU_Dataset\finals_jumping_squat\train `
  --external-holdout-dir IMU_Dataset\finals_jumping_squat\external_holdout `
  --validation-only `
  --window-seconds 2.5
```

分析训练/验证特征分离度：

```powershell
.\.venv\Scripts\python.exe python\analyze_feature_separability.py `
  --dataset-dir IMU_Dataset\imu_dataset_for_final `
  --extra-train-dir IMU_Dataset\finals_jumping_squat\train `
  --validation-report outputs\round9_finals_event_validation_20260711\validation_report.json `
  --output-json outputs\feature_separability.json `
  --output-csv outputs\feature_separability_top.csv
```

分析器只读取训练/验证文件，输出 Fisher 分数、文件级 Fisher 分数、训练/验证同方向 Cohen's d，以及候选特征与现有特征的相关性。

优化依据及方法取舍见 [docs/论文依据与优化取舍.md](docs/论文依据与优化取舍.md)。

## 输出文件

训练结果默认写入本地 `outputs/`：

```text
best_model.pt
scaler_and_config.npz
training_report.json
confusion_matrix.png
training_console.log
```

部署门槛按动作逐类验收：

```text
每个动作的测试集召回率 >= 0.90
```

召回率定义为该动作测试样本中被正确识别的比例。只有 11 个动作全部达标，才生成正式 `outputs/esp32_bp_model.h`，并同步到 `esp32/include/esp32_bp_model.h`。未达标时保留训练报告，但不发布模型头文件；`--export-when-below-target` 只用于本地诊断，不会覆盖 ESP32 代码区。

## 当前验证状态

- Python 单元测试覆盖特征顺序、方向不变性、时间变形、活动过滤、文件均衡、训练损失、逐 epoch 日志和 C 头文件合同。
- 生成的 C 特征提取器已使用 MinGW C99 编译，并与 Python 的 264 项特征逐值对照；最大绝对误差约为 `4.58e-05`。
- 当前最佳平铺 BP 使用 2.5 秒窗口，测试准确率 `94.61%`，宏平均 F1 `93.49%`。
- 两项后续方案仅在验证集消融：320 维周期形状特征得到验证准确率 `90.33%`、宏平均 F1 `89.97%`、最小类别召回 `78.19%`；保重力动态强度增强得到 `90.68%`、`90.35%`、`78.76%`。二者均低于 264 维基线的 `91.63%`、`91.16%`、`79.92%`，未读取测试指标，也未进入正式模型。
- 将原模型与动态增强模型做验证集 logits 加权融合后，最优权重仍为原模型 `100%`，因此未采用双 BP 部署。
- 决赛数据扩展和事件特征也严格只做验证消融：264 维加 `scy1/scy2` 得到 `90.95%/90.68%/78.38%`；12 项事件候选得到 `92.31%/92.01%/78.76%`；按分离度精选 3 项后得到 `90.82%/90.41%/78.78%`。三组数字依次为验证准确率、宏平均 F1、最小类别召回，均未同时超过固定基线 `91.63%/91.16%/79.92%`，因此测试集和 `scy3` 都未读取。
- 分离度分析表明 `event_gyro_vertical_correlation` 对四组易混动作最稳定；自由落体比例及最长连续比例主要区分 `jumping_squat` 与 `jumping_jack`；事件垂直跳变与现有特征相关系数为 `1.0`，属于重复特征。生产提取器因此恢复为 264 维，12 项候选只保留在分析工具中。

当前最佳模型的未达标类别：

| 动作 | 测试召回率 |
|---|---:|
| `jumping_squat` | 72.64% |
| `squat` | 89.56% |
| `tuck_jump` | 84.43% |

其余 8 类均达到 90%。因果 logits 平滑可把 `squat` 和 `tuck_jump` 提升到 99.60% 和 100%，但 `jumping_squat` 仍只有 80.07%，且 41 个窗口约对应 20 秒延迟，因此未作为正式实时方案。

失败主要集中在 `IMU-2023-05-17-16_38_15_jumping_squat.txt`。该会话峰值约为 800°/s 和 6–7g，明显高于同类训练文件，波形强度接近 `jumping_jack`。继续在当前测试文件上调参会导致测试集过拟合；下一轮应补采高强度 `jumping_squat` 的跨人员、跨会话和不同佩戴方向数据。

当前逐类 90% 门槛未满足，所以仓库中不会出现正式 `esp32/include/esp32_bp_model.h`。本地最佳结果以 `outputs/round4_264_features_20260711/` 为准。

### 后续弱类验证（Round 11-16）

在保持“264 项手工特征 + 单 BP + 可生成 C 头文件”不变的前提下，继续完成了以下 PyCharm 可见训练。每轮均逐 epoch 输出，且仅使用训练集和验证集：

| 方案 | 最佳 epoch | 验证准确率 | 宏平均 F1 | 最小类别召回 |
|---|---:|---:|---:|---:|
| 参数 EMA `0.90` | 65 | 91.58% | 91.22% | 77.41% |
| 扩展对称 hard-pair margin | 44 | 90.27% | 89.65% | 77.61% |
| 标签平滑 `0.05`、2.5 秒 | 35 | 91.27% | 90.91% | 79.15% |
| 12 项冲击对齐形态特征 | 11 | 90.71% | 90.23% | 77.80% |
| 4.0 秒上下文 | 38 | 90.48% | 89.76% | 79.23% |
| 4.0 秒 + 标签平滑 `0.05` | 54 | 92.31% | 91.81% | 79.81% |

最后一轮整体指标超过固定基线，但最小类别召回仍比 `79.92%` 基线低约 0.11 个百分点，因此没有进入测试评估。其验证召回仍低于 90% 的动作包括 `jumping_jack` 82%、`jumping_squat` 87%、`squat` 86% 和 `tuck_jump` 80%；`jumping_lunge` 达到 90%。测试集与 `scy3` 均未读取，ESP32 正式头文件未生成。

训练器现在支持显式 `--window-seconds 4.0`，默认窗口列表仍保持 `1.5/2.0/2.5` 秒；`--ema-decay` 和 `--label-smoothing` 默认均为 `0`。这些开关用于可复现实验，不改变默认生产路径。

## 说明

原始数据没有采集者 ID，因此当前结果能够证明原始文件之间无泄漏，但不能声称严格的跨人员泛化。正式部署前应使用目标手表采集独立用户、独立会话和不同佩戴方向的数据作为最终盲测集。
