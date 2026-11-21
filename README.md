# UniXNU-Fuxi-Arm64e-iOS17

**简体中文** · [English](README.en.md)

小米 13（fuxi，SM8550）运行 **iOS 17.7（21H16，iPhone15,2 内核缓存，arm64e）** 的 UniXNU 移植源码。

> 本仓库只含源码和脚本：**不含任何苹果二进制**（内核缓存、IPSW、dyld 共享缓存、从系统卷提取的文件、壁纸图片等），**不含构建产物**（内核、boot.img、payload、GPU addon），也**不含密钥和固件**。这些都需要自己Fork仓库后使用Actions产出。

| 项目 | 值 |
| --- | --- |
| 设备 | Xiaomi 13（`fuxi`，SM8550） |
| 目标系统 | iOS 17.7，build 21H16 |
| 内核 | xnu-10063，arm64e |
| 机型标识 | iPhone15,2 内核缓存，ADT 按 iPhone 14 Pro（D73AP）识别 |
| 当前进度 | 进入桌面；触摸、按键、声音、存储、串口、电池、迁移、壁纸可用 |

## 当前状态（2026-11-20）

| 功能 | 状态 | 说明 |
| --- | --- | --- |
| 触摸 | ✅ | FTM5 工作 |
| 按键 | ✅ | 电源键、音量键 |
| 声音 | ✅ | 播放、调节音量 |
| 存储 | ✅ | 重启保留用户数据 |
| 串口 | ✅ | 重启、截图、按键 |
| 电池 | ✅ | 电量、充电状态 |
| 迁移 | ✅ | 补全系统应用 |
| 壁纸 | ✅ | 锁屏壁纸、主屏幕壁纸 |
| 充电 | ✅ | 公版 PPS/PD 快充可用（单电荷泵）；小米私有快充未验证 |
| 键盘 | ✅ | 打字不再卡顿(非完美修复，在部分场景下可能还会卡顿) |
| Wi-Fi | ✅ | 可以上网，速率约 20 Mbit/s，连接速率较低 |
| 蓝牙 | ⚠️ | 上电及初始化完成 |
| 基带 | ⚠️ | 正在完成第一步调解器 |
| Safari | ⚠️ | 可以打开网页（内核放行 containermanagerd 查找，根因待查） |
| GPU | ⚠️ | 可工作，但效率较低 |
| 相机 | ❌ | 不工作 |
| 手电筒 | ❌ | 不工作 |

✅ 可工作 · ⚠️ 不完全工作 · ❌ 不工作

## 仓库结构

| 目录 | 内容 |
| --- | --- |
| `device/` | fuxi 设备仓库（基于 SadNutWall/unixnu-fuxi，`ios17-arm64e-checkpoint1` 分支）的当前源码：触控 FTM5、按键、背光、Wi-Fi、GPU 探针、UFS 等 |
| `ios17/` | iOS 17 / arm64e 覆盖层：板级配置、内核基线脚本、21H16 专用构建脚本、状态栏库。其中的符号链接指回 `device/` |
| `unixnu/patches/xnu-10063-arm64e/` | xnu-10063（arm64e）内核补丁，包括 iOS 17 启动策略、iPhone 壁纸挂载、探针等 |
| `unixnu/diffs/` | UniXNU 框架（SadNutWall/UniXNU，`fuxi/ios17-arm64e-checkpoint1`）及其子模块的未提交改动（`*.diff`，基准提交见 `*.HEAD.txt`） |
| `unixnu/files/` | 上述仓库里未跟踪的新增文件 |
| `unixnu/commits/` | 只存在于本地、未推到上游的提交（`git format-patch`）：UniXNU 的 arm64e xnu-10063 内核主体、XnuPrelink 的 21H16 fileset 支持（含函数映射表）、device 的 iOS 17 检查点 1 |
| `host/` | 主机侧脚本：`build-ios17-kernel.sh`（8 线程增量编译）、`pack-ios17-hidpath.py`（打包 boot.img）、`fuxi-auto-cycle.ps1`（串口回 fastboot → 临时启动 → 抓日志的无人值守循环） |
| `tools/` | 调试与生成工具：Data 骨架表生成（`mkskel.py`）、HEIC→PNG 壁纸转换（`heic2png.ps1`、`heicalpha.py`、`mkwallpaper.ps1`）、共享缓存符号化、线程探针分析、崩溃分析等 |
| `SKIPPED.txt` | 整理时排除的文件及原因 |

