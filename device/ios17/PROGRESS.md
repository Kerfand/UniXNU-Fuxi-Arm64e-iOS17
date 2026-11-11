# fuxi（小米 13）iOS 17 arm64e 移植进度 — 2026-11-05 21:00

## 1. 目标与范围

- 设备：小米 13（fuxi，SM8550）。系统：iPhone15,2 iOS 17.7（21H16）的 kernelcache。
- 内核：xnu-10063.141.1，按 arm64e 编译（BCM2837 机型配置，4K 页）。用预链接把我们的内核和苹果原 kernelcache 里保留的 kext 拼成一个 MH_FILESET。
- **检查点 1（进行中）**：只做内核的 boot.img，用 `fastboot boot` 临时启动，不刷 userdata。目标是走到等待根设备那一步。
- 和 iPadOS 21H461 路线完全隔离：iOS 17 用独立的设备覆盖层 `device-ios17/` 和输出目录 `UniXNU/out-ios17/`，iPadOS 的构建不受影响。

## 2. 当前状态一览

| 项目 | 当前值 |
|---|---|
| 最新 boot.img | `F:\UniXNU-FUXI-work\outputs\sadnut-fuxi-ios17-arm64e-kernel-20261105\boot.img` |
| boot.img sha256 | `a65c8a895aafb96e27ba9a9a70c8eef76b3e90eb70af58694fb7a3ef8179e226` |
| kernel sha256 | `0e7f16a639193646a611312631aa52ce2257deafe19e71ef865cd8a55044ed06` |
| 菜单项 | `FUXI IOS17 ARM64E KERNEL TEST 20261105` |
| 启动参数 | `serial=3 debug=0x14e -v kt_fixed_zones=120 qcom-usb-debug=1 sheng-log=1 armcpu-spi-ipi=1 sheng-smp=1 -no_slto_panic ml-timeout-lock-panic-scale=40 unixnu-unmapped=1` |
| 参考内核 | RELEASE + **LTO**，sha256 `8409b883…` |
| 已配对函数 | 13348（非 LTO 参考时只有 7414） |
| 被打桩的目标 / 调用点 | 471 / 2178（最初是 1355 / 12442） |
| 缺失数据目标 / 调用点 | 47 / 49（最初是 294 / 513） |
| 人工确认的身份（fixture） | 29 个函数、34 个数据项 |
| 保留的 kext（闭包） | 26 个，含 corecrypto、AppleImage4、AMFI、Sandbox、apfs、IOStorageFamily、IOHIDFamily、ApplePMGR、AppleARMPlatform 等 |
| 启动阶段 | 在 `PE_init_iokit` 里启动 kext，**还没进 `bsd_init`** |

## 3. 编译与上机指令

所有构建都在 WSL（`Ubuntu-26.04`）里跑，工作目录是 `/home/xiaomi/sadnut-fuxi-local-20261103`。默认 8 线程、增量编译。

### 3.1 一次性准备（已完成，正常不用再跑）

```bash
# Linux 上带 LTO 支持的 ld64：libLTO 取自与 clang 相同的 llvm 提交（58c46bae…），
# 再编 cctools-port 1030.6.3-ld64-956.6，装到 /home/xiaomi/lto-toolchain（源码和构建目录保留，可增量）
bash build-lto-toolchain.sh

# 把 DarwinBoot v2（支持 arm64e 和 MH_FILESET）替换进 UEFI 固件卷，生成 boot.img 模板
bash build-darwinboot-fv.sh
python3 make-t32-darwinboot-v2-template.py
```

### 3.2 每次迭代（常用）

```bash
# 一条命令完成：内核增量编译 → 预链接 → 打包 boot.img。默认复用已有的 LTO 参考内核
bash build-ios17-probe-chain.sh            # 日志：/tmp/ios17-build.log，最后一行是 CHAIN_DONE
```

从 Windows 后台启动（不受终端超时影响）：

```powershell
wsl -d Ubuntu-26.04 -- bash -c "cd /home/xiaomi/sadnut-fuxi-local-20261103 && (setsid nohup bash ./build-ios17-probe-chain.sh >/dev/null 2>&1 < /dev/null &)"
```

需要单独执行某一步时：

```bash
bash build-ios17-kernel.sh                  # arm64e 内核 + 预链接（JOBS=8）
python3 pack-ios17-kernel-test.py           # 生成 ADT、写入 payload、打包 boot.img
REBUILD_REFERENCE=1 bash build-ios17-probe-chain.sh   # 只在需要重做匹配时用：会完整重编 LTO 参考内核
bash build-ios17-reference.sh --lto         # 单独编 LTO 参考内核（链接时强制使用支持 LTO 的 ld）
```

查看实时日志：

```bash
tail -F /tmp/ios17-build.log
```

### 3.3 上机

```bash
fastboot boot "F:\UniXNU-FUXI-work\outputs\sadnut-fuxi-ios17-arm64e-kernel-20261105\boot.img"
```

