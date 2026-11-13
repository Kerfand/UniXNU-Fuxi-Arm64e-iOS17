# UniXNU-Fuxi-Arm64e-iOS17

小米 13（fuxi，SM8550）运行 **iOS 17.7（21H16，iPhone15,2 内核缓存，arm64e）** 的 UniXNU 移植源码。

本仓库只含源码和脚本：**不含任何苹果二进制**（内核缓存、IPSW、dyld 共享缓存、从系统卷提取的文件、壁纸图片等），**不含构建产物**（内核、boot.img、payload、GPU addon），也**不含密钥和固件**。这些都需要按下文从你自己的输入生成。

## 目录

| 目录 | 内容 |
|---|---|
| `device/` | fuxi 设备仓库（基于 SadNutWall/unixnu-fuxi，`ios17-arm64e-checkpoint1` 分支）的当前源码：触控 FTM5、按键、背光、Wi-Fi、GPU 探针、UFS 等 |
| `ios17/` | iOS 17 / arm64e 覆盖层：板级配置、内核基线脚本、21H16 专用构建脚本、状态栏库。其中的符号链接指回 `device/` |
| `unixnu/patches/xnu-10063-arm64e/` | xnu-10063（arm64e）内核补丁，包括 iOS 17 启动策略、iPhone 壁纸挂载、探针等 |
| `unixnu/diffs/` | UniXNU 框架（SadNutWall/UniXNU，`fuxi/ios17-arm64e-checkpoint1`）及其子模块的未提交改动（`*.diff`，基准提交见 `*.HEAD.txt`） |
| `unixnu/files/` | 上述仓库里未跟踪的新增文件 |
| `unixnu/commits/` | 只存在于本地、未推到上游的提交（`git format-patch`）：UniXNU 的 arm64e xnu-10063 内核主体、XnuPrelink 的 21H16 fileset 支持（含函数映射表）、device 的 iOS 17 检查点 1 |
| `host/` | 主机侧脚本：`build-ios17-kernel.sh`（8 线程增量编译）、`pack-ios17-hidpath.py`（打包 boot.img）、`fuxi-auto-cycle.ps1`（串口回 fastboot → 临时启动 → 抓日志的无人值守循环） |
| `tools/` | 调试与生成工具：Data 骨架表生成（`mkskel.py`）、HEIC→PNG 壁纸转换（`heic2png.ps1`、`heicalpha.py`、`mkwallpaper.ps1`）、共享缓存符号化、线程探针分析、崩溃分析等 |
| `SKIPPED.txt` | 整理时排除的文件及原因 |

## 当前进度（2026-11-11）

已经可以进入 iOS 17 桌面：

- **GPU / 显示**：Adreno 740 执行命令，IOSurface 与交换链正常，b3a8（XR10）帧由 CPU 转换后上屏。
- **触控**：FTM5 正常；21H16 上内核 `IOHIDEventDriver` 需要 IOHIDDevice 带 `HIDDefaultBehavior` 才会匹配（配合 `dk=0` 跳过 DriverKit）；补上 `OSArray::withObjects` 映射后触控不再卡死。
- **按键**：`FuxiButtons`（PMK8550 PON/RESIN + PM8550 GPIO6）接入 NativeHidInput report 2，电源可用，音量键不可用。
- **激活**：Data 种子缺 `/private/var/Keychains`，securityd 无法打开钥匙串，mobileactivationd 迁移失败。内核在 Data 挂载后按 21H16 系统卷 `/private/var` 模板补齐目录（`sheng-data-skel=1`，表由 `tools/mkskel.py` 从你自己的 IPSW 生成），mobileactivationd 原生激活，已跳过激活。
- **亮度**：`fuxi-backlight=dsi`（DCS 0x51）+ `sheng-backlight-control=1`；arm64e 上改为真正的 `getMetaClass()` 重写，不再改虚表。
- **GPU 插件**：Turnip（`libvulkan_freedreno.dylib`）改用 chained fixups 链接，修复 mediaserverd / iconservicesagent 在子线程加载 Metal 时 dyld 栈溢出（设置打开慢的根因）。
- **壁纸**：默认壁纸 7565 的 HEIC 图层需转成 PNG（Windows WIC，保留 alpha），由 `ios17-wallpaper-iphone.patch` 挂载；挂载成功，但 PosterBoard 的渲染扩展尚未出现，**壁纸仍不显示**。

