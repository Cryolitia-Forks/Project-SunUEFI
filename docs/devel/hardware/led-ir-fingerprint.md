# Piano 的 RGB 灯、红外与侧指纹

本页记录 RGB 通道、SPI 红外和 Goodix 侧指纹的原厂接口与 Linux 实现。指纹客户端提供初始化和受限存储探测，桌面认证还需要 libfprint 后端。

| 功能 | 原厂接口 | 当前 Linux 缺口 |
| --- | --- | --- |
| RGB 通道 | PM8550 SID1 上的 `qcom,pm8350c-pwm`，SPMI LPG，通道 1/2/3 为 red/green/blue | 标准驱动和产品DT已有；补齐SDAM/LPG模块加载后已出现三个标准LED接口。发光、可见位置和用途尚待确认。 |
| 红外发射 | SE15 / SPI `0x89c000` / CS0，`ir-spi`，Android 原始波形字符设备 | 已接入标准 SPI/LIRC、原厂波形量化与 MOSI 空闲处理；光学输出未验证。 |
| 侧指纹 | Goodix，GPIO 控制与高通 QTEE 安全服务 | 供电、复位、中断、初始化和受限 FS/GPFS 探测已接通；录入、认证和 fprintd 接入尚未完成。 |

## RGB 通道

原厂 `/soc/qcom,spmi@c42d000/qcom,pm8550@1/led-controller` 绑定 `qcom-spmi-lpg` / `leds_qcom_lpg`。三个子节点原样指定 `reg=1/2/3`、红/绿/蓝颜色和相应 label。其两个 nvmem 依赖是 **PMK8550 SID0 的 `sdam@8400`、`sdam@8500`**，用于 LPG 通道状态和图案数据，不是相机闪光灯的 `0xee00` 外设。