在菜单里选 `FUXI IOS17 ARM64E KERNEL TEST 20261105`。日志由 USB 抓取工具保存到 `E:\桌面\FUXI_IOS\builds\fuxi-usb-log-v2\logs\`。

### 3.4 探针

启动参数 `unixnu-unmapped=1` 打开后，kext 每次调用一个没配对的内核函数时，日志里会出现一行
`fileset: unmapped call #N from <调用点> (x0..x3, x16); returning 0`，然后**返回 0 继续运行**，到第 64 次才 panic。
返回 0 只是为了收集证据，本身不是修复。后续出现的 NULL 解引用、PAC 失败，常常是这个返回 0 造成的连带结果。

## 4. 已通过的阶段（按时间顺序）

| # | 阶段 | 原来的问题 | 根因 | 修复 |
|---|---|---|---|---|
| 1 | 编出 arm64e 内核 | 编译和链接报错 | PAC 密钥配置、上游 clang 的 `__ptrauth` 写法、arm64e 下不能有未定义的弱符号等 | `native-pac-keys`、`upstream-clang-auth-null`、`arm64e-threaded-starts` 补丁和 stub 库 |
| 2 | 预链接成 fileset | 段放不进、fixups 对不上 | xnu-10063 要求内核代码排在最前；需要 chained fixups 和 split-seg 信息 | `UNIXNU_FILESET_KERNEL_FIRST`、`XNU_KERNEL_CHAINED_FIXUPS=1`，缩小 QUD 缓冲区，最小闭包 |
| 3 | DarwinBoot 能加载 | `Unsupported` | 固件里的旧 DarwinBoot 不认识 arm64e/fileset，载荷里的驱动又被安全启动拒绝 | 把 DarwinBoot v2 换进固件卷 FV |
| 4 | 通过 DarwinBoot 校验 | `Load Error` | KC 头的 cpusubtype（0x2）和内核的（0x80000002）不一致 | 预链接时把 KC 头的 subtype 改成和内核一致 |
| 5 | 通过 ADT 检查 | `Compromised Data` | payload 里残留旧的 ramdisk 和 gpuaddon，ADT 里没有对应占位 | 打包时去掉 initrd、gpuaddon、scratch-data |
| 6 | 内核起来，过 VM 初始化 | PAC/数据异常（读 kext 头） | 4K 页内核拆分 2MB 块映射时，把块里其他部分清空了 | `split-block-keeps-mapping.patch` |
| 7 | corecrypto 自检 | 调到未映射函数 | 非 LTO 参考内核导致匹配器认不出常用内核函数 | fixture 加入 `PE_parse_boot_argn`、`mach_absolute_time`、`clock_timebase_info`、`panic_active`、`IOMallocData`；加入探针 |
| 8 | corecrypto 注册 / PRNG | PAC 失败（PRNG 上下文为空） | `register_crypto_functions`、`register_and_init_prng` 没配对 | 加进 fixture |
| 9 | corecrypto 加锁 | `lck_mtx_lock` 收到空锁 | `lck_mtx_alloc_init`、`lck_mtx_try_lock` 没配对 | 加进 fixture |
| 10 | IOKit 启动、NVRAM | `postModLoad` 崩溃 | `IOCommand`、`OSObject` 等 `gMetaClass` 数据缺失，父类链读到乱数据 | 加入 13 个元类数据身份 |
| 11 | AppleImage4 加载、注册策略 | `/chosen` 没找到、kalloc 返回 NULL | `fromPath`、`code_signing_configuration`、`strprefix`、`kalloc_type_impl_external` 等没配对 | 加进 fixture，并补上一批常用锁函数 |
| 12 | AppleImage4 读环境变量 | `OSDynamicCast` 失败 | `safeMetaCast` 和 19 个 `Class::metaClass` 常量缺失 | 加进 fixture（依据：同位引用和元类构造） |
| 13 | IOStorageFamily | 写 NULL | `IOMallocTypeImpl`、`lck_mtx_gate_close` 没配对 | 加进 fixture |
| 14 | **改用 LTO 参考内核** | 每轮只能清掉 1～3 个问题 | Linux 上的 ld64 不支持 LTO | 编出 LTO 工具链；配对数 7414 → 13348 |
| 15 | **AMFI 加载并注册策略** | PAC 失败（IA 密钥） | UniXNU 的早期安全初始化用整数地址直接调用函数，arm64e 下没有签名 | `early-security-init-sign.patch`（编译结果是一条普通的 `blr`，已反汇编确认） |
| 16 | Sandbox 开始初始化 | 读到错误的 `vm_map` | 数据 `kernel_map` 缺失 | 加入 `kernel_map`、`NDR_record`（依据：同位引用，19 票和 8 票） |
| 17 | Sandbox 继续初始化 | 一轮内触发 64 次未映射调用 | `strcasecmp`、`OSDictionary::withCapacity` 等 libkern 容器函数没配对 | 一次补了 7 个（依据：PAC 判别符签名逐个匹配） |

