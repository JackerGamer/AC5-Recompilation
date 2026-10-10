# 皇牌空战5 静态重编译 · 键鼠与中文版

**《皇牌空战5：未颂之战》**（Ace Combat 5: The Unsung War，PS2 美版 SLUS-20851）的 Windows 原生版本。

本仓库是 [sal063/Ace-Combat-5-Static-recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation) 的修改版，在原项目基础上加入了**键盘 + 鼠标操作方案**、**摇杆反死区**和**简体中文设置菜单**，并修复了一处渲染代码的 bug。原版英文说明见 [README.en.md](README.en.md)。

> 仓库里**不含任何游戏代码或资源**。你需要自己准备正版游戏镜像，重编译器会在你的电脑上从中生成 C 代码。

## 这是什么

游戏的主 CPU 代码不是模拟运行的：一个 Python 工具读取原版可执行文件，提前把其中每个函数都翻译成 C 代码，再由 GCC 和一个代替 PS2 其余硬件的运行时一起编译。运行时用 Vulkan 绘制 GS 图形，并实现了 VU 向量单元、SPU2 音频、IPU 视频解码、IOP 模块、记忆卡和手柄。最终得到一个普通的 `ac5.exe`。

3D 画面（飞机、座舱、地形、地面物体、天空和云）也是原生绘制的：游戏在 VU1 上运行的顶点变换和光照程序被改写成了原生代码，几何体以真正的 GPU 网格绘制，支持 mipmap 和各向异性过滤。HUD、雷达和无线电字幕按窗口分辨率绘制，任何尺寸下都清晰。原生渲染器尚未覆盖的部分（菜单、机库、部分特效）仍通过模拟的 GS 绘制到同一帧中。

游戏可以完整通关。

## 本分支的改动

