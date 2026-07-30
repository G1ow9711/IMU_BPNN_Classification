# 参考论文

本目录保存与项目算法和端侧部署直接相关的开放获取论文。它们用于说明研究背景、设计取舍和验证边界，不表示本项目逐行复现了论文算法，也不表示论文实验指标可以直接代表本项目结果。

## 论文清单

### 1. 可穿戴 IMU 多特征与 BP 神经网络

- 题名：*Wearable Sensor-Based Human Activity Recognition Method with Multi-Features Extracted from Hilbert-Huang Transform*
- 作者：Huile Xu、Jinyi Liu、Haibo Hu、Yi Zhang
- 出版信息：Sensors 2016, 16, 2048
- DOI：[10.3390/s16122048](https://doi.org/10.3390/s16122048)
- 官方 PDF：[MDPI](https://mdpi-res.com/d_attachment/sensors/sensors-16-02048/article_deploy/sensors-16-02048.pdf)
- 仓库文件：[Li_2016_Wearable_IMU_BPNN_Multi_Features.pdf](Li_2016_Wearable_IMU_BPNN_Multi_Features.pdf)
- 项目关联：论文展示了从可穿戴惯性信号提取多类特征，再以 BP 神经网络分类的完整路线。本项目同样采用可解释特征和轻量全连接网络，但使用自己的 297 维特征、双 M0 融合、文件级划分和 ESP32 数值一致性合同。
- SHA-256：`04FD2B36310486FA231F05E1FCCF9641BE652C2EA891CBF9A730141B4D118CB3`

### 2. 智能手表健身动作识别与重复计数

- 题名：*Recognition and Repetition Counting for Complex Physical Exercises with Deep Learning*
- 作者：Andrea Soro、Gino Brunner、Simon Tanner、Roger Wattenhofer
- 出版信息：Sensors 2019, 19, 714
- DOI：[10.3390/s19030714](https://doi.org/10.3390/s19030714)
- 官方 PDF：[MDPI](https://mdpi-res.com/d_attachment/sensors/sensors-19-00714/article_deploy/sensors-19-00714.pdf)
- 仓库文件：[Soro_2019_Exercise_Recognition_and_Repetition_Counting.pdf](Soro_2019_Exercise_Recognition_and_Repetition_Counting.pdf)
- 项目关联：论文把动作类别识别和重复计数作为两个相连但不同的任务。该分层与本项目“主动作分类负责选择计数器，逐点相位状态机负责产生权威次数”的设计原则一致；具体网络和计数器实现不同。
- SHA-256：`F67C413231659DB5237642B7F3D3BD417DCF952C3AB4A32694086D03213066C5`

### 3. 佩戴位置变化下的动作识别与计数

- 题名：*ExerSense: Physical Exercise Recognition and Counting Algorithm from Wearables Robust to Positioning*
- 作者：Shun Ishii、Anna Yokokubo、Mika Luimula、Guillaume Lopez
- 出版信息：Sensors 2021, 21, 91
- DOI：[10.3390/s21010091](https://doi.org/10.3390/s21010091)
- 官方 PDF：[MDPI](https://mdpi-res.com/d_attachment/sensors/sensors-21-00091/article_deploy/sensors-21-00091.pdf)
- 仓库文件：[Ishii_2021_ExerSense.pdf](Ishii_2021_ExerSense.pdf)
- 项目关联：论文比较不同穿戴设备和佩戴位置，说明传感器位置会显著影响识别表现。本项目因此把当前可验证范围固定为右手腕，不把单一佩戴域的结果夸大为任意位置泛化。
- SHA-256：`6186A852E6D365E7D4EDBA0E46AE1D00A907B4B129E325A6329C68E8BBE2D89E`

### 4. 实时识别、背景活动与重复计数

- 题名：*Real-Time Sensor-Based Human Activity Recognition for eFitness and eHealth Platforms*
- 作者：Łukasz Czekaj、Mateusz Kowalewski、Jakub Domaszewicz、Robert Kitłowski、Mariusz Szwoch、Włodzisław Duch
- 出版信息：Sensors 2024, 24, 3891
- DOI：[10.3390/s24123891](https://doi.org/10.3390/s24123891)
- 官方 PDF：[MDPI](https://mdpi-res.com/d_attachment/sensors/sensors-24-03891/article_deploy/sensors-24-03891.pdf)
- 仓库文件：[Czekaj_2024_Real_Time_Sensor_Based_HAR.pdf](Czekaj_2024_Real_Time_Sensor_Based_HAR.pdf)
- 项目关联：论文把实时运动识别、重复计数和背景活动误报同时纳入评估。本项目据此强调休息/静止门、背景噪声冻结、窗口因果性和逐次计数事件，不能只报告动作窗口分类准确率。
- SHA-256：`18151A1369BE16DB48D927948071D529BB0B4635A236ADA85360D94A0FB42343`

### 5. 资源受限设备上的 TinyML 推理

- 题名：*TinyML: Enabling of Inference Deep Learning Models on Ultra-Low-Power IoT Edge Devices for AI Applications*
- 作者：Norah N. Alajlan、Dina M. Ibrahim
- 出版信息：Micromachines 2022, 13, 851
- DOI：[10.3390/mi13060851](https://doi.org/10.3390/mi13060851)
- 官方 PDF：[MDPI](https://mdpi-res.com/d_attachment/micromachines/micromachines-13-00851/article_deploy/micromachines-13-00851.pdf)
- 仓库文件：[Alajlan_2022_TinyML_Edge_Inference.pdf](Alajlan_2022_TinyML_Edge_Inference.pdf)
- 项目关联：论文综述了受限微控制器本地推理的延迟、功耗、内存和隐私价值。本项目用纯 C 双 M0 前向、冻结参数和 ESP32 主机数值对照落实端侧推理，但不声称复制论文中的具体模型。
- SHA-256：`0F41AC44ED864FA744C85CE3233C13983CCC202D35A840D8208659D2EEBFB91D`

## 许可证与引用

上述五篇论文均在 PDF 版权页声明采用 [Creative Commons Attribution 4.0 International](https://creativecommons.org/licenses/by/4.0/) 许可。再使用或再分发时必须保留作者、题名、出版信息、DOI、版权和许可证说明，并注明是否作过修改。

仓库根目录的 Apache License 2.0 只覆盖项目自有代码、文档和项目生成资产，不会改变这些论文的 CC BY 4.0 许可。论文中的商标、第三方图片或另行标注的材料仍按各自权利声明处理。