## 构建概要

1. 准备 UniXNU（SadNutWall/UniXNU），先 `git am` `unixnu/commits/UniXNU`（主仓库），再在各子模块里 `git am` `unixnu/commits/<子模块>`（`kits_XnuPrelink`、`drivers_Adreno740`、`drivers_AdrenoMetal`、`drivers_ArmCpu`、`drivers_AppleKeyStoreState`、`drivers_GopFramebuffer`；基准见各目录的 `HEAD.txt`，AdrenoMetal 先导入官方 51b7003 源码），再应用 `unixnu/diffs/*.diff`（基准见 `*.HEAD.txt`），拷入 `unixnu/files/`、`unixnu/patches/`。`unixnu/commits/device` 是 `device/` 的提交记录，`device/` 已是最终源码。
2. 把 `device/` 放在 UniXNU 旁边，`ios17/` 作为 `device-ios17/`。
3. 准备你自己的 21H16 IPSW 输入（内核缓存、系统卷 dmg、信任缓存）与 SDK，生成 `device/package/DataSkel21H16.h`（`tools/mkskel.py`）和壁纸覆盖文件（`tools/mkwallpaper.ps1`）。
4. `host/build-ios17-kernel.sh` 编译内核，`host/pack-ios17-hidpath.py`（`UFS_PROBE=0`）打包 boot.img，`fastboot boot` 临时启动（不刷写 userdata）。

