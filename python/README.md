# Python 训练端

本目录保存数据读取、窗口过滤、手工特征提取、BP 训练、评估、C 头文件导出和自动测试代码。

主要文件：

- `train_export.py`：完整训练与导出入口；
- `test_train_export.py`：核心行为和导出合同测试；
- `prepare_finals_dataset.py`：按清单校验并准备决赛新增会话；
- `finals_jumping_squat_manifest.json`：新增会话哈希、行数和训练/盲测角色；
- `analyze_feature_separability.py`：训练/验证特征 Fisher 分数和弱类配对效应量分析；
- `test_prepare_finals_dataset.py`、`test_feature_separability.py`：数据边界与分析器测试；
- `requirements.txt`：Python 依赖版本。

当前提取器输出 294 项手工特征：288 项既有统计/频谱/峰形值，加 6 项经过训练/验证无训练分离度检查的事件对齐与水平各向异性值。Round21 的 288 维平铺 BP 仍是固定验证集最佳结果。`--enable-family-specialist` 为跳跃四类形状专家消融开关，当前结果较差，默认不启用。`--primary-artifact-dir` 可加载已有平铺主 BP，只训练专家网络。

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

当前发布门槛为五个批准弱类召回率至少 `85%`，其余六类至少 `90%`。验证隔离候选未逐类达标时，不运行正式测试、不读取外部会话，也不生成 `esp32/include/esp32_bp_model.h`。

Round21 的 288 维可视验证结果为准确率 `91.88%`、宏平均 F1 `91.56%`、最低类别召回 `81.47%`。仍未达标的类别为 `jumping_lunge`、`jumping_squat`、`tuck_jump` 和 `lunge`，因此后续先执行误分类窗口子集特征分析，不直接继续训练。

Round22 将误分类子集筛出的 7 项候选加入到 295 维输入，但可视验证仅得到准确率 `91.36%`、宏平均 F1 `91.06%`、最低类别召回 `79.34%`。该轮低于 Round21，已按验证隔离规则拒绝。下一次训练前必须先完成动作相位、窗口标签、分组交叉验证、BP 表征与损失采样的联合诊断。

Round23 的 294 维平铺 BP 为 `91.01%/90.84%/78.76%`。Round24 联合启用多分支、P×K、定向 margin 和辅助头后为 `90.95%/90.53%/77.80%`，并在 epoch 80 早停、恢复 epoch 35。Round24 提升了 `jumping_lunge`，但 `jumping_squat`、`squat` 和 `tuck_jump` 仍低于 85%；本轮未读取测试集或 `scy3`，也未导出正式 C 头文件。

Round25 新增 `--pk-prior-corrected-ce`，并组合 `--supcon-weight 0.01 --dropout 0.20 --label-smoothing 0.03`。该轮在 epoch 60 早停、恢复 epoch 15，验证准确率/宏 F1/最低召回为 `91.04%/90.99%/77.22%`。先验修正没有解决 `jumping_squat/squat/tuck_jump` 局部边界，后续不再继续全局损失权重搜索。

所有命令均从仓库根目录执行，具体命令见根目录 `README.md`。
