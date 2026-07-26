# 离线诊断与可视验证

本目录提供两类可复现工具：

1. 把上位机 44 列中文日志中的六轴波形、分类窗口和固件权威计数事件对齐到同一设备时间轴。
2. 用显式指定的外部 StepCounter 参考实现，对七列 IMU 数据生成峰谷配对图、逐组 CSV 和机器可读清单。

工具只读取命令行给出的文件，不搜索作者桌面、工作树或运行时缓存。输出目录也必须
由调用者显式指定，建议放在仓库外或项目本地忽略目录中。

## 安装依赖

在仓库根目录执行：

```powershell
python -m pip install -r python/analysis/requirements.txt
```

## 44 列日志事件对齐

```powershell
python -m python.analysis.count_event_alignment `
  --input <44列中文日志.csv> `
  --session <会话序号> `
  --action <动作名称> `
  --truth <人工核数> `
  --output-dir <输出目录>
```

输出内容：

- `会话<序号>_<动作>_对齐.png`：加速度、角速度、分类窗和权威计数点共用设备毫秒时间轴。
- `会话<序号>_<动作>_对齐.json`：每个分类窗、每个计数事件及事件前 1.6 秒活动强度统计。

人工真值只进入标题和误差说明，不参与阈值选择。动作参数接受任意产品动作名称，
不会切换到某个动作专用的离线公式。

## 峰谷配对可视验证

```powershell
python -m python.analysis.stepcounter.peak_valley_pairs `
  --data-root <七列IMU文本目录> `
  --stepcounter-root <合法取得的StepCounter参考工程> `
  --output-root <输出目录>
```

当前配对合同：每轴按时间相邻关系，把一峰一谷组成一个完整动作；峰先出现或谷先
出现均可，每个极值最多使用一次。幅值、时间间隔过门后计一次，三轴次数取中位数，
不把峰数乘二。

外部 StepCounter 源码、二进制和数据没有随本仓库发布，本项目也未确认其可再分发
许可。公开教程只提供适配入口、来源哈希和结果合同；使用者必须自行确认外部实现及
数据的取得方式和许可证。

## 测试

始终可运行的合成合同：

```powershell
python -m unittest python.analysis.tests.test_peak_valley_pairs -v
```

可选真实数据回归需要显式设置外部路径；未设置时仅跳过这一条跨仓库用例：

```powershell
$env:IMU_STEPCOUNTER_ROOT = "<StepCounter参考工程>"
$env:IMU_STEPCOUNTER_DATA_ROOT = "<七列IMU数据目录>"
python -m unittest python.analysis.tests.test_peak_valley_pairs -v
```

测试覆盖一峰一谷只计一次、末尾半周期不计、谷后峰边界周期可计、极值不可复用，
以及外部三份数据的可选回归。
