/* 引入文件后端公共合同和统一错误码。 */
#include "session_store.h"

/* 引入 errno，区分文件不存在和其它打开失败。 */
#include <errno.h>
/* 引入 LONG_MAX，确保 size_t 文件偏移可安全传给 fseek。 */
#include <limits.h>
/* 引入 FILE、fopen、fseek、fread、fwrite 和 fflush。 */
#include <stdio.h>
/* 引入 string.h，执行路径长度检查、复制和 0xFF 块填充。 */
#include <string.h>

/* Windows 主机用 _commit/_fileno 把 stdio 缓冲刷新到文件系统。 */
#ifdef _WIN32
/* 引入 _commit。 */
#include <io.h>
#else
/* ESP-IDF/newlib 和 POSIX 主机用 fsync/fileno。 */
#include <unistd.h>
#endif

/* 单次擦除/扩容使用 256 字节块，限制任务栈并减少 fwrite 调用。 */
#define SESSION_FILE_FILL_BLOCK_SIZE (256U)

/* 把公开 void* 句柄转换为 stdio FILE*；调用前必须检查 initialized。 */
static FILE *session_file_handle(session_file_backend_t *file_backend)
{
    /* file_handle 只保存 fopen 返回对象。 */
    return (FILE *)file_backend->file_handle;
}

/* 检查 offset+length 是否位于固定容量内，避免 size_t 加法溢出。 */
static bool session_file_range_is_valid(
    const session_file_backend_t *file_backend,
    const size_t offset,
    const size_t length)
{
    /* 后端必须已初始化且句柄存在。 */
    if ((file_backend == NULL) || !file_backend->initialized ||
        (file_backend->file_handle == NULL)) {
        /* 无有效文件。 */
        return false;
    }
    /* offset 不能超过容量。 */
    if (offset > file_backend->capacity) {
        /* 越界。 */
        return false;
    }
    /* length 必须不大于剩余容量，写法同时避免 offset+length 溢出。 */
    return length <= (file_backend->capacity - offset);
}

