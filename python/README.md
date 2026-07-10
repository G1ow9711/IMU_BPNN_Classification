# Python 训练端

本目录保存数据读取、窗口过滤、手工特征提取、BP 训练、评估、C 头文件导出和自动测试代码。

主要文件：

- `train_export.py`：完整训练与导出入口；
- `test_train_export.py`：核心行为和导出合同测试；
- `requirements.txt`：Python 依赖版本。

当前默认流程使用 264 项手工特征和单个平铺 BP。`--enable-family-specialist` 为跳跃四类形状专家消融开关，当前结果较差，默认不启用。`--primary-artifact-dir` 可加载已有主 BP，只训练专家网络。

`--validation-only` 用于测试隔离的消融训练：保留逐 epoch 日志和验证指标，但不构建测试窗口、不输出测试指标、不触发 ESP32 头文件导出。验证结果保存在 `validation_report.json`。

所有命令均从仓库根目录执行，具体命令见根目录 `README.md`。
