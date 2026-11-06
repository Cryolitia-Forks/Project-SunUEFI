# 原厂 Piano 刷新率切换

本记录对照 MiCode 显示驱动提交
`aa06fd1757c28dce96fbe4e04d4530ec21b52aac` 和同机 Android 最终 DT，
不将通用驱动中存在的分支直接当作本机运行结果。

## 最终 DT 的配置

原厂包含 `p81_35_02_0b` 和 `p81_42_02_0a` 两个 P81 面板节点。
本机 Android 的启动参数实际选择
`qcom,mdss_dsi_p81_42_02_0a_dualdsi_dsc_vid`。这些节点启用 `mi,video-mode-multi-timing` 和
`qcom,mdss-dsi-pan-enable-dynamic-fps`，更新方式为
`dfps_immediate_porch_mode_vfp`。

三个基础时序组为 120、144、90 Hz。120 Hz 组允许 120 和 60 Hz；
90 Hz 组还允许 50、48、30 Hz。低刷新率通过增加垂直前沿间隔
生成，不是所有刷新率各自重新初始化一次面板。
三个组的 DSI bit clock 均配置为 1,197,500,000 Hz。

CSOT 时序切换命令选择 page 0x10、解锁，然后按 B2、B3 顺序写入：

| 时序组 | B2 | B3 |
| --- | --- | --- |
| 120 Hz（含 60 Hz） | 0x91 | 0x40 |
| 144 Hz | 0x00 | 0x00 |
| 90 Hz | 0x00 | 0x80 |

`qcom,mdss-dsi-timing-switch-video-command-state` 为 `dsi_hs_mode`。
两家面板的 timing-switch-video-command 都按 B2、B3 顺序发送。
BOE 在 on-command 初始化段按 B3、B2 顺序发送，和时序切换命令不同，
不能混用这两个序列。

## 代码路径