## 进度更新（2026-11-13）

- **显示**：启用逻辑分辨率 1125×2436（375×812 pt，UIKit 认识的屏幕类型），由 GPU（AdrenoMetal `scanout_scale` AIR 内核，`unixnu/diffs/drivers_AdrenoMetal.diff`）缩放到 1080×2400 面板；控制中心布局恢复正常。AdrenoMetal 必须基于 `official-51b7003-import`（5d56587）构建。
- **键盘**：可以打字。mobileassetd（普通 arm64 进程）因内核未关闭用户态 PAC 而在 dyld 中 FPAC 崩溃，kbd 同步等待它导致打开键盘的 App 卡死；`native-user-jop-disable.patch` 在返回 EL0 时为 `disable_user_jop` 线程清除 SCTLR_EL1 EnIA/IB/DA/DB、进入 EL1 时恢复。打字仍卡顿（UIKeyboardInputModeController 反复重算听写语言，待修）。
- **音频**：ADSP、CS35L41 功放、HAL 插件 `device/hal/ShengAudioPlugIn.c` 均已工作。
  - ADT 补上真机 D73AP 设备树的 `/product/product-id` 和 `/product/audio/acoustic-id = 8006`，MobileGestalt 识别为 iPhone 14 Pro，VirtualAudio 加载 AID8006 调校与路由数据库。
  - 插件以 VirtualAudio 的 `Codec` 设备身份出现（8 声道输出，输出元素 4 的静音/音量控件），已建出 `Built-In Speaker` 端口。
  - 插件在 arm64e 上改写 VirtualAudio 的导入（MobileGestalt、fopen、IORegistry、`__cxa_throw`、`_os_log_impl`），错误日志以 `ShengVA:` 信标出现在内核日志里。
  - AID8006 的 `graph_configurations.plist` 去掉 `speaker_*` 项后由构建脚本从你自己的系统卷生成（不入库），避免闭环扬声器保护因缺出厂校准数据而崩溃。
  - **仍无声**：播放路由要求 HAL 级扬声器保护（`spac`），尚未满足。
- **内核探针**：1 kHz 热点采样器增加 LR 与整栈包含式统计（`zz-hot-sampler.patch`），线程探针、IOKit 探针常驻关键词。

## 已知问题

- 扬声器无声（见上），打字卡顿。
- ADT 中 iPhone15,2 的 `target-type` 仍为 `D72`，真机为 `D73`。

## 构建概要

1. 准备 UniXNU（SadNutWall/UniXNU），先 `git am` `unixnu/commits/UniXNU`（主仓库）和 `unixnu/commits/kits_XnuPrelink`（子模块），再应用 `unixnu/diffs/*.diff`（基准见 `*.HEAD.txt`），拷入 `unixnu/files/`、`unixnu/patches/`。
2. 把 `device/` 放在 UniXNU 旁边，`ios17/` 作为 `device-ios17/`。
3. 准备你自己的 21H16 IPSW 输入（内核缓存、系统卷 dmg、信任缓存）与 SDK，生成 `device/package/DataSkel21H16.h`（`tools/mkskel.py`）和壁纸覆盖文件（`tools/mkwallpaper.ps1`）。
4. `host/build-ios17-kernel.sh` 编译内核，`host/pack-ios17-hidpath.py`（`UFS_PROBE=0`）打包 boot.img，`fastboot boot` 临时启动（不刷写 userdata）。

脚本里的绝对路径是作者本机路径，使用前请按自己的环境修改。