## 5. 当前卡点

1. **在等新镜像（`a65c8a89…`）上机后的日志。** 上一次日志停在 Sandbox 的 `mpo_policy_init`（探针在 64 次时 panic）。这次已经补上 `strcasecmp`、`OSDictionary/OSArray/OSSet::withCapacity`、`OSNumber::withNumber`、`OSMetaClass` 的析构和实例计数。
2. **离 `bsd_init` 还有：** Sandbox 剩下的初始化，然后是 apfs、IOHIDFamily、ApplePMGR、AppleARMPlatform 等 kext 的启动。`PE_init_iokit` 返回后才会进 `bsd_init`，之后挂根设备，最后停在等根设备（第一个检查点的终点）。

## 6. 已知未解决的问题和风险

- **AppleImage4 读 nonce magazine 时的 8 次未映射调用**（调用点 `0x…8da3a14`）。这些是 AppleImage4 内部的 thunk 经函数指针尾调用到内核函数，所以日志里看不出最终调的是哪个内核函数。探针里的 `x16` 在进入 stub 前已经被覆盖，没抓到。目前不阻塞启动，后面需要换一种探针。
- **还有 471 个桩目标、47 个缺失数据项。** 已经按调用次数从高到低处理，但只有实际调用到的才会在日志里暴露。
- **"按参考内核顺序"推出来的配对（约 1.08 万个）没有逐个核对。** 这是文档推荐的做法，但不排除个别配错。目前查过的可疑项都被证实是对的：`vm_map_page_mask` 没配错，问题出在参数 `kernel_map`。
- **苹果 SoC 专用驱动的风险。** ApplePMGR、AppleARMPlatform 等是苹果芯片的驱动，在高通硬件上可能访问不存在的寄存器。如果遇到，需要换成自己的驱动，或者从闭包里移除。
- **这个内核没有存储驱动**（关闭了 UFS），根设备 UUID 是故意填的一个不存在的值，所以检查点 1 的终点就是停在等根设备，不会继续往下走。

## 7. 改动清单（都是源码，没有提交、没有上传）

**工作目录** `/home/xiaomi/sadnut-fuxi-local-20261103/`

- `build-ios17-kernel.sh`：内核和预链接（arm64e，fileset 内核排在最前，chained fixups，JOBS=8）
- `build-ios17-reference.sh`：参考内核；加 `--lto` 时把支持 LTO 的 ld 同时放到 `XNU_TOOLCHAIN_BIN` 和 `PATH` 最前面
- `build-ios17-probe-chain.sh`：内核 → 预链接 → 打包一条龙；只有 `REBUILD_REFERENCE=1` 时才重编参考内核
- `build-lto-toolchain.sh`：LTO 工具链（libLTO 加 ld64，下载失败会自动重试）
- `build-darwinboot-fv.sh`、`make-t32-darwinboot-v2-template.py`：DarwinBoot v2 换进固件卷
- `pack-ios17-kernel-test.py`：生成 ADT、写 payload、打 boot.img（不带 initrd，带探针启动参数）
- `device-ios17/`：iOS 17 设备覆盖层（board.toml、defconfig、`scripts/fuxi-kernel-baseline.sh` 等）

**UniXNU 仓库** `UniXNU/`

- `patches/xnu-10063-arm64e/`（新增目录）：
  - `native-pac-keys.patch`
  - `arm64e-threaded-starts.patch`
  - `upstream-clang-auth-null.patch`
  - `fileset-unmapped-call.patch`（含探针）
  - `split-block-keeps-mapping.patch`
  - `early-security-init-sign.patch`
- `kits/XnuPrelink/`：
  - `fileset_layout.py`（内核排在最前）
  - `fileset_segmented.py`（`__fips_hmacs` 改为可选，KC 头 subtype 和内核一致）
  - `fixtures/fileset-21H16-symbols.json`（29 个函数、34 个数据项，每项都记了原始摘要和证据）
- `scripts/unixnu/kernel.py`、`scripts/linux/prepare-xnu.py`：支持 arm64e

**工具链** `/home/xiaomi/lto-toolchain/`：llvm-project 源码、llvm-build（libLTO）、cctools-port，`ld-only/ld`

**状态**：上面这些改动都还没提交。`git status` 显示 `patches/xnu-10063-arm64e/` 是未跟踪目录，`kits/XnuPrelink` 有修改。按之前说好的，上传时**只传源码，不传产物**（boot.img、kernelcache、out-ios17 都不上传）。

## 8. 调试流程约定

按顺序来：先查官方文档（unixnu.com），再在内核里加探针收集证据，确认根因后才修。每个身份都必须有可复核的依据，比如函数体、相邻已配对函数、PAC 判别符签名、同位引用。证据不足的项不加入 fixture。