| 改动 | 说明 |
| --- | --- |
| 键鼠操作方案 | 鼠标控制俯仰、L2/R2 和视角，键盘负责其余操作，详见下方[操作](#操作) |
| 方块键循环 | 方块键每按一次切换一档：50% 力度按住 → 100% 力度按住 → 松开（对键盘、鼠标、手柄都有效） |
| 摇杆反死区 | 摇杆只要被推动，就至少输出设定比例的行程，让细小的动作越过游戏自带的死区 |
| 简体中文菜单 | F4 设置菜单完整汉化，可在"快速设置"页切换语言；使用系统自带的微软雅黑字体 |
| 修复 `ov_bbox` | `ps2_video.c` 中一处有符号/无符号比较错误：完全在屏幕左侧或上方之外的绘制被误判为覆盖整行 |

改动涉及的文件：`runtime/include/ps2_settings.h`、`runtime/include/ps2_hle.h`、`runtime/src/ps2_settings.c`、`runtime/src/ps2_video.c`、`runtime/src/ps2_ui.cpp`、`runtime/src/ps2_hle_pad.c`，每个文件开头都有修改说明。

## 需要准备

- **游戏**：美版，序列号 SLUS-20851，ISO 镜像或解包后的光盘文件均可。其他地区版本无法使用，因为配置文件绑定了美版可执行文件中的地址。
- **64 位 Windows**，以及支持 Vulkan 的显卡。
- **[MSYS2](https://www.msys2.org)**：提供 GCC、SDL3、CMake 等全部编译工具。
- **Python 3**：只用到标准库，不需要 pip 安装任何东西。

与原版说明不同，本分支**不需要单独安装 Vulkan SDK**：Vulkan 头文件和着色器编译器 `glslc` 都可以从 MSYS2 安装。

## 第 1 步：取出游戏主程序

重编译器只需要光盘中的一个文件：游戏主程序 `SLUS_208.51`，位于 ISO 根目录。在资源管理器中双击 ISO 挂载，或用 7-Zip 打开，把这个文件复制出来。

先确认文件是否正确：

```powershell
Get-FileHash .\SLUS_208.51 -Algorithm SHA256
```

结果应为：

```
C3594227605307806592416DBD723BF5771952E279EE281A40DA305426667385
```

文件大小应正好是 3,634,092 字节。如果哈希值不符，请停下：说明你的是其他地区版本或被修改过的程序，重编译结果将无法和配置文件对应。

ISO 要保留：可执行文件只是代码，游戏运行时仍要从光盘读取模型、贴图、音效和过场动画。

## 第 2 步：安装编译工具

1. 安装 MSYS2（默认路径 `C:\msys64`），例如：

   ```powershell
   winget install MSYS2.MSYS2
   ```

2. 打开 **MSYS2 UCRT64** 终端，先更新，再安装编译所需的包：

   ```
   pacman -Syu
   pacman -S mingw-w64-ucrt-x86_64-{gcc,sdl3,pkgconf,cmake,ninja,vulkan-devel,shaderc}
   ```

   GCC 必须是 **15 或更高版本**。生成的代码依赖有保证的尾调用（`[[gnu::musttail]]`），旧版编译器编出的程序可能栈溢出。

之后的命令都在 PowerShell 中、从本仓库根目录运行。先把 MSYS2 的 `bin` 目录加到 PATH 最前面（只对当前窗口有效）：

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
```

## 第 3 步：重编译

```powershell
$env:PYTHONPATH = "tools"
python -m ps2recomp "C:\路径\SLUS_208.51" -o generated `
    --ida-db config/ida_db.json `
    --ida-seeds config/ida_seeds.json `
    --symbols config/sdk_symbols.json `
    --symbols config/manual_symbols.json `
    --overrides config/overrides.json `
    --hooks config/hooks.json
```

大约 15 秒，最后一行应为：

```
emitted 10742 functions into 43 files in 15.0s
```

这会生成约 50 MB 的 `generated/` 文件夹。**所有参数都要带上**：`--overrides` 把访问 PC 上不存在的硬件（SIF RPC、光驱、手柄等）的 SDK 函数替换为原生实现，`--hooks` 在游戏自身的场景切换、文件打开、音频加载等函数上挂接原生处理。缺了它们，游戏会一直等待永远不会响应的硬件。

## 第 4 步：编译

```powershell
cmake -S . -B build/gcc -G Ninja -DCMAKE_BUILD_TYPE=Release `
    -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe `
    -DCMAKE_CXX_COMPILER=C:/msys64/ucrt64/bin/c++.exe
cmake --build build/gcc
```

每个生成的文件基本上都是一个巨大的函数，GCC 编译较慢，12 线程 CPU 上大约需要 3 到 6 分钟。完成后得到：

- `build/gcc/ac5.exe`：游戏
- `build/gcc/shaders/`：编译好的着色器，必须和 `ac5.exe` 放在一起
- `build/gcc/gsreplay.exe`：回放图形捕获的开发工具，游玩不需要

## 第 5 步：运行

把下面三个 DLL 从 `C:\msys64\ucrt64\bin` 复制到 `build\gcc`（`ac5.exe` 旁边），这样不依赖 PATH 也能启动：

- `SDL3.dll`
- `libwinpthread-1.dll`
- `libiconv-2.dll`（SDL3 依赖它，缺少时会弹出"找不到 libiconv-2.dll"）

`vulkan-1.dll` 由显卡驱动提供，不需要复制。

然后在仓库根目录运行：

```powershell
.\build\gcc\ac5.exe --data generated --disc "C:\路径\Ace Combat 5 - The Unsung War (USA) (En,Ja).iso" --watchdog 0
```

- `--data`：包含 `ps2_image.bin` 的文件夹，即 `generated`。
- `--disc`：ISO 文件，或解包后的光盘文件夹。
- `--watchdog 0`：关闭看门狗。否则游戏 10 秒没有输出画面时运行时会自动退出，这在调试时有用，游玩时不需要。

启动后窗口会黑屏约 20 秒，这是正常的。日志输出到控制台窗口，不要关闭它。

**请从仓库根目录启动**（或者把快捷方式的"起始位置"设为仓库根目录）：存档 `saves/`、Mod 文件夹 `mods/` 以及 `config/` 中的名称表都是相对于启动时的工作目录查找的。设置保存在 `ac5.exe` 旁边的 `ac5_settings.ini`。

## 操作

### 键盘 + 鼠标（本分支的默认方案）

| PS2 | 键鼠 |
| --- | --- |
| 左摇杆 | WSAD；鼠标上下也控制左摇杆的上下（默认反向：鼠标向上 = 机头抬起） |
| 右摇杆 | 按住空格 + 移动鼠标（松开空格回中） |
| 方向键 | Alt + WSAD |
| L1 / R1 | Ctrl / Shift |
| L2 / R2 | Q / E；鼠标左右移动也会按下 L2 / R2（带力度） |
| R3 | CapsLock |
| 开始键 | Enter |
| 选择键 | 鼠标滚轮（每格按一下） |
| 交叉键 | 鼠标左键 |
| 三角键 | 鼠标右键 |
| 圆圈键 | 鼠标侧键 4 |
| 方块键 | 鼠标侧键 5（循环：50% 力度按住 → 100% 力度按住 → 松开） |

- **键盘优先**：同一个轴上有键盘按键按住时，鼠标对这个轴不起作用。比如按着 Q 或 E 时，鼠标左右不会触发 L2/R2；按着 W 或 S 时，鼠标上下不改变俯仰。
- **鼠标锁定**：游戏窗口在前台、且设置菜单关闭时，鼠标会被锁定并隐藏。按 F4 打开菜单或 Alt+Tab 切走即可释放。点击窗口回到游戏时，那一下点击不会被当作交叉键。
- 按住 Alt 时 Enter 不会触发开始键，因为 Alt+Enter 是切换全屏。
- 鼠标灵敏度、回中速度、鼠标死区和上下反向可在 F4 →"键盘和鼠标"中调整。鼠标控制的摇杆同样受"摇杆与死区"页中的灵敏度、死区和反死区影响，L2/R2 不受影响。

> **提示**：Windows 连按 5 次 Shift 会弹出"粘滞键"对话框并抢走焦点。建议在 设置 → 辅助功能 → 键盘 → 粘滞键 中关闭"粘滞键的键盘快捷方式"。另外 CapsLock 每按一次也会切换系统的大写锁定状态。

### 手柄

手柄通过 SDL 识别，插上即可使用。按键按位置对应：下方面键为交叉键，右为圆圈键，左为方块键，上为三角键；肩键为 L1/R1，扳机为 L2/R2，按下摇杆为 L3/R3，Back 和 Start 为选择键和开始键。方块键的循环规则对手柄同样有效。

### 其他按键

- **F4**：打开 / 关闭设置菜单（所有按键都可在菜单中重新绑定）
- **F11** 或 **Alt+Enter**：切换全屏
- **Esc**：直接退出游戏（没有确认提示），菜单打开时为关闭菜单
- **F6 ~ F10**：调试热键（捕获、状态转储），游玩时可忽略

## 设置菜单

按 F4 打开。菜单语言可在"快速设置"页顶部或"显示 → 界面"中的 **Language / 语言** 切换（中文需要系统中有微软雅黑、等线、黑体或宋体中的任意一种）。

"摇杆与死区"页包含 PCSX2 的 DualShock 2 模拟摇杆设置（死区、灵敏度、按键/扳机死区、反向），以及本分支新增的**摇杆反死区**：摇杆只要被推动，就至少输出这个比例的行程，方向不变，推到底时不受影响。摇杆松开后不能精确回中的手柄，需要同时设置一点死区，否则飞机会漂移。

## Mod

Mod 放在启动目录下的 `mods` 文件夹中，每个 Mod 一个子文件夹，不需要重新打包。Mod 可以替换文件（包括 `DATA.PAC` 内的文件）、通过 `params.txt` 修改游戏参数、应用数据类的 PCSX2 `.pnach` 补丁、运行 Lua 脚本，以及在 `mod.toml` 声明 `native = true` 后加载原生 DLL。

> **安全提示**：带 DLL 的 Mod 可以以你的权限在电脑上执行任意代码，而 `native = true` 是 Mod 自己声明的，不需要你确认。来路不明的 Mod，请先检查里面有没有 `.dll` 文件。

详细说明见 [mods/README.md](mods/README.md) 和 [README.en.md](README.en.md) 中的 Mods 部分。游戏异常时可先设置环境变量 `PS2_NO_MODS=1` 排除 Mod 的影响。

## 常见问题

- **CMake 报 `Cannot find source file: .../generated/ps2_func_table.c`**：还没做第 3 步，或输出目录不是 `generated`。
- **CMake 找不到 Vulkan、`sdl3` 或 `glslc`**：第 2 步的 PATH 命令没有在当前窗口执行，或 MSYS2 包没有装全。
- **CMake 警告编译器不支持 musttail**：GCC 版本太旧，需要 15 或更高。
- **启动时提示找不到某个 DLL**：见第 5 步，把三个 DLL 复制到 `ac5.exe` 旁边。
- **日志提示 `vk: cannot open shader`**：`shaders` 文件夹不在 `ac5.exe` 旁边。放回去，或设置 `PS2_SHADER_DIR` 指向 `.spv` 文件所在位置。
- **游戏自己退出并提示 `==== WATCHDOG: the guest delivered no field for 10 seconds`**：启动时漏了 `--watchdog 0`。

## 法律声明

Ace Combat 是 Bandai Namco Entertainment 的商标。本项目与其没有任何关联，也未获其认可。仓库中不包含任何游戏文件，也不会提供。

编译生成的 `ac5.exe`、`generated/` 文件夹和 `ps2_image.bin` 都包含由游戏代码转换而来的内容，与游戏镜像一样受版权保护，请勿分发。

## 许可证

原项目由 sal063 以 Apache License 2.0 发布，本修改版沿用同一许可证，见 [LICENSE](LICENSE)。Dear ImGui 采用 MIT 许可证，见 `third_party/imgui/LICENSE.txt`；Lua 同样采用 MIT 许可证，见 `third_party/lua/LICENSE.html`。
