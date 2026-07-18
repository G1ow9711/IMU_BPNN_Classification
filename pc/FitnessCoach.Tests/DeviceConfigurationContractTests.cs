// 引入显式小端读取工具，边界测试必须核对线上 Unix 秒而不是接口毫秒。
using System.Buffers.Binary;
// 引入协议编解码类型，测试只验证线上字节合同，不依赖真实蓝牙硬件。
using FitnessCoach.Bluetooth;

// 配置协议测试位于现有控制台测试程序集，避免引入额外测试框架依赖。
namespace FitnessCoach.Tests;

/// <summary>验证设备配置 TLV 和开发者六轴同步诊断样本的固定 v1 合同。</summary>
internal static class DeviceConfigurationContractTests
{
    /// <summary>顺序执行黄金字节、边界和畸形输入测试。</summary>
    public static void RunAll()
    {
        // 先验证五条配置命令的精确小端黄金字节。
        TestGoldenConfigurationBytes();
        // 再验证所有物理范围和枚举边界，非法值不得进入 BLE 帧。
        TestConfigurationBoundaries();
        // 最后验证 RawStream 22 字节布局及短帧拒绝。
        TestRawStreamCodec();
    }

    // 验证每个 TLV 都使用 type:u8、length:u8、value:length，且多字节整数为小端。
    private static void TestGoldenConfigurationBytes()
    {
        // 接口输入 2023-11-14T22:13:20Z 的 Unix 毫秒 1700000000000，线上必须转换为 Unix 秒 1700000000。
        byte[] time = DeviceConfigurationCodec.EncodeTimeSync(1_700_000_000_000L, 480);
        // 核对 Unix 秒 int64 和时区 int16 的精确小端字节，禁止把接口毫秒原样发送。
        Assert(time.SequenceEqual(new byte[] { 0x01, 0x08, 0x00, 0xF1, 0x53, 0x65, 0x00, 0x00, 0x00, 0x00, 0x02, 0x02, 0xE0, 0x01 }), "Cmd6 时间同步 TLV 黄金字节错误。");
        // 65 kg 使用 65000 g，资料 revision 使用 0x01020304。
        byte[] profile = DeviceConfigurationCodec.EncodeProfile(65_000U, 0x01020304U);
        // 核对 weight_g 和 revision 两个 u32LE 项。
        Assert(profile.SequenceEqual(new byte[] { 0x01, 0x04, 0xE8, 0xFD, 0x00, 0x00, 0x02, 0x04, 0x04, 0x03, 0x02, 0x01 }), "Cmd7 用户资料 TLV 黄金字节错误。");
        // 卡路里目标使用 kind=3 和 300000 milli-kcal。
        byte[] goal = DeviceConfigurationCodec.EncodeGoal(DeviceGoalKind.CaloriesMilliKcal, 300_000U);
        // 核对一字节 kind 和四字节目标值。
        Assert(goal.SequenceEqual(new byte[] { 0x01, 0x01, 0x03, 0x02, 0x04, 0xE0, 0x93, 0x04, 0x00 }), "Cmd8 训练目标 TLV 黄金字节错误。");
        // 设备偏好覆盖亮度、振动、声音、熄屏、revision 和开发者模式。
        byte[] preferences = DeviceConfigurationCodec.EncodePreferences(new DevicePreferencesV1(35, true, false, 30, 0x11223344U, true));
        // 核对 bool 只能使用 0/1，熄屏为 u16LE，revision 为 u32LE。
        Assert(preferences.SequenceEqual(new byte[] { 0x01, 0x01, 0x23, 0x02, 0x01, 0x01, 0x03, 0x01, 0x00, 0x04, 0x02, 0x1E, 0x00, 0x05, 0x04, 0x44, 0x33, 0x22, 0x11, 0x06, 0x01, 0x01 }), "Cmd9 设备偏好 TLV 黄金字节错误。");
        // 原始流开关只包含一个 bool 项。
        Assert(DeviceConfigurationCodec.EncodeRawStreamEnabled(true).SequenceEqual(new byte[] { 0x01, 0x01, 0x01 }), "Cmd11 原始流开 TLV 黄金字节错误。");
        // 关闭值必须明确编码为零。
        Assert(DeviceConfigurationCodec.EncodeRawStreamEnabled(false).SequenceEqual(new byte[] { 0x01, 0x01, 0x00 }), "Cmd11 原始流关 TLV 黄金字节错误。");
    }

