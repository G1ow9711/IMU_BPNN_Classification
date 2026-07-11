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

当前默认流程使用 264 项手工特征和单个平铺 BP。`--enable-family-specialist` 为跳跃四类形状专家消融开关，当前结果较差，默认不启用。`--primary-artifact-dir` 可加载已有主 BP，只训练专家网络。

`--validation-only` 用于测试隔离的消融训练：保留逐 epoch 日志和验证指标，但不构建测试窗口、不输出测试指标、不触发 ESP32 头文件导出。验证结果保存在 `validation_report.json`。

`--extra-train-dir` 中的文件只追加到原文件级训练划分；`--external-holdout-dir` 仅在非验证模式、验证候选已经选定后加载。外部盲测结果单独写入 `external_holdout`，不参与 11 类 ESP32 发布门槛。

事件候选特征先由 `analyze_feature_separability.py` 在训练/验证数据上检查。当前 12 项和精选 3 项均未通过固定验证基线，生产路径保持 264 维；候选公式保留在分析器中，不进入 ESP32 推理。

所有命令均从仓库根目录执行，具体命令见根目录 `README.md`。
