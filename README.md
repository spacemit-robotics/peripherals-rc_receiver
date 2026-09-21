# RC Receiver 组件

RC Receiver 提供统一的遥控器数据接口。当前实现为 UART SBUS，支持同步轮询和异步
回调两种读取方式；协议解析、校验和设备管理封装在组件内部。

## 当前驱动

| 驱动 | 总线 | 说明 |
| --- | --- | --- |
| `drv_uart_sbus` | UART | SBUS 25 字节帧，支持经典帧尾和可选 S.Bus2 帧尾 |

新增协议时，在 `src/drivers/` 实现同一套内部 `rc_receiver_ops`，并通过
`REGISTER_RC_RECEIVER_DRIVER()` 注册。协议驱动只输出通用 frame，不定义具体产品的通道功能。

## 构建与测试

```bash
cd components/peripherals/rc_receiver
cmake -S . -B ../../output/test-artifacts/rc_receiver \
    -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ../../output/test-artifacts/rc_receiver -j
ctest --test-dir ../../output/test-artifacts/rc_receiver --output-on-failure
```

独立构建未指定驱动时默认启用 `drv_uart_sbus`；SDK 构建可通过
`SROBOTIS_PERIPHERALS_RC_RECEIVER_ENABLED_DRIVERS` 选择驱动。

## 最小 API 示例

```c
#include "rc_receiver.h"
#include <stdio.h>

static void on_frame(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame, void *context)
{
    (void)receiver;
    (void)context;
    /* frame->channels[0] 是 CH1。 */
    printf("CH1=%u\n", (unsigned int)frame->channels[0]);
}

int main(void)
{
    struct rc_receiver *receiver =
        rc_receiver_alloc_uart("sbus", NULL, 0U, NULL);
    if (receiver == NULL || rc_receiver_init(receiver) != 0) {
        rc_receiver_free(receiver);
        return -1;
    }
    if (rc_receiver_set_callback(receiver, on_frame, NULL) != 0) {
        rc_receiver_free(receiver);
        return -1;
    }

    /* 退出前注销回调，再释放对象。 */
    rc_receiver_set_callback(receiver, NULL, NULL);
    rc_receiver_free(receiver);
    return 0;
}
```

`rc_receiver_alloc_uart()` 的 name 是注册的协议驱动名，也可以写成 `sbus:main`。
设备路径或波特率传入 `NULL`/`0U` 时使用驱动默认值。当前 SBUS 默认值为
`/dev/ttyS5`、`100000 8E2`。

## 读取行为

| 项目 | 行为 |
| --- | --- |
| 同步读取 | `rc_receiver_read()` 是非阻塞轮询接口 |
| 异步读取 | core worker 使用 `poll()` 等待 UART 和停止 eventfd |
| 无数据 | 返回 `RC_RECEIVER_NO_DATA` |
| 得到完整帧 | 返回 `RC_RECEIVER_FRAME`，回调中的 frame 指针只在本次调用有效 |
| 回调期间读取 | 返回 `RC_RECEIVER_ERROR`，并设置 `errno=EBUSY` |
| 初始化前注册 | 允许；回调启动错误由 `rc_receiver_init()` 报告 |
| 注销回调 | 传入 `NULL`，等待在途回调结束 |
| UART 异步断开 | 停止回调并记录错误；不会自动重连，需注销后重新初始化 |
| UART 同步断开 | 驱动按约 1 秒周期重试打开串口 |

外部生命周期操作必须串行执行。回调中可以查询诊断信息或注销自身，但不能注册新回调、
调用 `close()` 或 `free()`；自身注销后仍需由外部完成线程回收。

## SBUS 数据

SBUS 25 字节帧解析为 16 个 11-bit 通道，并转换到统一的 `0..65535` 范围。通道号从
1 开始，因此 `frame->channels[0]` 对应 CH1。`channel_count` 不超过
`RC_RECEIVER_CHANNEL_COUNT`；链路状态位包括 `RC_RECEIVER_FLAG_FRAME_LOST` 和
`RC_RECEIVER_FLAG_FAILSAFE`。

SBUS 的 CH17、CH18 是协议特有的一位数字通道，不暴露到通用 frame。具体产品使用哪些
通道、每个通道对应哪个摇杆或按键，应由上层应用配置，不能写死在协议驱动中。

## 硬件连接

SBUS 是反相 TTL 信号：

```text
SBUS 接收机 TX -> 外部 SBUS 反相器 -> UART RX
SBUS 接收机 GND ---------------------> UART GND
```

```bash
../../output/test-artifacts/rc_receiver/test_rc_receiver /dev/ttyS5
```

也可以运行 `tests/test_hw_sbus_uart_smoke.sh`，通过 `RC_RECEIVER_DEVICE` 和
`RC_RECEIVER_TEST_BINARY` 覆盖设备节点和测试程序路径。未连接硬件时 smoke 测试会跳过。

## 目录结构

```text
include/rc_receiver.h                 公共 API
src/rc_receiver_core.c                驱动注册和通用生命周期
src/rc_receiver_core.h                私有驱动契约
src/drivers/drv_uart_sbus.c           SBUS 协议与 UART 驱动
example/test_rc_receiver.c            诊断示例
tests/                                 协议、API 和硬件测试
```

## License

源码文件头声明为 Apache-2.0，最终以本目录 `LICENSE` 文件为准。
