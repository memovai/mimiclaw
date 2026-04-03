# 嵌入式C Doxygen 注释规范

## 一、核心原则

- 够用 > 完整，删掉所有"仪式感"标签，只留真正有信息量的内容。

- 统一使用JavaDoc风格

## 二、统一使用JavaDoc风格

### 风格细节：`///` 单行 + `/** */` 块注释混用

```c
/// 简短说明用单行，IDE 悬浮提示效果最好
int foo(void);

/**
 * 复杂函数用块注释
 * 多行描述时更清晰
 */
int bar(int x, int y);
```

## 三、头文件 API 注释模板（重点）

### `.h` 文件是对外契约，注释要最完整。

```c
/**
 * @brief 初始化 UART 外设
 *
 * 必须在任何收发操作前调用。重复调用会重置 FIFO 并
 * 重新配置波特率，调用期间会短暂禁用中断。
 *
 * @param[in] port     串口号，取值 UART_PORT_1 / UART_PORT_2
 * @param[in] baudrate 波特率，建议使用标准值（9600/115200）
 * @param[in] cfg      详细配置项，见 uart_config_t
 *
 * @return  0   成功
 * @return -1   端口号非法
 * @return -2   硬件初始化失败（检查时钟配置）
 *
 * @note 调用前需确保对应 GPIO 已完成复用配置
 * @warning 非线程安全，FreeRTOS 环境下需加锁
 */
int uart_init(uart_port_t port, uint32_t baudrate, const uart_config_t *cfg);
```

### 标签取舍原则

| 标签                  | 是否保留 | 原因                         |
| --------------------- | -------- | ---------------------------- |
| `@brief`              | ✅ 必须   | IDE悬浮提示的第一行          |
| `@param[in/out]`      | ✅ 必须   | 区分输入输出，嵌入式指针很多 |
| `@return` / `@retval` | ✅ 必须   | 错误码含义最容易忘           |
| `@note`               | ✅ 保留   | 时序依赖、前置条件           |
| `@warning`            | ✅ 保留   | 线程安全、不可重入等陷阱     |
| `@see`                | ⚠️ 按需   | 有强关联函数时才加           |
| `@author` `@date`     | ❌ 删除   | 交给 Git blame               |
| `@version` `@file`    | ❌ 删除   | 不生成文档无意义             |

## 四、`.c` 文件内部函数注释模板（轻量）

内部函数不对外暴露，注释要轻，别增加维护负担：

c

```c
/**
 * @brief 将接收缓冲区数据搬运到环形队列
 *
 * @param[in]  src  DMA 缓冲区起始地址
 * @param[in]  len  本次有效数据长度（字节）
 * @return 实际写入环形队列的字节数
 */
static int uart_flush_dma_to_ring(const uint8_t *src, size_t len);
```

**`static` 函数可以更精简，只写 `@brief` + 必要的 `@param` 即可。**

## 五、状态机注释规范（重点）

状态机逻辑是最容易让队友迷失的地方，注释要讲清楚"为什么"。

### 5.1 状态枚举

c

```c
/**
 * @brief 设备连接状态机状态定义
 *
 * 状态转移图：
 *   IDLE ──connect()──> CONNECTING ──成功──> CONNECTED
 *                            │                   │
 *                          超时/失败           disconnect()
 *                            │                   │
 *                            └──────> ERROR <─────┘
 *                                       │
 *                                   retry() / reset()
 *                                       │
 *                                     IDLE
 */
typedef enum {
    DEV_STATE_IDLE       = 0,  ///< 空闲，未发起连接
    DEV_STATE_CONNECTING = 1,  ///< 正在握手，等待响应
    DEV_STATE_CONNECTED  = 2,  ///< 连接建立，可正常通信
    DEV_STATE_ERROR      = 3,  ///< 错误状态，需要恢复处理
} dev_state_t;
```

### 5.2 状态处理函数

c

```c
/**
 * @brief 处理 CONNECTING 状态下的超时事件
 *
 * @details
 * 触发条件：连接发起后 @ref CONNECT_TIMEOUT_MS 内未收到 ACK
 * 执行动作：
 *   1. 重试次数 +1
 *   2. 重试次数 < MAX_RETRY → 重发握手包，维持 CONNECTING
 *   3. 重试次数 >= MAX_RETRY → 切换至 ERROR，上报错误码
 *
 * @note 此函数只能在主循环 / 同一任务上下文中调用
 * @see  DEV_STATE_CONNECTING, on_connect_success()
 */
static void on_connect_timeout(dev_ctx_t *ctx);
```

