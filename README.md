# RC Receiver 组件

RC Receiver 提供统一的遥控器数据接口。上层只需要选择协议驱动并调用
`rc_receiver_init()`、`rc_receiver_set_callback()`；UART 参数、帧解析、校验、
断线重连和协议默认值都由驱动负责。需要同步处理时仍可使用非阻塞的
`rc_receiver_read()`。

协议名称和通信方式是两个独立维度。协议 driver 通过注册表选择，通信方式通过对应
的 factory 选择。当前仓库实际使用的通信方式只有 UART：

```text
rc_receiver_alloc_uart("sbus", ...)
```

当前仓库只实现 `drv_uart_sbus`。后续只有在仓库里真正加入 CAN、网络、SPI、PWM/PPM
或 USB/HID driver 后，才增加对应 transport 的枚举、参数结构和 factory；不会提前暴露
没有实现的通信方式。

## 当前驱动

| 驱动 | 总线 | 说明 |
| --- | --- | --- |
| `drv_uart_sbus` | UART | SBUS 25 字节帧，支持经典帧尾和可选 S.Bus2 帧尾 |

后续 UART 协议（例如 CRSF、IBUS）应新增对应的
`src/drivers/drv_uart_<protocol>.c`，实现同一套内部 `rc_receiver_ops`，并通过
`REGISTER_RC_RECEIVER_DRIVER()` 注册。只要输出公共归一化 frame，核心层和应用层就不
需要了解协议帧格式。将来出现其他通信方式时，按 IMU、Motor、Lidar 的模式，在有真实
driver 实现的同一变更中再增加对应 transport factory。

## 构建

```bash
cd components/peripherals/rc_receiver
cmake -S . -B ../../output/test-artifacts/rc_receiver \
    -DBUILD_TESTS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build ../../output/test-artifacts/rc_receiver -j
ctest --test-dir ../../output/test-artifacts/rc_receiver --output-on-failure
```

SDK 构建可以通过 `SROBOTIS_PERIPHERALS_RC_RECEIVER_ENABLED_DRIVERS` 选择驱动；独立
构建未指定时默认启用 `drv_uart_sbus`。

示例程序默认使用 `sbus`，也可以通过 `RC_RECEIVER_DRIVER` 环境变量选择其他已编译的
UART driver，例如 `RC_RECEIVER_DRIVER=crsf ./build/test_rc_receiver /dev/ttyS5`。

## 通用 API

```c
#include "rc_receiver.h"
#include <stdio.h>

static void on_frame(struct rc_receiver *receiver,
        const struct rc_receiver_frame *frame, void *context)
{
    (void)receiver;
    (void)context;
    printf("CH1=%u\n", (unsigned int)frame->channels[0]);
}

int main(void)
{
    struct rc_receiver *receiver;

    /* name 是注册的协议驱动名，也可以写成 "sbus:main"。 */
    receiver = rc_receiver_alloc_uart("sbus", NULL, 0U, NULL);
    if (receiver == NULL || rc_receiver_init(receiver) != 0) {
        rc_receiver_free(receiver);
        return -1;
    }

    if (rc_receiver_set_callback(receiver, on_frame, NULL) != 0) {
        rc_receiver_free(receiver);
        return -1;
    }
    /* 程序在这里处理其他任务，完整帧会由后台线程送到 on_frame()。 */

    rc_receiver_set_callback(receiver, NULL, NULL);

    rc_receiver_free(receiver);
    return 0;
}
```

`uart_dev == NULL` 或 `baudrate == 0U` 表示使用所选驱动的默认值。SBUS 驱动当前的
默认值是 `/dev/ttyS5`、`100000 8E2`，这些值只定义在驱动实现中，不会污染通用头文件。
SBUS 回调线程通过 Linux `poll()` 阻塞等待 UART 可读事件，串口没有数据时不会定时调用
`read()`。frame 指针只在本次回调期间有效。回调应快速返回，上下文释放前需要传入
`NULL` 停止回调；`rc_receiver_free()` 和 `rc_receiver_close()` 也会先停止回调。
启用回调期间不能同时调用 `rc_receiver_read()`。

`rc_receiver_read()` 保留为非阻塞轮询接口：返回 `RC_RECEIVER_FRAME` 表示得到完整帧，
返回 `RC_RECEIVER_NO_DATA` 表示当前没有完整帧。设备未连接或断开时，SBUS 驱动会按约
1 秒周期重试打开串口。`rc_receiver_close()` 会恢复打开前保存的串口配置。
`timestamp_us` 统一使用 `CLOCK_MONOTONIC`，适合计算帧间隔。

`struct rc_receiver_frame` 的 `channels` 是统一归一化后的协议输出，范围固定为
`RC_RECEIVER_CHANNEL_MIN..RC_RECEIVER_CHANNEL_MAX`，中心值约为
`RC_RECEIVER_CHANNEL_CENTER`。`channel_count` 不应超过
`RC_RECEIVER_CHANNEL_COUNT`。链路状态位包括 `RC_RECEIVER_FLAG_FRAME_LOST` 和
`RC_RECEIVER_FLAG_FAILSAFE`；不提供某项状态的协议
驱动应清零对应位。

SBUS 25 字节帧由驱动解析为 16 个 11-bit 通道，并转换到统一的
`0..65535` 范围。具体产品使用哪些通道应由产品配置或上层映射决定，不能写死在
协议 driver 中。SBUS 的 CH17、CH18 是协议特有的一位数字通道，不暴露到通用 frame；
通用 `flags` 只表示丢帧和 failsafe 链路状态。
离线解析测试通过测试文件中的私有声明调用驱动内部函数，不作为 SDK 公共 API 发布。

## 目录结构

```text
include/rc_receiver.h                 通用公共 API
src/rc_receiver_core.c                驱动注册、工厂和通用生命周期
src/rc_receiver_core.h                私有驱动契约
src/drivers/drv_uart_sbus.c           SBUS 协议与 Linux UART 驱动
example/test_rc_receiver.c            SBUS 通道诊断程序
tests/                                 协议、API 和硬件测试
```

## 硬件测试

SBUS 是反相 TTL 信号，连接方式如下：

```text
SBUS 接收机 TX -> 外部 SBUS 反相器 -> UART RX
SBUS 接收机 GND ---------------------> UART GND
```

```bash
../../output/test-artifacts/rc_receiver/test_rc_receiver /dev/ttyS5
```

也可以运行 `tests/test_hw_sbus_uart_smoke.sh`，通过 `RC_RECEIVER_DEVICE` 和
`RC_RECEIVER_TEST_BINARY` 覆盖设备节点和诊断程序路径。未连接硬件时该 smoke 测试会
跳过；协议解析和 API 测试不依赖硬件。

## 代码规范与验证

本组件遵循仓库 `docs/coding_standards.md`：公共头文件只暴露稳定接口，驱动实现放在
`src/drivers`，C 文件和标识符使用 snake_case，缩进使用 4 个空格，建议行宽不超过
120 字符。

提交前执行：

```bash
bash scripts/lint/lint_cpp.sh components/peripherals/rc_receiver
cmake -S components/peripherals/rc_receiver -B build -DBUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

源码文件头声明为 Apache-2.0，最终以本目录 `LICENSE` 文件为准。