- [dsi_display.c](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_display.c#L5433)
  的 `dsi_display_dfps_update()` 首先异步更新时钟主控制器的时序，
  随后更新其他控制器。DFPS/VRR 路径设置 timing double buffer，
  更新 host config；不能等同于断开所有显示资源再重新启动。
- [dsi_panel.c](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/dsi/dsi_panel.c#L6594)
  的 `dsi_panel_post_enable()` 对 P81 发送
  `DSI_CMD_SET_TIMING_VIDEO_SWITCH`。
- [sde_encoder.c](https://github.com/MiCode/vendor_opensource_display-drivers/blob/aa06fd1757c28dce96fbe4e04d4530ec21b52aac/msm/sde/sde_encoder.c#L7075)
  的 VRR 提交流程等待视频进入 active 区域，提交新的显示数据，
  随后对 P81 发送该命令。退出 60 Hz 到非 120 Hz 模式还有独立的
  `DSI_CMD_SET_DISP_PEN_CLEAR` 和等待下一次垂直同步步骤。

## Android 报告的能力

同机原厂 `OS3.0.304.0.WPYMIXM` 的 DisplayManager 报告
`hasArrSupport=false`。SurfaceFlinger 列出的 30、48、50、60、90、120、144 Hz
七个模式均为 `vrrConfig=N/A`。高通 HWC 报告 video mode、`DynFPS:true`、
`QSync:false`、`Qsync mode:0`；当前为 120 Hz，VFP 68、vtotal 2310。
实际运行设备树中的两个 P81 节点均声明同一份七档 DFPS 列表，没有 QSync
最低刷新率或连续 VRR 范围。

这些信息确认原厂通过固定档位和 VFP 更新实施刷新率调节，不能把设置中的
可变刷新率直接解读为 30–144 Hz 任意频率。系统级的 `vrr_config=true` 或
`smartpen_vrr_fps` 属性也不能代替显示器实际报告的能力。
[Android ARR](https://source.android.com/docs/core/graphics/arr) 本身使用离散
VSync 步进，`vrrConfig=null` 则表示该模式没有启用标准 ARR。

Linux 的固定档位使用普通 DRM/KMS 模式接口，不需要 `vrr_capable`。
按交互、空闲和视频内容自动选档属于合成器策略；内核需要保证各档切换时
正确同步双 DSI、面板命令及帧缓冲生命周期。

## Linux 对照

当前 Linux 走完整 modeset，关闭并重新准备面板和显示链路。
将 CSOT B2/B3 顺序与原厂对齐、在 panel enable 中通过 HS 补发
时序命令后，120 Hz 启动正常，命令返回成功；60 Hz 切换仍灰屏。
因此只补发命令未复现原厂的完整动态切换流程。
该实验没有加入产品补丁序列，设备恢复正式模块。

下一步需要同机 Android 运行采集确定选用 DFPS/VRR 分支，
并对照主从控制器的提交、时钟和时序寄存器。Linux 应通过 DRM
标准 modeset/VRR 接口实现；不能仅增加刷新率或 VRR 按钮。

## 2026-10-13 同机运行采集

在独立 ftrace instance 中开启 `sde:sde_evtlog`，结束后关闭并移除，
没有更改 Android 原有的 tracing instance。
事件中出现 `dsi_display_dfps_update` 进入和退出，
`dsi_display_set_mode_sub` 的模式标志为 `0x10`。
这证明本机使用动态时序更新分支，不能把源码中的分支当作推测。

两轮采集请求 60 Hz，但读取 DRM 当前状态是 50 Hz，
不是 60 Hz。第一轮还误用了横屏尺寸；第二轮按显示管理器登记的
2136×3200 设置偏好后，状态仍是 50 Hz。因此尚未完成准确的
120→60→120 对照，不能按命令请求值报告实测模式。

50 Hz 时序为 807067 kHz，水平总计 3882，垂直总计 4158。
采集的 DSI byte clock 为 149687500 Hz，pixel clock 为
159666666 Hz。原厂静止画面的刷新率策略需要与硬件切换区分开。
后续采集应观察模式并覆盖稳定 60 Hz，避免仅依赖设置项。

临时 peak/min 刷新率、用户显示模式偏好及自动灭屏时间已恢复，
随后通过固件代号匹配的原生请求工具回到 Linux。

## 本地来源校验

源码缓存的 SHA-256：

| 文件 | SHA-256 |
| --- | --- |
| dsi_display.c | `100d0074dd68cdbdb205d8296fe97c2ff532d8d6e24b3598c5592e6e9b38b1ef` |
| dsi_panel.c | `b3a2c00531c42465ef377b2c2d19797cd681862e84b60461c06485cf1929c090` |
| sde_encoder.c | `97cacaf4a41455990317a4ce58e521b1c76f0b2edc1644d574a482620e80c09b` |

最终 DT 的本地文本缓存 SHA-256：
`e243e388d76d329dad88ef92d1484de9acc9c6204359fc1d9a3524a4c53362f9`。

## 连续 VRR 的驱动与面板边界

`feat/drm-avr` 分支实现双 DSI 的 DPU AVR 提交路径，使用 DRM 的
`VRR_ENABLED` 属性控制，通过共享 CTL 提交后同时触发两个接口。
它没有为 P81 面板公布 `vrr_capable` 或连续刷新范围，因此正常镜像
继续使用固定刷新率。[固定档位切换](display-resume.md)与连续 VRR
是两条不同的能力边界。

AVR 退出先提交固定周期，等待 CTL flush 清零和两侧 INTF 的硬件帧
计数都推进，再关闭两侧 DSI 接收器的 AVR 位并读回。原子提交、CTL
恢复、卸载和关机都经过同一个退出流程。退出超时将禁用 AVR 重入，
并阻塞当前提交或销毁，保留电源、时钟、plane 映射和原子状态；
超时本身不表示 DMA 已停止。硬件确认退出后才继续普通提交或关闭
链路，完整 modeset 后才允许重新尝试 AVR。
VRR 开启或切换期间，光标提交也使用同步 flush 路径，先确认硬件
消费更新再释放旧 framebuffer 的映射，避免延迟光标更新提前清理
旧画面。

面板接入仍需分别确认 CSOT 与 BOE 的连续周期限制和 DDIC 设置。
原厂 DFPS 的 120/60、90/50/48/30、144 Hz 分属不同的时序和命令组；
这些离散档位不能直接转成 30～144 Hz 的连续范围。同一时序组拥有
多个固定档位，也不能证明其间任意前沿长度都被面板接受。
[CSOT 配置](https://github.com/MiCode/vendor_qcom_opensource_display-devicetree/blob/cc4e65ad4bd54e26b60f5a8ee76bbc3cca6821b9/display/dsi-panel-p81-35-02-0b-dualdsi-dsc-lcd-video.dtsi)、
[BOE 配置](https://github.com/MiCode/vendor_qcom_opensource_display-devicetree/blob/cc4e65ad4bd54e26b60f5a8ee76bbc3cca6821b9/display/dsi-panel-p81-42-02-0a-dualdsi-dsc-lcd-video.dtsi)
和它们引用的[公共配置](https://github.com/MiCode/vendor_qcom_opensource_display-devicetree/blob/cc4e65ad4bd54e26b60f5a8ee76bbc3cca6821b9/display/dsi-panel-p81-p82-dualdsi-dsc-lcd-video-common.dtsi)
都没有声明 QSync 最低频率或连续 VRR 范围。

Mutter 50 已原生处理 connector 的 `vrr_capable`、CRTC 的
`VRR_ENABLED` 和全屏窗口的可变帧时钟，不需要恢复旧版 GNOME 的
VRR 实验开关。面板开放能力之前，不应通过桌面补丁绕过驱动限制。
开放后的验收应记录两侧真实帧周期随应用提交间隔变化，检查双屏
接缝、underrun、固定与可变模式切换，以及熄屏、恢复和退出失败；
仅看到属性置位不算连续 VRR 正常工作。
