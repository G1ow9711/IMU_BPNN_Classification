# 训练数据集

`Dataset/` 保存本项目训练、验证、特征可视化使用的六轴 IMU 基础数据。仓库克隆完成后即可直接读取，不需要再从其它目录复制。

基础记录来自同一仓库所有者发布的 [`G1ow9711/IMU_Datasrt`](https://github.com/G1ow9711/IMU_Datasrt)。原始说明保留在 [`ReadMe.txt`](ReadMe.txt)，完整文件大小、行数和 SHA-256 见 [`manifest.json`](manifest.json)。

## 数据规模

- 11 个动作类别。
- 189 个动作记录文件，另有 1 个原始说明文件。
- 557,904 个采样点。
- 采样率固定为 25 Hz。
- 原始数据共 18,316,995 字节。
- 数据树 SHA-256：`0e05b0d3ff767445f57e89936e03bdc1f595caa13c0c93dc7abb1de002fb6591`。

| 目录 | 中文动作 | 文件数 | 采样点数 |
|---|---|---:|---:|
| `good_morning` | 早安式 | 17 | 37,728 |
| `jumping_jack` | 开合跳 | 18 | 52,968 |
| `jumping_lunge` | 跳跃弓步 | 15 | 57,516 |
| `jumping_squat` | 跳跃深蹲 | 16 | 56,640 |
| `lunge` | 弓步 | 18 | 43,956 |
| `sit` | 静坐 | 19 | 56,736 |
| `squat` | 深蹲 | 15 | 41,892 |
| `trot` | 小跑 | 20 | 53,196 |
| `tuck_jump` | 收腹跳 | 15 | 53,412 |
| `walk` | 行走 | 18 | 50,688 |
| `wave` | 挥手 | 18 | 53,172 |

## 单行数据格式

每个动作文件包含 `N×8` 个逗号分隔整数：

```text
gx, gy, gz, ax, ay, az, timestamp_low, timestamp_high
```

前三列是陀螺仪原始量，量程为 ±2000 °/s，比例为 `16.4 LSB/(°/s)`；中间三列是加速度计原始量，量程为 ±8 g，比例为 `4096 LSB/g`。物理量换算为：

$$
\omega_i = \frac{g_i}{16.4}\ \text{deg/s},
\qquad
a_i = \frac{r_i}{4096}\ g
$$

其中，$g_i$ 表示陀螺仪整数读数，$r_i$ 表示加速度计整数读数。最后两列是预留时间戳字段；当前 557,904 个采样点中这两列全部为 0，因此训练程序使用固定 25 Hz 采样周期，不能从这两列推算绝对时间。

## 直接用于训练

在仓库根目录执行：

```powershell
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r python\requirements.txt

.\.venv\Scripts\python.exe -u python\train_export.py `
  --dataset-dir Dataset
```

只复现教程中的动作特征图：

```powershell
.\.venv\Scripts\python.exe -m python.visualize_action_features `
  --dataset-dir Dataset `
  --output-dir docs\assets\algorithm
```

训练程序仍按采集文件划分训练、验证和测试角色。不能先把相邻重叠窗口随机打散后再划分，否则同一记录的近重复窗口会同时出现在训练集和测试集，造成数据泄漏。

## 数据边界

这些文件没有完整受试者 ID、可靠佩戴侧元数据或逐次动作完成时刻标签。因此：

- 文件级隔离可以避免同一记录泄漏，但不能证明严格跨人员泛化。
- 当前产品验收固定右手腕；不能仅凭此数据宣称左右手等价。
- 数据可用于动作分类、特征分析和类内差异观察，不能直接充当每次计数事件的时间真值。
- 部分高动态记录包含“动作—静止休息—继续动作”，休息段是原始采集的一部分，不应简单删除。

现场手表导出的 BLE/CSV 日志不在本目录，也没有随基础数据集公开。新增采集记录前，应取得参与者授权并移除姓名、设备地址等直接标识。

## 完整性校验

[`manifest.json`](manifest.json) 为每个源文件记录相对路径、类别、字节数、采样点数和 SHA-256。`tree_sha256` 的计算规则为：按 UTF-8 相对路径排序，依次写入“相对路径 + NUL + 32 字节文件 SHA-256”，最后计算整体 SHA-256；清单自身不参与。

Git 对 `Dataset/**/*.txt` 禁用换行转换，所以仓库中的 TXT 与训练源文件逐字节一致。

## 授权与来源

本数据由仓库所有者并入当前 Apache-2.0 教程工程。使用、修改或再分发时请保留仓库根 [`LICENSE`](../LICENSE)、本说明和原始来源链接。第三方新增数据仍须由贡献者确认采集授权，不能因本目录公开而推定任何外部数据自动获得许可。
