# 喵喵输入法 · MeowIME

## ⬇️ [**点这里下载安装包**](https://github.com/cda775819-blip/MeowIME/releases/latest/download/MeowIME_Setup.exe)

**39.8 MB · Windows 10 / 11 (x64) · 安装时右键选「以管理员身份运行」**

> ⚠️ 首次运行会被 SmartScreen 拦住（「Windows 已保护你的电脑」）——安装包没有代码签名证书。
> 点「**更多信息**」→「**仍要运行**」即可，这是未签名分发的常态，不是文件有问题。

也可以到 [**Releases 页面**](https://github.com/cda775819-blip/MeowIME/releases/latest) 看更新日志。
链接指向 `releases/latest`，**永远是最新版**，不用改。

安装步骤、卸载方法见下面的 [安装](#安装普通用户)。

---

一个**真正的 Windows 输入法**，把中文句子在句末改写成喵喵风格：`我爱你。` → `本喵爱你喵。 (^ω^ฅ)`

> 走的是 **TSF（Text Services Framework）** 正规路线——注册成 COM 进程内文本服务（TIP），
> 不是键盘钩子、不是剪贴板替换、不是「假输入法」。
> 中文引擎用 [librime](https://github.com/rime/librime)，方案为 `luna_pinyin`。

![候选框 · 浅色](docs/candidate-light.png)

---

## 这是什么 / 不是什么

**是**

- 标准 TSF 键盘输入法：`ITfTextInputProcessor` + `ITfKeyEventSink` + `ITfEditSession` + `ITfCompositionSink`
- 通过 `ITfInsertAtSelection(TF_IAS_QUERYONLY)` 取 range → `StartComposition` → `SetText` 落字
- 自绘候选窗（GDI+ 分层窗口），不依赖宿主程序渲染
- 全文件零网络代码

**不是**

- ❌ AutoHotkey / 键盘钩子
- ❌ 安卓侧读剪贴板做文本替换
- ❌ 套壳商业输入法

---

## 特性

| 特性 | 说明 |
|---|---|
| **喵化改写** | 句末（`。！？；…`）或按 Enter 发送前，把整句改写成喵喵风格 |
| **逐句加喵** | `你好，今天不错。` → `你好喵，今天不错喵。 (表情)` |
| **随机颜文字** | 内置 70 个猫系颜文字，随机追加 |
| **中英切换** | 单按 Shift 切换（交给 Rime 的 `ascii_composer`，不是自己实现） |
| **自绘候选窗** | 圆角 + 柔和投影 + 深色主题 + DPI 缩放 + 中/英状态徽标 |
| **安全改写** | 改写前**回读校验**，对不上就整个放弃——宁可不动，也不覆盖用户别处的文字 |

### 候选窗

| 浅色 | 深色 |
|---|---|
| ![](docs/candidate-light.png) | ![](docs/candidate-dark.png) |

左侧「中 / 英」徽标会在切换中英时闪一下。以前没有这个指示，切到英文后打不出汉字完全没有线索。

---

## 安装（普通用户）

1. 下载 [**MeowIME_Setup.exe**](https://github.com/cda775819-blip/MeowIME/releases/latest/download/MeowIME_Setup.exe)
2. **右键 → 以管理员身份运行**（要写 HKLM 的输入法注册项）
3. 装完打开 **设置 → 时间和语言 → 语言和区域 → 中文(简体) → 添加键盘**，选中「喵喵助手」
4. 用 `Win + Space` 切换

| | |
|---|---|
| 安装位置 | `%LOCALAPPDATA%\Programs\MeowIME` |
| 卸载 | `MeowIME_Setup.exe /u`（同一个安装包加 `/u` 参数） |
| 升级 | 关掉正在用输入法的程序（聊天软件、浏览器、资源管理器）后再装，或先注销一次最干净 |

> 卸载时如果还有程序在用这个输入法，DLL 被锁着删不掉是正常的，
> 注销一次手动删掉安装目录即可。注册项在卸载时已经清掉了。

---

## 从源码构建（开发者）

### 环境要求

- Windows 10 / 11，**x64**
- Visual Studio 2022 BuildTools（需要 `vcvars64.bat`）
- [librime](https://github.com/rime/librime) 的 `include` / `lib`

### 1. 准备 librime

本仓库**不包含** librime 的二进制和 Rime 词典数据（体积大、且各有自己的许可证）。请自行获取，并按下面结构放置：

```
<项目根>/
├─ rime_dl/
│  └─ rime/
│     └─ dist/
│        ├─ include/rime_api.h      ← 编译需要
│        ├─ lib/rime.lib            ← 链接需要
│        └─ bin/rime_deployer.exe   ← 重建词典需要
├─ rime.dll                          ← 运行需要（放在 DLL 同目录）
├─ rime_data/                        ← Rime 共享数据（luna_pinyin 方案等）
└─ rime_user/                        ← Rime 用户数据（会自动创建）
```

> DLL 是按**自身所在目录**去找 `rime.dll` / `rime_data` / `rime_user` 的。
> `luna_pinyin` 方案与词典可从 Rime 的 [rime-data](https://github.com/rime/rime-data) 等仓库获取。

### 2. 构建

```powershell
# 只编译到 _build\CatTextService.dll
.\build.ps1

# 编译 + 部署 + 注册（改完代码跑这个）
.\build.ps1 -Deploy

# 还改了 rime_data 下的配置时加上（必须重建，否则运行的是旧编译产物）
.\build.ps1 -Deploy -RebuildRime

# 打开/关闭运行时调试日志
.\build.ps1 -Debug
.\build.ps1 -NoDebug
```

`build.ps1` 会处理几件容易踩坑的事：

- **`/MT` 静态链接 CRT** —— 否则会依赖 `MSVCP140.dll`，在没装 VC++ 运行库的机器上加载不起来
- **换名部署绕过 DLL 占用** —— 正在运行的程序锁着旧 DLL 时，自动改用 `CatTextService.N.dll` 并把注册指过去，不用你手动关程序
- **只改 HKCU 的 CLSID** —— 不需要管理员权限，并打印回滚命令

### 3. 启用输入法

安装后到 **设置 → 时间和语言 → 语言和区域 → 中文(简体) → 添加键盘**，选中「喵喵助手」。
用 `Win + Space` 切换。

---

## 目录结构

```
CatTextService.cpp      核心实现（TSF 文本服务 + 自绘候选窗，约 1800 行）
CatTextService.def      COM 导出
installer.cpp           单文件安装器（把文件作为资源内嵌，运行即解压安装）
installer.manifest      安装器清单（requireAdministrator）
build.ps1               编译 + 部署 + 注册 + 重建 Rime  ← 主要入口
build_installer.ps1     打包 MeowIME_Setup.exe
docs/                   文档与截图
docs/改造前后对比.md     详细的改造记录（含实测证据与方法论）
```

---

## 已知限制

诚实地列出来，免得你踩坑：

| 限制 | 说明 |
|---|---|
| **仅 64 位** | 没有 32 位版本。32 位程序里输入法会出现在列表中但选中无反应 |
| **首次输入有卡顿** | 引擎初始化（词典部署）发生在第一次按键的回调里，会阻塞宿主 UI 线程 |
| **引擎进程内加载** | librime 被加载进每一个使用输入法的程序。引擎崩溃会带走宿主程序 |
| **单会话** | 同一进程内多个文档共用一份 Rime 会话，理论上会串词 |
| **改写是事后改文档** | 喵化在文本上屏**之后**改写宿主文档，而不是在组合阶段完成。因此依赖回读校验兜底 |
| **候选框不支持鼠标** | 只能用键盘选择，没有点选和翻页 |
| **`我` → `本喵` 是全局替换** | 「我们」会变成「本喵们」 |

更完整的失败模式清单见 `docs/改造前后对比.md` 第 4 节。

---

## 第三方依赖与许可

**本仓库不包含**下列组件的二进制或数据，需要自行获取（见「快速开始」）。

| 组件 | 用途 | 许可 |
|---|---|---|
| [librime](https://github.com/rime/librime) | 中文输入引擎 | **BSD-3-Clause** — `Copyright (c) 2014, RIME Developers` |
| [rime-luna-pinyin](https://github.com/rime/rime-luna-pinyin) | `luna_pinyin` 方案与词典 | **LGPL-3.0** |
| [rime-essay](https://github.com/rime/rime-essay) | 语言模型 `essay.txt` | **LGPL-3.0** |
| [rime-stroke](https://github.com/rime/rime-stroke) | 笔画反查方案 | **LGPL-3.0** |
| [rime-prelude](https://github.com/rime/rime-prelude) | `default.yaml` 等预置配置 | **LGPL-3.0** |
| [OpenCC](https://github.com/BYVoid/OpenCC) | 简繁转换数据 | **Apache-2.0** |
| GDI+ | 候选窗绘制 | Windows 系统组件 |

本项目自身的代码以根目录 [`LICENSE`](LICENSE)（MIT）为准。

### ⚠️ 分发二进制时请注意

`MeowIME_Setup.exe` 里**打包了** librime 的二进制和上述 Rime 数据，因此：

- **BSD-3-Clause** 要求二进制分发时附带版权声明、条件列表和免责声明
- **LGPL-3.0** 要求附带许可证全文，并提供对应源码的获取途径

仓库根目录的 [`THIRD-PARTY-NOTICES.txt`](THIRD-PARTY-NOTICES.txt) 和 [`licenses/`](licenses/) 就是为此准备的，
`build_installer.ps1` 会自动把它们打进安装包，安装后释放到安装目录。

自己打包分发时**不要删掉这些文件**。

---

## 隐私

- **整个项目没有任何网络代码。** 输入法属于最高信任等级的软件，这个项目可以完整审计。
- 调试日志（`CatTextService.debug` 开关控制）会把你**输入过的内容明文**写入 `cand_debug.log`，
  默认关闭，排查完请删除该文件和开关文件。
- 喵化改写会**修改你正在输入的文本**。请知悉这一点再使用。

---

## 文档

- [`docs/改造前后对比.md`](docs/改造前后对比.md) —— 12 项问题的症状、实测证据、根因、代码前后对比，
  以及**用真实引擎跑行为探针**和**离屏渲染验证 UI** 的方法论。想改这个项目的话建议先看它。
