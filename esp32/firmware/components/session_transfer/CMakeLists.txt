# 注册纯 C 会话同步组件；组件只依赖摘要仓储和 BLE 回调类型，不直接读写 NimBLE 或硬件。
idf_component_register(
    # session_transfer.c 负责编解码、分页快照和待发送 TransferData 队列。
    SRCS "session_transfer.c"
    # 公开协议结构与服务适配接口，应用 BLE 任务可直接调用。
    INCLUDE_DIRS "include"
    # session_store 提供最近 200 条摘要，ble_service 提供稳定回调返回码。
    REQUIRES session_store ble_service
)
