# xiaoji UMDF2 HID 驱动

`xiaoji` 是基于 [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) 完整
UMDF2 驱动实现改造的用户态 HID 驱动。当前包名、二进制名、设备硬件 ID 和
注册表/共享内存命名均已切换为 `xiaoji`，驱动主体保留 HIDMaestro 的完整
HID 报告、共享内存输入、Feature/Output 报告和版本握手逻辑。

上游快照：

- commit `d9199785d09a3c206dd30caf163b33295a5ad0a1`
- 归档 SHA-256 `43bdeb90d7ac857ab8b5701ec7668e783332fc3b24d065e155396710a56f2a36`
- 许可证：MIT，见 [HIDMaestro 原始许可证](../third_party/HIDMaestro/LICENSE)

## 目录

```text
driver/                  UMDF2 驱动、共享协议和 INF
host/                    设备管理、输入和本地签名安装器
app/                     xiaoji 驱动管理器窗口
tests/                   命令行诊断、设备检查与 Raw Input 观察
scripts/                 构建、打包和验证脚本
third_party/HIDMaestro/  上游许可证与归属信息
```

## 构建

驱动构建需要 Visual Studio 2022/2026 的 MSVC x64 工具和配套的 Windows SDK/WDK（含 UMDF 2.15）。脚本会通过 `vswhere` 查找 Visual Studio，并从注册表读取 Windows Kits 安装位置。

从仓库根目录运行 `.\scripts\build.cmd x64 driver`，只编译 `xiaoji.dll` 并复制 `xiaoji.inf`，不生成管理器、宿主或诊断程序。默认输出到 `x64/driver/`；可用 `XIAOJI_OUT_DIR` 指定目录。需要生成或更新未签名的 `xiaoji.cat` 时，改用 `.\scripts\build.cmd x64 driver package`；不要将旧 CAT 与新 DLL 混用。

只构建命令行管理工具用 `.\scripts\build.cmd x64 cli`，只需 MSVC 和 Windows SDK，产物 `x64/cli/xiaoji.exe` 可独立运行。全量构建另外需要 .NET Framework 4.8、SignTool 和 Inf2Cat。可用环境变量显式指定工具路径：

```powershell
$env:XIAOJI_WINDOWS_SDK_ROOT = 'D:\Windows Kits\10'
$env:XIAOJI_WDK_ROOT = 'D:\Windows Kits\10'
$env:XIAOJI_WDK_VERSION = '10.0.28000.0'
.\scripts\build.cmd x64 package
```

输出：

```text
build/xiaoji.dll
build/xiaoji.inf
build/xiaoji.cat
build/xiaoji-host.exe
build/xiaoji.exe
build/xiaoji-gui.exe
build/xiaoji-test-cli.exe
```

`scripts/build.cmd` 不写证书库。`build/` 中的 DLL 和 CAT 仍未签名；宿主 EXE 内嵌的是这次构建的 DLL、INF、安装助手、SignTool 和 Inf2Cat。目标测试机只需这个宿主和 .NET Framework 4.8，不需要安装 WDK。微软工具仍受相应 SDK/WDK 许可约束，对外分发前应确认再分发权。

## 测试机本地签名

先检查当前 EXE 的内嵌包。这个命令会在临时目录运行真正的 SignTool 和 Inf2Cat，不创建证书、不访问 Driver Store、不发送输入：

```powershell
.\build\xiaoji-host.exe install --check
```

在**你控制的 x64 Windows 测试机**上，以管理员身份打开交互式终端运行：

```powershell
.\build\xiaoji-host.exe install
```

首次输入 `TRUST` 后，安装器才创建专用的 `CN=xiaoji UMDF2 Local Test` 代码签名证书（RSA-3072、SHA-256、有效期一年）。私钥不可导出，保存在本机；仅公钥证书加入 `LocalMachine\Root` 和 `LocalMachine\TrustedPublisher`。这是**整机发布者信任**，不只针对一个 DLL；不要在客户机上把它当作正式签名。不会启用 `TESTSIGNING`、关闭 Secure Boot，也不使用 `HIDMaestroTestCert`。

安装器只签内嵌的 xiaoji 包：签 DLL → 重新生成 CAT → 签 CAT → 验签 → `pnputil /add-driver` 暂存。随后通过 SetupAPI 核对 Windows 实际发布的 OEM 包，重新验签 Driver Store 中的 INF、DLL、CAT，记录这三份实际文件的摘要，并打印其 INF 路径。若 Windows 复用了同一份 INF 的旧包，会明确报告「复用」；不会把另一个构建的缓存摘要当成安装记录，也不会批量更新匹配设备。相同候选包和证书会复用本机的签名缓存，复用前再次验签；指纹记录在 `HKLM\SOFTWARE\XiaojiUmdf2LocalSigning`。

