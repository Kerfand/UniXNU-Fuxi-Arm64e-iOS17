# 设备身份（基带调试）注意事项

> ⚠️ **改设备身份会出现其他问题。** 日常使用请保持 iPhone 身份、不开基带。

为了让 CommCenter / ABM 走 Mav16 基带路径，`ios17/overlay/scripts/make-fuxi-adt.py --baseband`
把根 compatible 和 `/product` 型号改成了 **J121AP**（之前试过 D73AP、D53gAP）。
同时内核补丁 `zz-vmm-hide.patch` 在设置了 `fuxi-modem` 时默认隐藏 `kern.hv_vmm_present`，
MobileGestalt 的缓存也会被一次性清掉（`fuxi-logdump.m`）。

## 换身份后已知的问题

| 现象 | 已查到的情况 |
|---|---|
| 没有声音 | 内核音频通路正常（块发送/返回一致、PLL 锁定、无错误），问题在用户态：iPad 身份下系统不再把声音送往扬声器 |
| 网页打不开 | 换身份后出现；可能与国行按应用的蜂窝数据权限、iPad 身份有关，未定位 |
| 设置 → 蜂窝网络 白屏 / 只能打开一次 | 与身份和基带服务（大量 Apple 自定义 QMI 服务回 0x5e 不支持）有关 |
| 状态栏 无服务 / 无 SIM 卡 | 卡已就绪（USIM READY），但 CommCenter 报 "Not enough SIMs (0)"；D26/D27 正在补 ICCID |
| MobileGestalt 旧缓存 | 换身份后必须删除 MG 缓存，否则继续用旧型号 |

## 恢复日常使用

不加 `--baseband` 生成 ADT（iPhone 身份）、不带 `fuxi-modem` 启动参数，并清一次 MG 缓存。
