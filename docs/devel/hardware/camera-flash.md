# Piano 摄像头闪光灯

后摄的两路灯由标准 Qualcomm SPMI flash 驱动管理，通过 LED class 和 V4L2 flash subdevice 提供接口。相机服务通过传感器的 `flash-leds` 关联找到两路灯，在既有帧循环中控制一次拍照，不需要另一个闪光守护进程。

拍照命令、支持的模式和曝光覆盖方式见[相机格式与控件](camera-controls.md#闪光灯)。普通视频应用不会自动发出同步拍照请求。

## 接线与限值

原厂最终 Android 设备树将控制器放在 PM8550 的 SPMI SID 1、基址 `0xee00`；实机身份寄存器为 `type=0x18`、`subtype=0x07`。原厂 `qcom,qti-pm8350c-flash-led` 是 IP 名称，不能据此推测 SID。原厂 `qcom,id` 0/1 对应标准的一基 `led-sources=<1>/<2>`，两路 LED 分别控制。

每路 flash 上限为 400 mA、300 ms。原厂 torch 允许短时 400 mA，5000 ms 后降为 100 mA；标准驱动没有这条时间曲线，所以设备树将连续 torch 上限设为每路 100 mA。原厂限值来自 [MiCode 驱动](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/blob/45fb9bd6ae5ba2942fc1d53e4b6b46ef76992f71/drivers/leds/leds-qti-flash.c#L1710)。

[piano-camera-flash.dtso](../../../linux/dts/piano-camera-flash.dtso) 禁用旧 vendor flash 和 trigger consumer，创建标准 `led-controller@ee00`，并把两路 LED 连接到 S5KJN1。传感器和 flash wrapper 通过现有 V4L2 async notifier 关联，无需手工写 PMIC 寄存器。相关内核配置在 [piano-flash.config](../../../linux/configs/piano-flash.config)。

## 电流与控制接口

LED class 提供 torch brightness、flash brightness、timeout、strobe 和 fault；相机程序使用相应 V4L2 控件。请求成功表示驱动接受了控制，发光和曝光覆盖需要分别检查。

驱动以每通道 5 mA 为档位。电流转 LED 亮度编码时向上取整，再由 setter 按硬件档位取整，避免两次向下取整将 25 mA 降为 20 mA。反向转换按同一档位返回电流，零亮度仍表示关闭。当前补丁顺序以 [Linux series.json](../../../patches/linux/7.3/series.json) 为准。

## 同步拍照

服务使用软件 strobe 和硬件 timeout，冻结当前 3A 控制后等待两个新帧，再点亮两路灯。软件控制调用前后的单调时钟时间限定亮灯窗口；物理 SOF/EOF 与传感器积分模型限定每帧曝光区间。只交付曝光区间完整落在亮灯窗口内的图像，取消、超时或错误均停灯并恢复控制。

帧编号、时间戳精度和错误帧语义见[帧时间戳与错误](camera-controls.md#帧时间戳与错误)。这条路径使用覆盖滚动曝光的长脉冲，不提供预闪测光或短脉冲冻结运动；全幅 RDI 模式与前摄不接受闪光请求。