当前产品 `board.dtb` 已保留上述节点和依赖，节点未写 status，按 DT 规则启用。当前配置已有 `LEDS_QCOM_LPG=m`、`LEDS_CLASS_MULTICOLOR=m`、`NVMEM_SPMI_SDAM=m`；[上游 LPG 驱动](https://github.com/torvalds/linux/blob/master/drivers/leds/rgb/leds-qcom-lpg.c)已经支持该 compatible、通道和标准 LED class，[绑定说明](https://github.com/torvalds/linux/blob/master/Documentation/devicetree/bindings/leds/leds-qcom-lpg.yaml)也接受这些字段。**无需再加一个重复 RGB overlay。**

Linux起初没有RGB，LPG的deferred-probe原因为未取得LPG chan SDAM。正确提供者模块名为 `nvmem_qcom_spmi_sdam`；加载它及 `leds_qcom_lpg` 后，platform device绑定 `qcom-spmi-lpg`，出现 `red`、`green`、`blue`，初始brightness为0、max_brightness为255。没有读取SDAM内容或写brightness，尚不证明可见灯的位置或用途。

[通用模块加载文件](../../../linux/bsp/common/etc/modules-load.d/piano-rgb.conf)已写入BSP manifest和完整root stager，也已安装到当前平板；下一次正常启动会按同一策略加载，不添加daemon或重复DT节点。记录在 `内部记录目录 `sdam@7000/sm1510_present@5d` cell 曾因无效位范围阻止其提供者注册；打包器现保留完整原始字节并移除无效位切片，详见[启动设备树处理](../linux/kernel.md#piano-启动配置)。LPG 使用的是 8400/8500 两个 SDAM。

## 红外发射

Piano 的发送器通过标准 rc-core/LIRC 接口接收脉冲与间隔，设备为
`/dev/lircN`。无需 Android HAL 或常驻服务，`ir-ctl` 由 v4l-utils 提供。
发送时可用 `ir-ctl -d /dev/lirc0 --carrier=38000 --send=remote.txt`，文件使用
ir-ctl 的 `pulse` / `space` 文本格式。接口注册和编译不代表光学输出已经测量；
首次上机需要核对实际载波、包络以及发送后的熄灭状态。

原厂最终设备树将发送器放在
`/soc/qcom,qupv3_2_geni_se@8c0000/spi@89c000/ir-spi@0`，即
SE15 / QUP2 SE7 / CS0。GPIO28/29/30/31 分别为 MISO/MOSI/CLK/CS，
子设备最大时钟为 19.2 MHz。
[原厂驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/media/rc/ir-spi.c)
使用私有 `/dev/ir_spi` 位图接口。Linux 适配保留原厂接线与波形量化，改用
`xiaomi,piano-ir-spi` 匹配标准 ir-spi 驱动，不要求原厂未描述的供电开关。

原厂 `/odm/lib64/hw/consumerir.qcom.so` 的 SHA256 为
`fc9e6f2c7770b0f5eb765af8e6eec17494ba7a081a47051606188bec17164f9a`。
其固定 1.92 MHz 采样时钟、约 40% 默认占空比、原生 32 位 MSB-first
字序和低电平间隔由编码器保留。载波接受 30–56 kHz，并支持 LIRC 占空比设置。
38 kHz 请求量化为 51 点周期、20 点高电平，名义载波约 37647 Hz。
累计时间决定采样边界，不沿用原厂减 5 µs 的时间修正。

每次请求须为正值、奇数段、最多 1024 段。缓冲区在完整零 word 后对齐到
1024 字节，包含填充最多 32 KiB，约能容纳 136 ms 报文；超长返回 `EMSGSIZE`。
当前 GPI 只接受单个 DMA 段，不能把长报文简单拆成多个 SPI transfer 后
假定时钟连续。空调等长报文需要先补齐 GPI 的多 TRE 连续传输和完成事件处理。

共享 [`设备树`](../../../linux/dts/piano-ir-spi-controller.dtso)沿用原厂
GCC ID123、GIC SPI462、GPI2 SE7 和 IOMMU 路由。运行与休眠的差异只在
GPIO29：idle/sleep 状态将 MOSI 主动拉低，其余引脚沿用原厂 GPIO 休眠配置。
GENI 驱动在消息开始时选择 default，在队列空闲且 SE 确认停止后选择 idle；
自动休眠选择相同的低电平状态。IR 客户端不直接抢占控制器引脚。

发送超时后，GENI 必须等两路 GPI 通道确认终止才返回 SPI 核心，避免核心
提前解除仍被 DMA 使用的缓冲区映射。终止失败时保留消息、映射、时钟和引脚，
继续恢复；不把超时当成硬件已停止。以上改动分别在 Linux 7.3
[`series.json`](../../../patches/linux/7.3/series.json) 的 0025–0027 中维护，
`RC_CORE`、`LIRC` 和 `IR_SPI` 配置由共享平台片段启用。

## 侧指纹

HyperOS 4 的 `mfp-daemon` 使用 Goodix HAL、`/dev/xiaomi-fp`
和高通 QTEE。`libQSEEComAPI.so` 通过 `libminkdescriptor.so` 打开
UID 122 的 QSEECOMPAT 服务，再加载名为 **`gfpia`** 的签名安全程序。
原厂进程的映射和文件描述符已确认这条路径，不能套用其他小米平板的
FPC 驱动或 Goodix USB 驱动。

Linux 7.3 的 `qcomtee` 驱动提供标准 `/dev/tee0`。
使用高通公开的 [quic-teec](https://github.com/quic/quic-teec) 客户端，
已在实机取得同一个 QSEECOMPAT 服务，并成功加载、释放由原厂分段文件
还原的完整签名 `gfpia`。程序大小为 11,681,880 字节，QTEE 版本为
5.2.0；安全程序和加载接口均没有修改或绕过签名。

原始 QTEE 共享内存分配器使用 `alloc_pages_exact()`，超过伙伴分配器
支持的最大阶数时会报错。项目的
[`0014-qcomtee-large-shared-memory.patch`](../../../patches/linux/7.3/0014-qcomtee-large-shared-memory.patch)
让大缓冲区使用可回收的 CMA 连续内存，小缓冲区保留原来的路径，
并继续登记和注销安全世界的共享内存桥。
[`piano-cma.dtso`](../../../linux/dts/piano-cma.dtso) 将默认池从 36 MiB
扩大到 256 MiB，保留原厂允许的分配地址范围。这部分内存空闲时仍能
供 Linux 使用，不是永久划走的固件内存。

传感器控制依据[固定 MiCode 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/input/fingerprint/mi_fp/fp_driver.c)
提供供电、复位和中断：原厂 DT 指定 GPIO73 复位、GPIO85 IRQ，
`fp_3v3_vreg-supply` 指向 PM-HUMU L9。认证数据由安全服务处理；
Goodix 客户端使用 `QSEECom_send_modified_cmd()` 发送请求。
[`0017-uio-piano-fingerprint-control.patch`](../../../patches/linux/7.3/0017-uio-piano-fingerprint-control.patch)
提供内核供电、复位和中断控制，设备树位于
[`piano-fingerprint.dtso`](../../../linux/dts/piano-fingerprint.dtso)。
打开 `piano-goodix` UIO 设备时打开供电并释放复位，关闭时停止中断、
拉低复位并归还电源引用；没有 MMIO 映射，也不把中断当成认证结果。
Linux 已完成供电、复位和关闭。普通桌面应用的认证仍需 libfprint 后端。

原厂 `pm_humu_l9` 为 3.300 V。现有主线 PLDO 使用不同的离散电压档位；
必须选择不超过原厂上限的 3.296 V。之前上取整到 3.304 V 时，
`regulator_enable()` 尚未返回就发生系统复位；改用较低档位后，
打开与关闭均已正常完成。不要为适应档位把原厂电压上限放宽。

Goodix 客户端的设备信息命令为路由 1、命令 1000，128 字节请求、
64 字节响应，另带传感器共享缓冲区。Linux 能加载并正常关闭安全程序。
QSEECOMPAT 的 lookup 返回 `architecture=2`，命令应使用 64 位地址，
地址宽度按服务返回值选择；当前签名固件本身也是 ELF64。修正后传输层返回成功，
最初响应中的缓冲区状态为 `-1`，原因是小缓冲区来自 4 GiB 以上的伙伴
分配器。该安全程序接受的共享区域必须在 4 GiB 以下；使用默认 CMA
池中的 4 KiB 缓冲区后，传输、应用和缓冲区状态均返回成功，
传感器类型为 5。无需给每条命令申请大缓冲区，也无需为这个查询启动
文件／时间监听服务。

[`0018` 内核修正](../../../patches/linux/7.3/0018-qcomtee-public-buffers-cma.patch)
使应用共享内存使用设备树声明的连续内存池，保留驱动私有小消息的
普通分配方式。地址范围由池的配置决定，不通过伪造指针或用户空间
内存热补丁处理。

原生客户端位于
[`piano-goodix.c`](../../../linux/userspace/fingerprint/piano-goodix.c)，
使用固定版本的 Qualcomm MinkIPC／quic-teec 与 QCBOR，由四个发行版
共用的运行时构建器静态编译；来源记录在
[`config/fingerprint.json`](../../../config/fingerprint.json)。
执行 `sudo piano-fingerprint-ctl info` 可查询设备信息，`--help` 不访问设备。
签名程序按固件清单随镜像提供，不包含用户模板或密码。

`sudo piano-fingerprint-ctl initialize` 完成传感器初始化、原厂模型加载和
交互策略设置，再正常关闭传感器。实机返回传感器类型 5、芯片类型 49、
132 行 × 24 列阵列，以及 132 × 16 的导航区域。初始化响应中的芯片类型为零
表示沿用设备查询值，与原厂 HAL 的处理一致，不代表未知芯片。

该设备要求防伪和识别模型，两者均从同版原厂 `libgf_hal.so` 的导出数据符号
提取，属于通用算法参数，不是用户模板。CAP S／T 的选择依据设备返回的标志，
芯片类型 49 使用 `DLWeightLightDubaiU4_12_10_14MisTouch`。
来源库 SHA256、符号、文件偏移、大小和输出校验值全部记录在
[`固件清单`](../../../vendor/firmware/hyperos4-fingerprint/manifest.json)，
不会安装原厂可执行库。

本机原厂 HAL 的构建目标为 `xiaomi_P81_GF3656ZSCB_qsee5_sm8750_64bit_aidl_2025`。
`gf_hal_show_version`（`0x1ed64`）给出包版本 `V1.0.6.0.0` 和软件版本
`1.0.6.0.2.0`。当前国际版库与固件清单中的国内版库整文件摘要不同，但按 ELF
导出符号提取的三份模型逐字节相同，因此不需要替换 Linux 模型。两份完整签名
`gfpia` 的可加载代码和数据仅有三处构建时间字符串不同，另有对应签名元数据差异。
这些静态比对不代替实际录入或认证测试。

协议顺序如下，所有命令都通过公开 QTEE 接口进入原厂签名程序：

| 命令 | 功能 |
| --- | --- |
| 1000 | 查询传感器类型和功能标志 |
| 1001 | 读取传感器参数与校准数据 |
| 1113 | 分块加载防伪／识别模型；每块 511900 字节，逻辑模型命令为 1111 |
| 1005、1109 | 完成初始化并应用交互策略 |
| 1009 | 进入待机检测 |
| 1016 | 读取并确认传感器中断 |
| 1002 | 关闭传感器，随后关闭安全程序并归还供电 |

`sudo piano-fingerprint-ctl watch 30` 在限定时间内监听 UIO 中断，最多 120 秒。
事件接口位于 [`piano-goodix-events.c`](../../../linux/userspace/fingerprint/piano-goodix-events.c)，
由现有客户端调用；它将传输错误与安全程序返回的采集状态分开，保留事件包推进
阶段 4、5，完成后才重新启用 IRQ。输出包括事件标志、中断计数、操作、阶段和
应用状态，不保存或输出图像、模板 ID 和认证令牌。尚未接入的采集、导航阶段 2、8
返回不支持，并发送取消命令，不将未处理事件当成匹配结果。超时、Ctrl+C 和错误
退出都先执行真实取消，再关闭安全程序及 UIO。这里的时限在每条同步 QTEE 命令
返回后检查，不能中断一条尚未返回的安全调用。

上述字段来自原厂 Milan 函数表 `gf_hal_milan_function_customize`（`0x3f778`）：
取消函数 `0x40188` 发送命令 1009、`0x70` 字节；IRQ 函数 `0x41390` 发送
1016、`0xd134` 字节，设置 `+0x98=1`、`+0xa8=512`、`+0x2c5=0x02000000`。
响应的 `+0x6c` 是事件位图、`+0x70` 是操作、`+0x98` 是处理阶段。
`0x415a4–0x41610` 在阶段 4、5 保留原包再次调用 1016。阶段 2 的普通路径同样
保留原包继续 1016；特定录入模式将包扩至 `0x10e9e0` 字节。阶段 8 在处理导航
通知后清零 `+0x2f4` 起 `0x5a20` 字节，再继续交换。当前客户端尚未实现这两条
状态分支；静态调用链未发现必须运行 Android HAL 内专有匹配算法的环节。
`+0x2c5` 是输出 HAT 中的大端 `authenticator_type`，数值表示指纹类型 2，
不是可替代录入授权的令牌。`libgf_ca.so:gf_ca_invoke_command`（`0x4170`）将
64 字节响应中的第一个字作为应用状态返回，第二个字单独表示共享缓冲区错误；
不能把所有非零应用状态都当成 QTEE 传输失败。

`sudo piano-fingerprint-ctl list 1000` 创建 Linux 目录
`/var/lib/piano-goodix/1000`，将分组 ID 和路径交给命令 1014，再通过 1015
枚举模板。这个命令没有文件服务；传入 Linux 路径或返回空列表，都不能证明
安全程序的 FTS 存储已经独立。最大模板数量取自设备信息，并按响应容量解析。
标准 fprintd 用户元数据仍由后续的 libfprint 集成管理。

`storage-probe --state-dir <empty-directory> <group>` 显式启动受限 FS/GPFS
服务，初始化传感器并选择分组，随后关闭安全程序和服务。目录必须是新建的
Linux 空目录；服务将安全程序使用的虚拟路径映射到这里，不访问原厂目录。
初始化会在该目录生成校准文件，所以这是带本地写入的探测，不是只读查询。
命令不复制模板、不录入、不认证，也不执行删除或重命名。

文件服务位于 [`piano-goodix-storage.c`](../../../linux/userspace/fingerprint/piano-goodix-storage.c)。
普通 FS 仅提供受限打开、关闭和错误状态，GPFS 处理初始化所需的文件请求；
路径解析拒绝目录穿越、符号链接及外部文件描述符。空目录实机探测已完成
初始化和组选择，并生成四份校准元数据；这不代表 Android 模板可直接用于认证，
也不证明安全程序内部状态与 Android 已隔离。

[`MinkIPC 补丁系列`](../../../patches/minkipc/0.1.0/series.json)修正回调输出缓冲区的
重复释放与异常回收，并避免工作线程释放最后一个 root object 时自等待。
构建器在固定上游提交的副本中应用补丁，记录补丁及存储源码的摘要；原始源码缓存保持不变。

`sudo piano-fingerprint-ctl prepare-auth` 初始化传感器并完成 Keymaster 到
Goodix 的认证数据绑定。没有录入或认证操作，也不输出绑定数据。
这一步已在重新启动后的 Linux 上独立完成，不依赖 Android 先运行指纹服务。

原厂加载的 64 位 `keymaster64` 报告 API 5.3／TA 5.3000。
同版 `libkeymasterdeviceutils.so` 和 `libqtikeymint.so` 表明，原生服务 151
打开指定安全应用的环境，应用内服务 150 提供命令接口。客户端执行
`0x200`／`0x207` 协商；需要时通过 `0x220e` 取得测试启动的共享参数，
用 `0x220f` 完成 HMAC sharing。请求保留准确 CBOR 长度，不把对齐填充算入协议。

绑定请求仍沿用原厂 CA 的 `0x205, 2` 和 256 字节响应，再通过 Goodix 路由 2、
命令 109 传入签名安全程序。Goodix 必须先加载并初始化；之前在它尚未加载时
查询，会得到另一个错误。只有明确返回 `-10020` 时才初始化测试启动的
sharing key，已有绑定可用时直接使用，避免重置其他客户端的有效会话。
响应的偏移和长度均检查边界，临时参数和加密绑定数据在结束时清零。
没有把密钥、令牌或指纹模板保存到普通文件。

上述 FS/GPFS 只在显式 `storage-probe` 进程内提供，不增加默认后台服务。
原有[旧 GPFS 协议记录](../../../linux/bsp/patches/qtee/README.md)保留厂商接口说明；
它不是 Linux 文件系统隔离或 RPMB 写入授权的依据。

录入和认证的入口布局已由原厂 `libgf_hal.so` 静态确认，均使用路由 1：

| 操作 | 函数与命令 | 共享包内容 |
| --- | --- | --- |
| 生成录入挑战 | `gf_hal_common_pre_enroll`，`0x2115c`，1006／`0x78` 字节 | 挑战位于 `+0x70` |
| 开始录入 | `gf_hal_common_enroll`，`0x2161c`，1007／`0xbc` 字节 | 分组 `+0x6c`，真实的 69 字节 HAT 位于 `+0x76` |
| 结束录入 | `gf_hal_common_post_enroll`，`0x219b4`，1008／`0x6c` 字节 | 结束该次录入流程 |
| 开始认证 | `gf_hal_common_authenticate`，`0x21a18`，1010／`0x80` 字节 | 分组 `+0x6c`，操作 ID `+0x70`，`+0x78=-1` |
| 保存模板 | `gf_hal_save`，`0x20a74`，1012／`0x78` 字节 | 分组、模板 ID 位于 `+0x6c`、`+0x70` |
| 删除模板 | `gf_hal_common_remove`，`0x21f34`，1013／`0xcc` 字节 | 分组、模板 ID 位于 `+0x6c`、`+0x70`；返回数量在 `+0xc8` |

成功结果由 Milan 结果分发函数 `0x42308` 判断：应用状态为零后，再区分操作。
操作 1、2 的认证结果携带分组、模板 ID（`+0x80`、`+0x84`）及 HAT
（`+0x2ac`，69 字节）；`0x42774` 将这些字段交给认证成功回调。
`gf_hal_common_authenticate_success`（`0x248b4`）使用同样布局。录入操作 0
则读取剩余次数 `+0x88`，归零后必须经 1012 保存，不能仅凭手指中断或
进度归零报告录入成功。以上入口没有作为诊断客户端命令开放。

匹配重试状态 1064（`GF_ERROR_MATCH_FAIL_AND_RETRY`）需要将 `+0x98` 的
64 位值设为 2 并继续 1016。认证成功还需完成原厂的策略更新 1109，以及
`GF_CMD_AUTHENTICATE_FINISH`（1086、`0x148` 字节）；后者不是可省略的日志命令。

标准 libfprint 后端仍缺少三项实质功能：独立 Linux 凭据的合法 Gatekeeper/HAT
签发；完整的 1016 采集、重试、结果和取消状态机；可恢复的独立模板存储、备份与
删除事务。现有受限 FS/GPFS 只覆盖初始化，既没有按 TA 身份隔离回调，也未提供
完整模板事务。Android 的[标准指纹会话接口](https://android.googlesource.com/platform/hardware/interfaces/+/b91c536488ae4de7782b0b8f1f6a9bba642692ad/biometrics/fingerprint/aidl/android/hardware/biometrics/fingerprint/ISession.aidl)
要求录入使用真实 HAT；认证入口接收操作 ID，并在成功后返回 HAT，不要求先传入
新的录入 HAT。因此不能将 Keymaster 绑定或 IRQ 检测描述成已实现认证。当前仍使用
密码登录，不向 GNOME 注册无法完成录入和验证的指纹设备，也不读取或复制 Android 模板。

随后bde3普通重启发现，虽然modules-load文件已安装，旧modprobe黑名单仍使systemd把LPG记为deny-listed。已仅移除 `leds_qcom_lpg` 这一条，并让完整root stager与通用BSP使用同一份tracked modprobe配置。重新启动标准modules-load后插入LPG并出现三个接口，随后一次完整正常重启也已确认三个接口自动出现。当前记录在 `bde3-rgb-startup.txt` 与 `bde3-rgb-policy-fixed.txt`，不能用之前手动modprobe结果替代启动策略验证。