只更新管理器或宿主不需要替换正在运行的驱动。要实际发布新的驱动 DLL，请提高 `driver/xiaoji.inf` 的 `DriverVer` 后重新构建、签名和安装。只重编 DLL 或重新签 CAT，不保证 Windows 会替换原有包。

**撤销测试信任：** `xiaoji-host remove` 只移除测试设备，不删除驱动包或证书。先在管理员 PowerShell 运行 `(Get-ItemProperty 'HKLM:\SOFTWARE\XiaojiUmdf2LocalSigning').Thumbprint` 核对实际指纹。确认不再测试后，在 `certlm.msc` 的“受信任的根证书颁发机构”和“受信任的发布者”中按该指纹移除证书；退役签名密钥时，再从“个人”删除对应证书及私钥。只有要重新初始化时才删除该项目专属的指纹记录。已签名包仍可能留在 Driver Store；不要按证书名称批量删除或强制删除其他设备的包。

## 可选图形管理器

**双击 `build/xiaoji-gui.exe` 打开图形管理器。** `build/xiaoji.exe` 是独立命令行，不打开窗口；安装、卸载、验签和状态查询见[命令行指南](cli.md)。GUI 目标为 x64 Windows、.NET Framework 4.8。打开窗口不会自动安装、签名或发送输入。发送键鼠输入需要管理员权限。

1. 点「离线自检」检查内嵌包，或点「设备状态」查看在线节点；状态和日志都在窗口内。
2. 在你控制的电脑上，以管理员身份点「安装驱动」。首次建立整机证书信任需要单独确认。安装将签名包加入 Driver Store，并创建或复用两个固定的 PnP 节点；成功时键盘、鼠标都已启动。
3. 点「键盘：点按 X」或「鼠标：向右移动」即向当前桌面发送真实输入，无确认框、无接收窗口。未安装时只报错，不自动安装。重复输入和关闭 GUI 都不删除设备；重新打开后可以继续使用。日志中的“已发送”表示 HID 已确认报告，不代表某个应用已处理它。
4. 只有点「卸载驱动」并确认才移除两个固定设备及其配置，然后删除与本机安装记录中 INF/DLL/CAT 摘要匹配的 OEM 包。不会强制删除仍在使用的包；失败会保留错误供重试。保留测试证书和签名缓存；卸载不是撤销整机证书信任。

「验证安装」先验签实际绑定的 Driver Store 包，再显示已启动设备数及是否与本构建逐字节一致。同一 INF 下已验签的不同构建不是签名错误；没有本构建的签名缓存时显示「未比对当前构建」，不会要求为验证重装。没有活动设备时可以验证已暂存的包，但不会把暂存报告为设备已启动。

**按键连发：** 在 GUI 选择一个字母键（A–Z，不附加 Shift）、数字键或空格，设置次数（1–1000）和间隔（10–5000 ms），点「开始连发」。默认是 X 键、10 次、100 ms。间隔从上一次释放确认后算起，不是固定频率；最后一次不再等待。输入直接进入当前桌面，不弹窗、不更改设备安装状态。

如果运行中的管理器占用了构建产物，可以设置 `XIAOJI_OUT_DIR` 为独立输出目录（如 `x64\gui-dev`）后重新构建。

进行中可点「停止连发」，关窗也会请求停止并等待宿主退出。正在进行的点按会先尝试释放；显示的完成次数只统计 HID 已确认按下和释放的点按，不代表前台应用实际接收次数。若 HID 确认超时，程序报错并停止，不重试点按。`xiaoji-host.exe repeat-test` 可在私有共享内存中检查连发和取消，不操作桌面。

需要脚本调用时使用独立的命令行版 `xiaoji-test-cli.exe`：

```powershell
.\build\xiaoji-test-cli.exe offline
```

| 参数 | 检查内容 |
| --- | --- |
| `offline`（默认） | 用固定数据检查键鼠报告的判定规则，再从 EXE 解出同次构建的包及工具，实际运行 SignTool 和 Inf2Cat。通过不代表驱动已签名或已加载。 |
| `install-test` | **仅在隔离测试机，以管理员交互运行。** 输入 `INSTALL` 才调用真实安装器；首次建立本机信任还需要输入 `TRUST`。随后进行 `verify`。驱动包和本机信任会保留，不自动卸载。 |
| `verify` | 只读验签实际发布包并检查设备状态；有本构建缓存时另比对三个文件的 SHA-256。已验签的不同构建仍可通过安装验证，输出会明确区分；缺包或实际验签失败仍报错。读取受保护缓存时使用管理员终端。 |
| `devices` | 只读检查 xiaoji 固定键鼠实例，显示启动状态、PnP 问题码和绑定 INF。关闭 GUI 后仍可查询已安装的节点。 |
| `observe-keyboard` / `observe-mouse` | 打开前台窗口，最多观察 30 秒。只认父实例是对应 xiaoji 节点的 Raw Input：键盘须从同一设备收到 X 的按下和释放，鼠标须收到非零相对位移。窗口本身不产生输入，也不把物理键鼠算作成功。 |