    // 验证冻结的工程范围，避免 UI 范围检查被绕过后生成设备无法解释的数据。
    private static void TestConfigurationBoundaries()
    {
        // 体重下界 30 kg 和上界 250 kg 均合法。
        _ = DeviceConfigurationCodec.EncodeProfile(30_000U, 1U);
        // 核对体重上界。
        _ = DeviceConfigurationCodec.EncodeProfile(250_000U, uint.MaxValue);
        // 小于 30 kg 必须拒绝。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodeProfile(29_999U, 1U), "低于 30kg 的资料未拒绝。");
        // 大于 250 kg 必须拒绝。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodeProfile(250_001U, 1U), "高于 250kg 的资料未拒绝。");
        // 亮度必须在 5～100%。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodePreferences(new DevicePreferencesV1(4, true, false, 30, 1U, false)), "过低亮度未拒绝。");
        // 熄屏时间必须在 10～300 秒。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodePreferences(new DevicePreferencesV1(35, true, false, 301, 1U, false)), "过长熄屏时间未拒绝。");
        // 未知目标类型不得发给固件。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodeGoal((DeviceGoalKind)99, 1U), "未知目标类型未拒绝。");
        // 2000-01-01T00:00:00Z 是固件 RTC 支持的最小时间。
        byte[] minimumTime = DeviceConfigurationCodec.EncodeTimeSync(946_684_800_000L, 0);
        // 核对最小输入编码为 Unix 秒 946684800。
        Assert(BinaryPrimitives.ReadInt64LittleEndian(minimumTime.AsSpan(2, 8)) == 946_684_800L, "Cmd6 最小年份未编码为 Unix 秒。" );
        // 同一秒内的 999 毫秒必须向下截断，不允许四舍五入到下一秒。
        byte[] truncatedTime = DeviceConfigurationCodec.EncodeTimeSync(946_684_800_999L, 0);
        // 核对毫秒尾数被明确截断。
        Assert(BinaryPrimitives.ReadInt64LittleEndian(truncatedTime.AsSpan(2, 8)) == 946_684_800L, "Cmd6 毫秒没有按合同截断。" );
        // 2099-12-31T23:59:59.999Z 是固件 RTC 支持的最大输入。
        byte[] maximumTime = DeviceConfigurationCodec.EncodeTimeSync(4_102_444_799_999L, 0);
        // 核对最大输入仍编码为 2099 年最后一秒。
        Assert(BinaryPrimitives.ReadInt64LittleEndian(maximumTime.AsSpan(2, 8)) == 4_102_444_799L, "Cmd6 最大年份未编码为 Unix 秒。" );
        // 2000 年前一毫秒必须拒绝。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodeTimeSync(946_684_799_999L, 0), "2000 年以前 UTC 未拒绝。" );
        // 2100-01-01T00:00:00Z 必须拒绝。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodeTimeSync(4_102_444_800_000L, 0), "2100 年及以后 UTC 未拒绝。" );
        // 时区范围限制到 UTC-14:00～UTC+14:00。
        AssertThrows<ArgumentOutOfRangeException>(() => DeviceConfigurationCodec.EncodeTimeSync(1_700_000_000_000L, 841), "非法时区未拒绝。");
    }

    // 验证 RawStream 固定记录与协议文档中的字段偏移完全一致。
    private static void TestRawStreamCodec()
    {
        // 构造 sample_index、单调时间、六轴固定点诊断码和质量位的 22 字节黄金 payload；测试只锁定布局，不把码值冒充 FIFO 原始值。
        byte[] payload = [0x04, 0x03, 0x02, 0x01, 0x44, 0x33, 0x22, 0x11, 0x01, 0x00, 0xFE, 0xFF, 0x03, 0x00, 0xFC, 0xFF, 0x05, 0x00, 0xFA, 0xFF, 0xAA, 0x55];
        // 解码固定长度 payload。
        bool decoded = RawStreamV1Codec.TryDecode(payload, out RawImuSampleV1? sample, out string? error);
        // 核对所有字段和通道顺序 gx、gy、gz、ax、ay、az。
        Assert(decoded && error is null && sample is not null && sample.SampleIndex == 0x01020304U && sample.MonotonicMilliseconds == 0x11223344U && sample.GxRaw == 1 && sample.GyRaw == -2 && sample.GzRaw == 3 && sample.AxRaw == -4 && sample.AyRaw == 5 && sample.AzRaw == -6 && sample.QualityFlags == 0x55AA, "RawStream 22字节解码错误。");
        // 缺一字节的记录必须拒绝，不能越界读取质量位。
        Assert(!RawStreamV1Codec.TryDecode(payload.AsSpan(0, 21), out _, out _), "RawStream 短帧未拒绝。");
        // 多一字节同样拒绝，防止未来版本被静默错读为 v1。
        Assert(!RawStreamV1Codec.TryDecode([.. payload, 0x00], out _, out _), "RawStream 长帧未拒绝。");
    }

    // 断言指定操作抛出预期异常类型。
    private static void AssertThrows<TException>(Action action, string message)
        where TException : Exception
    {
        try
        {
            // 执行待测非法操作。
            action();
        }
        catch (TException)
        {
            // 收到预期异常即通过。
            return;
        }

        // 没有抛出预期异常表示边界失效。
        throw new InvalidOperationException(message);
    }

    // 统一布尔断言，失败时抛出可定位消息。
    private static void Assert(bool condition, string message)
    {
        // 条件成立时测试通过。
        if (condition)
        {
            // 返回调用方继续执行。
            return;
        }

        // 条件失败时终止测试进程。
        throw new InvalidOperationException(message);
    }
}
