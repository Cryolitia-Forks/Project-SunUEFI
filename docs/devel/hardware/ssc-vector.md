# SSC 发现与原始报告

libssc 扩展复用已有 FastRPC/SSC 传输，提供完整元数据发现、原始报告和向量解码。`piano-ssc-vector` 是限时诊断客户端，不作为服务自启动，也不创建 IIO 设备。SSC 端点存在、成功订阅和硬件实际产生有效读数是三个不同的结果。

共享补丁由 `config/release.json` 的 `sensors.patches` 接入 `./build.sh sensors`，经标准 Debian quilt/Meson 流程构建。`0003-libssc-raw-vector-reports.patch` 提供报告接口及客户端，`0005-libssc-metadata-discovery.patch` 增加元数据发现。后者的 Debian 版本是 `0.4.4-2+piano1+sunuefi3`，客户端对新符号的依赖由 `libssc2.symbols` 记录。Nix 构建应用同一组 libssc 补丁。

## 只发现元数据

只发现模式发送一次 SUID 请求，再为返回的每个 UID 请求属性，不启用采样、不订阅属性更新、不修改 registry。默认只查询该类型的默认实例，`--all` 查询全部实例：

```sh
piano-ssc-vector --probe --sensor sar_sensor
piano-ssc-vector --probe --all --sensor sar_sensor
```

输出先给出 `discovered` 数量，再逐个输出 UID、全部属性及原始属性 protobuf。明确的空 UID 回复输出 `discovered=0`，与取消、超时、传输失败分别处理。SAR 的准确 SSC 类型是 `sar_sensor`；`sar`、Android 的 `xiaomi.sensor.sar` 和芯片名称不能互换。

公开异步接口位于 `<libssc-discovery.h>`，也由 `<libssc.h>` 引入：

```c
ssc_sensor_discover ("sar_sensor", FALSE, cancellable, discovered, user_data);
/* 在回调中： */
GVariant *sensors = ssc_sensor_discover_finish (result, &error);
```

第二个参数为 `TRUE` 时查询默认实例，`FALSE` 时保留全部实例。返回值由调用方释放，类型为 `aa{sv}`；每一项包含：

| 字段 | 类型 | 内容 |
| --- | --- | --- |
| `data-type` | `s` | 请求的准确 SSC 类型 |
| `uid-high`、`uid-low` | `t` | 原始 128 位 SUID 的两部分 |
| `attributes` | `a(iaa{sv})` | 原始属性编号及该属性的全部值 |
| `attributes-raw` | `ay` | 完整属性 protobuf，保留未知字段和浮点位模式 |

属性值字典沿用 protobuf 字段名：`a` 是嵌套值数组，`s` 是字符串，`f` 是浮点数，`i` 是原始无符号 64 位整数，`b` 是布尔值。属性顺序和同一属性的多个值均保留，不将 vendor 字段猜测为物理单位或校准状态。

接口接受 `GCancellable`；调用方在自己的超时或退出路径取消。收到空 UID 列表时成功返回空数组，收到损坏的 protobuf 或传输错误时返回 `GError`。调用现有 `ssc_sensor_new()` 仍只初始化单个默认实例并提取常用属性；只有随后调用 `ssc_sensor_open()` 才启用报告。

## 限时读取原始报告

采样使用准确 SSC 类型，默认读取三秒；`--seconds` 接受 1–30 秒：

```sh
piano-ssc-vector --sensor sar_sensor --seconds 3
piano-ssc-vector --sensor cct_front --seconds 3
piano-ssc-vector --sensor ambient_light_back --seconds 3
piano-ssc-vector --sensor flicker --seconds 3
```

客户端使用现有 `SSCSensor` 的 `open/close` 流程，支持 libssc 的 continuous 和 on-change 订阅。其他 stream type 返回不支持错误。`--all` 仅用于发现，不会同时启用所有实例。

`SSCSensor::raw-report(sensor, guint message_id, GBytes *payload)` 只转发与对象 SUID 匹配的原始报告。`GBytes` 在回调内借用，延长使用需调用 `g_bytes_ref()`。信号运行在现有 report dispatch context，GUI 应用需转交到自己的主循环。未订阅该信号时不复制载荷，原有强类型 measurement 信号和采样策略保持兼容。

输出包含原始 message ID、完整 payload hex，以及已支持布局的解码结果。未知 ID 保留原始输出；不能把“收到字节”当成已确定通道含义。SAR 需先核对原厂解析器与实机报告，才能给各分量命名；不能用光学接近传感器的数据替代。

Ctrl-C 或期限到达后调用现有 close API，收到关闭回复才输出 `closed=true`。发现或 open 超时会取消，并等待最多五秒处理关闭；停止后晚到的发现结果不会继续 open。程序不会写入 registry 或运行校准。

## 离线向量解码

`ssc_sensor_decode_vector_report()` 返回由调用方持有的 `GVariant a{sv}`，当前解释以下已经核对的布局：

| 原始事件 ID | 返回字段 | 含义边界 |
| --- | --- | --- |
| 1025 | `values: ad`、`accuracy: i` | field1 全部 float32、field2 原始 int32 |
| 769 | `field1: i`、`field2: i`、`field3: i`、`values: ad` | 三个原始 int32、field4 全部 float32 |

float32 转为 double 保持数值；原始位模式仍在 `GBytes`。769 的三个整数只按字段号命名，不推断频率、精度或物理单位。未知 ID 返回 `G_IO_ERROR_NOT_SUPPORTED`，损坏或不完整的已支持布局返回 `G_IO_ERROR_INVALID_DATA`，原始载荷始终可以单独保存。

```sh
piano-ssc-vector --decode 1025 --payload-hex 0a04000000001003
```

手动编译仓库中的同一客户端时，使用应用上述补丁的开发库：

```sh
cc -O2 -Wall -Wextra linux/userspace/piano-ssc-vector.c \
  -o piano-ssc-vector $(pkg-config --cflags --libs libssc gio-unix-2.0)
```

传输与原有六类传感器测试沿用上游 libssc。新增元数据测试直接编译真实发现逻辑和 protobuf codec，只替换 SSC 传输，检查 default/all、多个 UID、嵌套属性、空回复、损坏回复、传输失败和取消，并拒绝任何采样或 registry 写请求。构建后运行 `meson test --setup=metadata tests-discovery`，无需 QRTR 设备。
