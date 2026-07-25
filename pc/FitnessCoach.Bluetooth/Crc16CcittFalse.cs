// CRC 公式、参数和复杂度见 docs/BLE通信协议.md 第 5 节。
// CRC 实现与逻辑帧编解码分离，便于用标准字符串独立验证参数。
namespace FitnessCoach.Bluetooth;

/// <summary>
/// 实现 CRC-16/CCITT-FALSE：poly=0x1021、init=0xFFFF、非反射、xorout=0。
/// </summary>
public static class Crc16CcittFalse
{
    /// <summary>
    /// 对连续字节计算 16 位校验；时间复杂度 O(n)，额外空间 O(1)。
    /// </summary>
    public static ushort Compute(ReadOnlySpan<byte> data)
    {
        // 按 CCITT-FALSE 定义把 16 位寄存器初值设为 0xFFFF。
        ushort crc = 0xFFFF;

        // 逐字节遍历输入，保持与 C 参考实现完全相同的顺序。
        foreach (byte value in data)
        {
            // 当前字节异或到 CRC 高 8 位，符合非反射算法。
            crc ^= (ushort)(value << 8);

            // 每个输入字节依次执行八轮多项式除法。
            for (int bit = 0; bit < 8; bit++)
            {
                // 保存左移前最高位，决定本轮是否异或 0x1021。
                bool highBitSet = (crc & 0x8000) != 0;
                // 在 16 位无符号范围内左移，显式丢弃溢出位。
                crc = (ushort)(crc << 1);

                // 最高位为一时应用 CCITT 生成多项式。
                if (highBitSet)
                {
                    // 异或 0x1021，结果与 ESP32 C 实现逐位一致。
                    crc ^= 0x1021;
                }
            }
        }

        // 返回未交换、未最终异或的 CRC 数值；线上再按小端写入。
        return crc;
    }
}
