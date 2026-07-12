# Python 训练端

本目录保存数据读取、窗口过滤、手工特征提取、BP 训练、评估、C 头文件导出和自动测试代码。

主要文件：

- `train_export.py`：完整训练与导出入口；
- `evaluate_fixed_ensemble.py`：固定双 M0 融合、动作段累计和最终测试/外部留出复现入口，不提供测试集调参开关；
- `test_train_export.py`：核心行为和导出合同测试；
- `prepare_finals_dataset.py`：按清单校验并准备决赛新增会话；
- `finals_jumping_squat_manifest.json`：新增会话哈希、行数和训练/盲测角色；
- `analyze_feature_separability.py`：训练/验证特征 Fisher 分数和弱类配对效应量分析；
- `test_prepare_finals_dataset.py`、`test_feature_separability.py`：数据边界与分析器测试；
- `requirements.txt`：Python 依赖版本。

当前提取器输出 297 项手工特征，通道顺序固定为 `gx、gy、gz、ax、ay、az`。Round29 在窗口提特征前修复单轴孤立尖峰并裁剪文件首尾静止段；新增 `wrist_acf_first_peak` 后形成当前合同。Round36 的依赖审计只让第二 M0 把标准化索引 `184:232` 的 48 项归一化阶段特征固定为零，原始数据和其余特征不删除。`--enable-family-specialist` 仅保留为历史消融开关，默认不启用。

`--validation-only` 用于测试隔离的消融训练：保留逐 epoch 日志和验证指标，但不构建测试窗口、不输出测试指标、不触发 ESP32 头文件导出。验证结果保存在 `validation_report.json`。

`--extra-train-dir` 中的文件只追加到原文件级训练划分；`--external-holdout-dir` 仅在非验证模式、验证候选已经选定后加载。外部盲测结果单独写入 `external_holdout`，不参与 11 类 ESP32 发布门槛。

候选特征先由 `analyze_feature_separability.py` 在训练/验证数据上检查窗口级、文件级效应方向和与现有特征的相关性。Round20 后选择的 8 项低重复值已同步到 Python 与生成 C，形成 288 维 Round21 验证候选；测试集与外部 `scy3` 仍保持隔离。

训练器还提供三个显式实验参数：

- `--ema-decay`：每个 epoch 后对单 BP 参数做 EMA，仅用于验证和检查点选择，默认 `0`；
- `--label-smoothing`：交叉熵标签平滑，默认 `0`；
- `--window-seconds 4.0`：显式启用 4 秒完整周期上下文，默认窗口列表仍为 `1.5/2.0/2.5` 秒。

Round24 新增三个联合弱类优化开关：

- `--multi-branch`：按 `(112,48,24,48,32,30)` 六组连续特征独立编码，再融合到 32 维主嵌入；
- `--pk-batches`：每批从全部存在类别各取 6 个窗口，并优先让同类窗口来自不同采集文件；
- `--auxiliary-heads`：训练是否跳跃、强腾空、左右交替、弓步/深蹲和跳跃深蹲/收腹跳五个二分类属性，部署主路径不使用这些头。

当前最接近固定验证门槛的组合是 4 秒窗口加 `0.05` 标签平滑：最佳 epoch 54 的验证准确率、宏平均 F1、最小类别召回分别为 `92.31%/91.81%/79.81%`。由于最小召回仍低于 `79.92%` 基线，该轮未读取测试集或 `scy3`，也未导出正式 C 头文件。

当前最终固定配置使用 Round29 未掩码 M0 和 Round37 掩码 M0，logits 权重为 `0.85/0.15`。活动段内累计当前及全部历史 logits，基础测试 `jumping_squat/squat/tuck_jump` 召回为 `89.12%/99.80%/100%`，总准确率 `99.29%`；外部 `scy3` 三个跳跃类均为 `100%`。固定复现命令见根目录 README。

生成 C 已提供 `bp_combine_ensemble_logits`、`BpBoutAccumulator`、`bp_bout_accumulator_reset` 和 `bp_bout_accumulator_update`。静止、动作切换、断连或用户切换时必须重置。当前 `export_esp32_header` 仍只自动导出单个平铺 `BPNet`，尚未自动打包两个六分支 M0 权重；不要把旧单模型头文件当作最终双模型。

Round21 的 288 维可视验证结果为准确率 `91.88%`、宏平均 F1 `91.56%`、最低类别召回 `81.47%`。仍未达标的类别为 `jumping_lunge`、`jumping_squat`、`tuck_jump` 和 `lunge`，因此后续先执行误分类窗口子集特征分析，不直接继续训练。

Round22 将误分类子集筛出的 7 项候选加入到 295 维输入，但可视验证仅得到准确率 `91.36%`、宏平均 F1 `91.06%`、最低类别召回 `79.34%`。该轮低于 Round21，已按验证隔离规则拒绝。下一次训练前必须先完成动作相位、窗口标签、分组交叉验证、BP 表征与损失采样的联合诊断。

Round23 的 294 维平铺 BP 为 `91.01%/90.84%/78.76%`。Round24 联合启用多分支、P×K、定向 margin 和辅助头后为 `90.95%/90.53%/77.80%`，并在 epoch 80 早停、恢复 epoch 35。Round24 提升了 `jumping_lunge`，但 `jumping_squat`、`squat` 和 `tuck_jump` 仍低于 85%；本轮未读取测试集或 `scy3`，也未导出正式 C 头文件。

Round25 新增 `--pk-prior-corrected-ce`，并组合 `--supcon-weight 0.01 --dropout 0.20 --label-smoothing 0.03`。该轮在 epoch 60 早停、恢复 epoch 15，验证准确率/宏 F1/最低召回为 `91.04%/90.99%/77.22%`。先验修正没有解决 `jumping_squat/squat/tuck_jump` 局部边界，后续不再继续全局损失权重搜索。

所有命令均从仓库根目录执行，具体命令见根目录 `README.md`。
