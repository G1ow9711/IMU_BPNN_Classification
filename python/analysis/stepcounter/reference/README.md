# StepCounter C 适配入口

`stepcounter_dataset_harness.c` 是本项目编写的数据适配入口，只负责把七列 IMU
文本转换为外部 StepCounter 的 `AccInput` 调用。

外部 `alg_step_counter.h/.c`、`main.exe` 及其 Python 仿真不属于本仓库，也没有在
本项目中取得可再分发许可。公开仓库不会附带这些文件。只有在你已经通过合法途径
取得对应实现时，才可按其许可证自行编译参考对照；当前产品计数验证应优先运行
`python.analysis.stepcounter.peak_valley_pairs`。