脚本里的绝对路径是作者本机路径，使用前请按自己的环境修改。日用打包环境和已知坑见 [2026-11-17](#2026-11-17)。

## 进度日志

### 2026-11-20

- **Safari**：可以打开网页。WebContent 的 `_container_init` 向 launchd 查找 `com.apple.containermanagerd`，launchd 用 `sandbox_check_by_audit_token(mach-lookup)` 检查，内核沙盒判拒绝后回 159（沙盒限制），网页进程退出。判定结果写在调用方的 8 字节变量里（`__mac_syscall` 参数块第 0 个字指向它，返回值始终为 0）。`zzz-sandbox-check-probe.patch` 只对 WebKit 进程查找这个服务时把拒绝改为放行（`fuxi-wk-cm-allow=0` 关闭）。根因（只在清数据后第二次开机起拒绝）未查明。另：AMFI 跳过 WebKit 扩展的启动约束、`zzz-jit-separated-heap.patch` 允许 JIT 拷贝、Sandbox 用到的 3 个 VFS KPI 补进函数映射表。
- **键盘卡顿**：已修复。根因是跳过设置向导后没有 `AppleKeyboards`，以及没有 `DictationLanguagesEnabled`，UIKit 每次都重算全部输入法。`BootWatchdogPolicy.c` 开机写入托管键盘列表、全部为 false 的听写语言字典（约 150 个键）并关闭听写。
- **Wi-Fi**：能上网（关闭强制门户探测）；数据包缓冲改用 DMA 窗口的缓存映射（64 MiB slab，收发前后做缓存维护，`fuxi-wifi-cached=0` 关闭），工作线程修复丢唤醒竞态；速率仍只有约 20 Mbit/s，未解决。
- **快充**：移植上游 `ShengChargeAuth`（电量计认证、小米充电头认证）；公版 PPS 实测可用（单电荷泵约 2.5 A）。插充电器期间的充电记录写入 Data（`/private/var/db/fuxi-charge-log`），下次开机输出为 `fuxi charge replay (file):`。
- **GPU / Metal**：合并上游 Adreno740（1c9029b）与 AdrenoMetal（c9ebd2f）、官方 Mesa/Turnip 补丁系列。
- **8 核**：打包参数 `CPUMASK=0xff` 可开 8 核（默认仍 0xf9）。
- **时间 / 时区**：没有存储偏移时按编译日期起步，联网对时后保存本机偏移；时区不再依赖 `sheng-data-skel=1`，用户选的时区不覆盖。
- **直接送屏（`DPU=2`，实验，默认关）**：GopFramebuffer 移植上游 df4a905，显示驱动在 NativeUfs 建好窗口后再启用（开机时窗口尚未建好），回开机帧缓冲的行宽按 fuxi 取值。可连续直接切换数百帧，但运行数十秒后整机无日志复位，原因未查明。
- **基带**：MPSS 启动到 stage -8（SCM 调用失败）。蓝牙：BlueTool 替代版，bluetoothd 补丁点待移植。

### 2026-11-17

- **扬声器出声**：两颗 CS35L41 的 PLL 锁定，DSP 周期 4395 发 / 4395 回，无错误。
  - 根因：VirtualAudio 的扬声器路由检查（`0x24f2d4`，Playback / Broadcast / 录音路由处理器都会调用）只有在 `0x8f698` 为真时才要求 HAL 扬声器保护；`0x8f698` = 机型在列表内 **且** `os_feature_enabled("VirtualAudio", "siri_ducking_for_speaker")`。
  - 插件只回答 `'spac'` 无效（HAL 不会把这个选择器转给插件）。现在插件在 arm64e 上额外改写 VirtualAudio 对 `__os_feature_enabled_impl` 的导入，**只**对这一个开关返回 false，其余 102 处照常。日志信标：`ShengVA:fuxi: feature siri_ducking_for_speaker forced off`。
  - 没有扬声器保护 DSP，请用中低音量。
- **日用打包**：`UFS_PROBE=0 PERSIST=2 DATA_SEED=<scratch-data-48g.img> GPU_ADDON=<含新插件的 addon> BTD_PATCHED=1 BTD=1`。
  - `PERSIST=2` 必须配 48G 种子；用默认 1G 种子会写坏 userdata 末尾已有的 48G 存储（APFS 校验和错误，挂载 Data 时 panic）。
  - `PERSIST` 会为启动参数腾位置去掉 `unixnu-iokit-probe=1`，此时看不到插件的 `ShengVA:` / `ShengHAL:` 信标；诊断时不开 `PERSIST`。
  - `pack-ios17-hidpath.py` 默认的 `GPU_ADDON` 仍是 11-11 的旧包；带补丁 bluetoothd 的 addon 要同时给 `BTD_PATCHED=1`，否则 bluetoothd 因签名被反复杀掉。
- **用户态编译**（HAL 插件、GPU 插件、Turnip）：`CROSS_EXTRA_FLAGS` 不带内核参数、`CROSS_CLANG` 指向 UniXNU 的 clang、`-B` 指向 CommandLineTools 以使用 ld64，并使用真正的 ad hoc `codesign`（工具链里的 `codesign` 是空实现，拿不到 CDHash）。
- **键盘卡顿（未修复）**：
  - 写入托管偏好关闭听写（`com.apple.assistant.support` 的 `Dictation Enabled=false`，`BootWatchdogPolicy.c`）、禁用 assistantd / siriknowledged 均无效，听写不是原因。
  - 热点采样：Preferences 在键盘弹出时持续 100%，用户态约 66% 在 `AutoreleasePoolPage::releaseUntil`；搜索框与普通文本框分布相同，是键盘在场时一直运行的循环。下一步需要采样器记录完整调用栈。
- **GPU**：键盘弹出时出现过一次 GPU 页错误，恢复后 backboardd 被插件按设计重启（`scanout copy failed 30 times`）。按官方默认值试 `TU_DEBUG=gmem,nocb` 会在首次迁移中整机卡死，已回退为空值。
- **蓝牙 / 电池 / RTC**：QcaBluetooth tty、补丁版 bluetoothd 与启动约束、电池服务、RTC 时间（见 `unixnu/commits/` 与 `device/package/`）。

### 2026-11-15

- **蓝牙**：修复蓝牙上电 ✓（暂未接入用户态）

### 2026-11-14

- **电池**：修复电量针 ✓ 修复电池状态显示 ✓
- **迁移**：修复开机迁移 ✓ 补全系统应用
- **壁纸**：修复壁纸渲染显示 ✓ 桌面 / 锁屏壁纸 ✓
- **存储**：修复数据保留 ✓ 扩容 Data 种子为 48G
- **时间**：添加 RTC 时间校准功能

### 2026-11-13

- **显示**：启用逻辑分辨率 1125×2436（375×812 pt，UIKit 认识的屏幕类型），由 GPU（AdrenoMetal `scanout_scale` AIR 内核，`unixnu/diffs/drivers_AdrenoMetal.diff`）缩放到 1080×2400 面板；控制中心布局恢复正常。AdrenoMetal 必须基于 `official-51b7003-import`（5d56587）构建。
- **键盘**：可以打字。mobileassetd（普通 arm64 进程）因内核未关闭用户态 PAC 而在 dyld 中 FPAC 崩溃，kbd 同步等待它导致打开键盘的 App 卡死；`native-user-jop-disable.patch` 在返回 EL0 时为 `disable_user_jop` 线程清除 SCTLR_EL1 EnIA/IB/DA/DB、进入 EL1 时恢复。打字仍卡顿（UIKeyboardInputModeController 反复重算听写语言，待修）。
- **音频**：ADSP、CS35L41 功放、HAL 插件 `device/hal/ShengAudioPlugIn.c` 均已工作。
  - ADT 补上真机 D73AP 设备树的 `/product/product-id` 和 `/product/audio/acoustic-id = 8006`，MobileGestalt 识别为 iPhone 14 Pro，VirtualAudio 加载 AID8006 调校与路由数据库。
  - 插件以 VirtualAudio 的 `Codec` 设备身份出现（8 声道输出，输出元素 4 的静音/音量控件），已建出 `Built-In Speaker` 端口。
  - 插件在 arm64e 上改写 VirtualAudio 的导入（MobileGestalt、fopen、IORegistry、`__cxa_throw`、`_os_log_impl`），错误日志以 `ShengVA:` 信标出现在内核日志里。
  - AID8006 的 `graph_configurations.plist` 去掉 `speaker_*` 项后由构建脚本从你自己的系统卷生成（不入库），避免闭环扬声器保护因缺出厂校准数据而崩溃。
  - **仍无声**：播放路由要求 HAL 级扬声器保护（`spac`），尚未满足（已在 11-17 解决）。
- **内核探针**：1 kHz 热点采样器增加 LR 与整栈包含式统计（`zz-hot-sampler.patch`），线程探针、IOKit 探针常驻关键词。

### 2026-11-12

- **Wi-Fi**：修复扫描和连接（2.4G）✓ 上网 ❌
- **GPU**：优化 GPU 效率，增强流畅度
- **状态栏**：去除屏幕顶部的红条

### 2026-11-11

已经可以进入 iOS 17 桌面：

- **GPU / 显示**：Adreno 740 执行命令，IOSurface 与交换链正常，b3a8（XR10）帧由 CPU 转换后上屏。
- **触控**：FTM5 正常；21H16 上内核 `IOHIDEventDriver` 需要 IOHIDDevice 带 `HIDDefaultBehavior` 才会匹配（配合 `dk=0` 跳过 DriverKit）；补上 `OSArray::withObjects` 映射后触控不再卡死。
- **按键**：`FuxiButtons`（PMK8550 PON/RESIN + PM8550 GPIO6）接入 NativeHidInput report 2，电源可用，音量键可用。
- **GPU 插件**：Turnip（`libvulkan_freedreno.dylib`）改用 chained fixups 链接，修复 mediaserverd / iconservicesagent 在子线程加载 Metal 时 dyld 栈溢出（设置打开慢的根因）。
- **壁纸**：默认壁纸 7565 的 HEIC 图层需转成 PNG（Windows WIC，保留 alpha），由 `ios17-wallpaper-iphone.patch` 挂载；挂载成功，但 PosterBoard 的渲染扩展尚未出现，**壁纸仍不显示**（11-14 已修复）。

## 已知问题

- ADT 中 iPhone15,2 的 `target-type` 仍为 `D72`，真机为 `D73`。
