// 引入独立测试类。
using FitnessCoach.SessionTransfer.Tests;

// 运行全部会话同步测试；异常会让进程返回非零。
await SessionTransferTests.RunAllAsync();
// 输出唯一成功标志，CI 和父任务可据此验收。
Console.WriteLine("CSHARP_SESSION_TRANSFER_TESTS_OK");