/* 从文件固定偏移读取连续字节。 */
static session_backend_result_t session_file_read(
    void *context,
    const size_t offset,
    uint8_t *output,
    const size_t length)
{
    /* 上下文转换为文件后端。 */
    session_file_backend_t *file_backend = (session_file_backend_t *)context;
    /* 非零读取要求输出指针有效。 */
    if ((length > 0U) && (output == NULL)) {
        /* 返回 I/O 错误，函数表没有参数错误枚举。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 检查固定容量范围。 */
    if (!session_file_range_is_valid(file_backend, offset, length)) {
        /* 区分范围错误。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 零长度读取直接成功。 */
    if (length == 0U) {
        /* 不调用 stdio。 */
        return SESSION_BACKEND_OK;
    }
    /* 取得有效 FILE 句柄。 */
    FILE *file = session_file_handle(file_backend);
    /* size_t 偏移先限制到 long 可表示范围；当前最大文件远小于 2 GiB。 */
    if (offset > (size_t)LONG_MAX) {
        /* fseek 无法表示。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 移动到绝对偏移。 */
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        /* 文件定位失败。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 必须完整读到 length 字节；短读视为介质错误。 */
    if (fread(output, 1U, length, file) != length) {
        /* 清除 EOF/错误位，允许后续恢复尝试。 */
        clearerr(file);
        /* 返回 I/O 错误。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 读取成功。 */
    return SESSION_BACKEND_OK;
}

/* 向文件固定偏移写入连续字节。 */
static session_backend_result_t session_file_write(
    void *context,
    const size_t offset,
    const uint8_t *input,
    const size_t length)
{
    /* 上下文转换为文件后端。 */
    session_file_backend_t *file_backend = (session_file_backend_t *)context;
    /* 非零写入要求输入指针有效。 */
    if ((length > 0U) && (input == NULL)) {
        /* 返回 I/O 错误。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 检查范围。 */
    if (!session_file_range_is_valid(file_backend, offset, length)) {
        /* 返回越界。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 零长度写入直接成功。 */
    if (length == 0U) {
        /* 不触碰文件。 */
        return SESSION_BACKEND_OK;
    }
    /* fseek 使用 long，显式拒绝不可表示偏移。 */
    if (offset > (size_t)LONG_MAX) {
        /* 返回越界。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 取得文件句柄。 */
    FILE *file = session_file_handle(file_backend);
    /* 移到目标偏移。 */
    if (fseek(file, (long)offset, SEEK_SET) != 0) {
        /* 定位失败。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 要求完整写入。 */
    if (fwrite(input, 1U, length, file) != length) {
        /* 返回 I/O 错误。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 写入成功；持久化时机由 sync 控制。 */
    return SESSION_BACKEND_OK;
}

/* 把指定范围覆盖为 0xFF，模拟擦除后的空白介质。 */
static session_backend_result_t session_file_erase(
    void *context,
    const size_t offset,
    const size_t length)
{
    /* 上下文转换为文件后端。 */
    session_file_backend_t *file_backend = (session_file_backend_t *)context;
    /* 检查范围。 */
    if (!session_file_range_is_valid(file_backend, offset, length)) {
        /* 返回越界。 */
        return SESSION_BACKEND_OUT_OF_RANGE;
    }
    /* 零长度擦除直接成功。 */
    if (length == 0U) {
        /* 不写文件。 */
        return SESSION_BACKEND_OK;
    }
    /* 构造固定 256 字节 0xFF 块。 */
    uint8_t blank[SESSION_FILE_FILL_BLOCK_SIZE];
    /* 全部字节填 0xFF。 */
    (void)memset(blank, 0xFF, sizeof(blank));
    /* 保存已擦除字节数。 */
    size_t written = 0U;
    /* 分块覆盖完整目标范围。 */
    while (written < length) {
        /* 当前块取剩余长度与 256 的较小值。 */
        const size_t remaining = length - written;
        /* 计算本块长度。 */
        const size_t chunk = remaining < sizeof(blank) ? remaining : sizeof(blank);
        /* 复用 write 的范围和完整写检查。 */
        const session_backend_result_t result = session_file_write(
            context,
            offset + written,
            blank,
            chunk);
        /* 任一块失败立即停止。 */
        if (result != SESSION_BACKEND_OK) {
            /* 返回原错误。 */
            return result;
        }
        /* 累加已完成长度。 */
        written += chunk;
    }
    /* 全范围已覆盖。 */
    return SESSION_BACKEND_OK;
}

/* 把 stdio 和操作系统文件缓存同步到介质。 */
static session_backend_result_t session_file_sync(void *context)
{
    /* 转换并验证上下文。 */
    session_file_backend_t *file_backend = (session_file_backend_t *)context;
    /* 未初始化不能同步。 */
    if (!session_file_range_is_valid(file_backend, 0U, 0U)) {
        /* 返回 I/O 错误。 */
        return SESSION_BACKEND_IO_ERROR;
    }
    /* 取得文件句柄。 */
    FILE *file = session_file_handle(file_backend);
    /* 先把 C 库缓冲写入操作系统。 */
    if (fflush(file) != 0) {
        /* 刷新失败。 */
        return SESSION_BACKEND_IO_ERROR;
    }
#ifdef _WIN32
    /* Windows 用 _commit 强制文件描述符落盘。 */
    if (_commit(_fileno(file)) != 0) {
        /* 落盘失败。 */
        return SESSION_BACKEND_IO_ERROR;
    }
#else
    /* ESP-IDF/newlib 和 POSIX 主机用 fsync。 */
    if (fsync(fileno(file)) != 0) {
        /* 落盘失败。 */
        return SESSION_BACKEND_IO_ERROR;
    }
#endif
    /* 同步成功。 */
    return SESSION_BACKEND_OK;
}

/* 把文件从当前长度扩展到固定容量，新增区域填 0xFF。 */
static session_store_status_t session_file_ensure_capacity(
    session_file_backend_t *file_backend,
    const size_t current_size)
{
    /* 当前文件大于合同容量时拒绝，避免静默截断用户数据。 */
    if (current_size > file_backend->capacity) {
        /* 返回空间/格式不匹配。 */
        return SESSION_STORE_STATUS_NO_SPACE;
    }
    /* 已达到容量无需写。 */
    if (current_size == file_backend->capacity) {
        /* 返回成功。 */
        return SESSION_STORE_STATUS_OK;
    }
    /* 临时标记初始化，使范围检查允许从 current_size 扩容。 */
    file_backend->initialized = true;
    /* 用 0xFF 填充剩余区域。 */
    const session_backend_result_t erase_result = session_file_erase(
        file_backend,
        current_size,
        file_backend->capacity - current_size);
    /* 擦写失败时撤销初始化。 */
    if (erase_result != SESSION_BACKEND_OK) {
        /* 标记不可用。 */
        file_backend->initialized = false;
        /* 返回 I/O 错误。 */
        return SESSION_STORE_STATUS_IO_ERROR;
    }
    /* 同步扩容。 */
    if (session_file_sync(file_backend) != SESSION_BACKEND_OK) {
        /* 标记不可用。 */
        file_backend->initialized = false;
        /* 返回 I/O 错误。 */
        return SESSION_STORE_STATUS_IO_ERROR;
    }
    /* 保持 initialized=true。 */
    return SESSION_STORE_STATUS_OK;
}

session_store_status_t session_file_backend_open(
    session_file_backend_t *file_backend,
    const char *path,
    const size_t capacity,
    const bool create_if_missing)
{
    /* 对象、路径和正容量必须有效。 */
    if ((file_backend == NULL) || (path == NULL) || (path[0] == '\0') ||
        (capacity == 0U)) {
        /* 返回参数错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* 路径必须放入固定数组并保留终止符。 */
    const size_t path_length = strlen(path);
    /* 拒绝过长路径，避免截断到其它文件。 */
    if (path_length >= sizeof(file_backend->path)) {
        /* 返回参数错误。 */
        return SESSION_STORE_STATUS_INVALID_ARGUMENT;
    }
    /* 清空旧上下文；调用方必须先 close 已打开对象。 */
    (void)memset(file_backend, 0, sizeof(*file_backend));
    /* 复制完整 UTF-8 路径含终止符。 */
    (void)memcpy(file_backend->path, path, path_length + 1U);
    /* 保存固定容量。 */
    file_backend->capacity = capacity;
    /* 先尝试读写打开现有文件。 */
    FILE *file = fopen(path, "r+b");
    /* 文件不存在且允许创建时新建。 */
    if ((file == NULL) && create_if_missing && (errno == ENOENT)) {
        /* w+b 创建空文件；路径父目录必须已由挂载层建立。 */
        file = fopen(path, "w+b");
    }
    /* 打开失败。 */
    if (file == NULL) {
        /* 清空路径和容量，保持关闭对象。 */
        (void)memset(file_backend, 0, sizeof(*file_backend));
        /* 返回 I/O 错误。 */
        return SESSION_STORE_STATUS_IO_ERROR;
    }
    /* 保存句柄。 */
    file_backend->file_handle = file;
    /* 移到文件末尾测量当前长度。 */
    if (fseek(file, 0L, SEEK_END) != 0) {
        /* 关闭失败对象。 */
        (void)fclose(file);
        /* 清空上下文。 */
        (void)memset(file_backend, 0, sizeof(*file_backend));
        /* 返回 I/O 错误。 */
        return SESSION_STORE_STATUS_IO_ERROR;
    }
    /* 读取当前末尾偏移。 */
    const long size_long = ftell(file);
    /* ftell 负值表示错误。 */
    if (size_long < 0L) {
        /* 关闭文件。 */
        (void)fclose(file);
        /* 清空上下文。 */
        (void)memset(file_backend, 0, sizeof(*file_backend));
        /* 返回 I/O 错误。 */
        return SESSION_STORE_STATUS_IO_ERROR;
    }
    /* 确保扩容到固定容量。 */
    const session_store_status_t capacity_status = session_file_ensure_capacity(
        file_backend,
        (size_t)size_long);
    /* 扩容失败时关闭。 */
    if (capacity_status != SESSION_STORE_STATUS_OK) {
        /* 关闭文件。 */
        (void)fclose(file);
        /* 清空上下文。 */
        (void)memset(file_backend, 0, sizeof(*file_backend));
        /* 返回具体错误。 */
        return capacity_status;
    }
    /* 标记初始化成功。 */
    file_backend->initialized = true;
    /* 返回成功。 */
    return SESSION_STORE_STATUS_OK;
}

void session_file_backend_close(session_file_backend_t *file_backend)
{
    /* 空对象或重复关闭安全无操作。 */
    if ((file_backend == NULL) || !file_backend->initialized ||
        (file_backend->file_handle == NULL)) {
        /* 没有资源。 */
        return;
    }
    /* 取得文件句柄。 */
    FILE *file = session_file_handle(file_backend);
    /* 尽力刷新；关闭路径不抛错误，提交操作已在业务层检查 sync。 */
    (void)fflush(file);
    /* 关闭句柄。 */
    (void)fclose(file);
    /* 清空上下文，防止悬空句柄复用。 */
    (void)memset(file_backend, 0, sizeof(*file_backend));
}

session_store_backend_t session_file_backend_interface(
    session_file_backend_t *file_backend)
{
    /* 构造按值函数表；未初始化对象仍返回表，具体操作会失败。 */
    const session_store_backend_t backend = {
        /* 透传长期有效上下文。 */
        .context = file_backend,
        /* 未初始化时容量为零。 */
        .capacity = file_backend == NULL ? 0U : file_backend->capacity,
        /* 注册随机读取。 */
        .read = session_file_read,
        /* 注册随机写入。 */
        .write = session_file_write,
        /* 注册 0xFF 擦除。 */
        .erase = session_file_erase,
        /* 注册强制同步。 */
        .sync = session_file_sync,
    };
    /* 返回函数表副本。 */
    return backend;
}