### 5.3 状态转移动作注释

c

```c
static void state_machine_run(dev_ctx_t *ctx, dev_event_t event)
{
    switch (ctx->state) {
        case DEV_STATE_IDLE:
            if (event == EVT_CONNECT_REQ) {
                /* → CONNECTING: 发送握手包并启动超时定时器 */
                send_handshake(ctx);
                timer_start(ctx->timeout_timer, CONNECT_TIMEOUT_MS);
                ctx->state = DEV_STATE_CONNECTING;
            }
            break;

        case DEV_STATE_CONNECTING:
            if (event == EVT_ACK_RECEIVED) {
                /* → CONNECTED: 停止定时器，重置重试计数 */
                timer_stop(ctx->timeout_timer);
                ctx->retry_count = 0;
                ctx->state = DEV_STATE_CONNECTED;
            } else if (event == EVT_TIMEOUT) {
                on_connect_timeout(ctx);  /* 内部处理重试/ERROR切换 */
            }
            break;
        /* ... */
    }
}
```

## 六、STM32 / ESP32 特殊场景

### 寄存器操作

```c
/**
 * @brief 配置 TIM2 为 1kHz PWM 输出
 *
 * @note 依赖 APB1 时钟 = 84MHz（请确认 SystemClock_Config）
 *       ARR = 840-1, PSC = 99 → 84M / 100 / 840 = 1kHz
 */
static void tim2_pwm_init(void);
```

### ISR / 中断函数

```c
/**
 * @brief UART1 接收中断处理
 *
 * @warning ISR上下文，禁止调用任何阻塞函数（HAL_Delay等）
 * @note   数据写入环形缓冲区，由主循环消费
 */
void USART1_IRQHandler(void);
```

### ESP32 FreeRTOS 任务

```c
/**
 * @brief MQTT 消息处理任务
 *
 * @param[in] pvParameters 传入 mqtt_ctx_t 指针（由 xTaskCreate 传递）
 *
 * @note 运行于独立任务，栈大小 4096 字节
 * @warning 不可在中断上下文中调用
 */
static void mqtt_task(void *pvParameters);
```

## 结构体和宏的注释

```c
/**
 * @brief UART 初始化配置项
 * @note  所有字段必须在调用 uart_init() 前填充
 */
typedef struct {
    uint32_t baudrate;    ///< 波特率，推荐 9600 / 115200
    uint8_t  data_bits;  ///< 数据位，固定填 8
    uint8_t  stop_bits;  ///< 停止位：1 = 1位，2 = 2位
    uint8_t  parity;     ///< 校验：0=无 1=奇 2=偶
    bool     flow_ctrl;  ///< 是否启用硬件流控（RTS/CTS）
} uart_config_t;
```

```c
#define MAX_RETRY       3     ///< 连接最大重试次数，超出后进入 ERROR 状态
#define TIMEOUT_MS      500   ///< 单次连接超时时间（ms），依赖 SysTick 精度
```

> **原则：魔法数字必须注释，说明"为什么是这个值"比"这是什么值"更重要。**

------

## 全局变量注释

嵌入式全局变量多，尤其是 ISR 和主循环共享的变量：

```c
/// 环形缓冲区写索引，仅由 ISR 写入，主循环只读
static volatile uint16_t g_rx_write_idx = 0;

/// 当前设备状态，所有状态转移必须通过 state_machine_run() 修改
static dev_state_t g_dev_state = DEV_STATE_IDLE;
```

------

## 注释的"坏味道"反例

比规范本身更值得警惕，避免发生下面的情况

```c
/* ❌ 复述代码，零信息量 */
i++;  /* i 加 1 */

/* ❌ 过期注释比没有注释更危险 */
// 已弃用，改用 v2 接口（但下面代码还在用 v1）
uart_init_v1();

/* ❌ @brief 写了废话 */
/**
 * @brief 初始化函数
 */
void uart_init(void);  // 函数名已经说明一切了

/* ✅ 正确：注释说代码没说的事 */
/**
 * @brief 初始化 UART1
 * @note  必须在 MX_GPIO_Init() 之后调用，否则 TX 引脚电平异常
 */
void uart_init(void);
```