命令行版 `observe-*` 仅供需要独立观察 Raw Input 时使用，不参与 GUI 的日常输入流程。

退出码：`0` 本项通过；`1` 检查或安装器失败；`2` 参数错误或在 `INSTALL` 确认处取消；`3` 没有设备、未收到目标输入或提前关闭观察窗口。

## 设备与协议

第三方程序请使用 [客户端 SDK 与调用示例](developer.md)，无需自行创建下列内部命名对象；共享内存布局不是面向业务程序的公共 ABI。

- 硬件 ID：`ROOT\XIAOJI`
- 默认实例：`ROOT\XIAOJI\KBD0001`、`ROOT\XIAOJI\MOU0001`
- 每实例配置：`HKLM\SOFTWARE\Xiaoji\Controller<N>`
- 输入共享内存：`Global\XiaojiInput<N>`（362 字节，HIDMaestro v2 布局）
- 输入事件：`Global\XiaojiInputEvent<N>`
- 输入确认：`Global\XiaojiInputAck<N>` / `Global\XiaojiInputAckEvent<N>`
- 版本握手：`Global\XiaojiVersion<N>`

键盘输入报文为 28 字节，无报告 ID。前 8 字节保留旧版的 6 键数组布局，后 20 字节用于同时表示多个按键；键盘 LED 输出报文不变。

| 字节偏移 | 含义 |
| --- | --- |
| 0 | 位 0–7 为修饰键 `0xE0`–`0xE7`，可同时置位。 |
| 1 | 保留，恒为 0。 |
| 2–7 | 6 个非修饰键用法组成的数组，每项 1 字节，空位为 0。 |
| 8–27 | Keyboard/Keypad 页位图：用法 `u` 对应字节 `8 + u/8`（整除）的第 `u%8` 位。范围 `0x00`–`0x9F`，其中 `0x00`–`0x03` 的位恒为 0。 |

旧 GUI、宿主和 ABI 1 客户端仍提交 8 字节键盘帧。驱动先将目标报文清零再复制帧，因此新版设备收到的位图全为 0，数组和修饰键的含义不变。位图是设备能力，本次没有新增发送整份状态的客户端接口。

鼠标输入报文仍为 4 字节，无报告 ID：字节 0 的位 0–4 依次为左键、右键、中键、按键 4、按键 5，高 3 位为 0；字节 1–3 仍为 X、Y 和滚轮的有符号相对值（−127 到 127）。

用新版 `xiaoji.exe install <INF>` 迁移已有设备时，安装器核对归属后更新控制器布局，重启相应的固定设备，再检查 HID 子设备实际加载的布局。`status` 和无参数 `verify` 查询的是运行中的 HID 能力，不是注册表：键盘显示 `layout: nkro-28` 或 `layout: legacy-8`，鼠标显示 `buttons: 5` 或 `buttons: 3`。旧布局会提示升级但仍支持 ABI 1；无法查询时会显示原因。

`xiaoji-host.exe self-test` 只验证协议布局、报告描述符和键位映射，不会
安装驱动或产生真实输入：

```powershell
.\build\xiaoji-host.exe self-test
```

如已有签名包，仍可用 `install <INF>` 安装包和两个持久设备，不创建本机证书，也不替外部文件签名。宿主无参数的 `install` 子命令只负责本地签名和暂存包；GUI「安装驱动」会另外完成设备安装。
下例的 `$inf` 须设为本机安装器打印的绝对路径或你自己签好的 INF，不是未签名的 `build/xiaoji.inf`。
`type` 和 `mouse` 只连接已安装的设备，并核对绑定的包与指定 INF 同目录的文件一致，不再创建或删除设备。`remove` 只移除本项目设备及配置，保留包；GUI 的「卸载驱动」同时处理本地自签安装记录对应的包。

```powershell
.\build\xiaoji-host.exe install $inf
.\build\xiaoji-host.exe type $inf "xiaoji"
.\build\xiaoji-host.exe mouse $inf 20 -10
.\build\xiaoji-host.exe remove
```

安装器在注册根设备前写入控制器配置和 `ControllerIndex`。驱动在 `EvtDeviceAdd` 时按该索引读取描述符、报文长度和 VID/PID；运行时从共享内存读取输入帧。
