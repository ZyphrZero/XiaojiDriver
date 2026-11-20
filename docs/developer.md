# 第三方应用接入 xiaoji

应用加载 **`xiaoji_client.dll`**，不是 `xiaoji.dll`。后者由 Windows 的 UMDF 宿主加载，不提供给业务进程调用。客户端通过与 xiaoji 管理器相同的共享内存、事件和报告确认机制向驱动发送输入；不需要启动 GUI、常驻助手进程、WDK 或签名工具。

## 首次部署

1. **首次安装驱动。** 在管理员终端执行 `xiaoji.exe install <已签名包的 xiaoji.inf 路径>`，随后用 `xiaoji.exe verify` 检查安装。CLI 不签名、不创建证书；仅复制客户端 DLL 或暂存 INF 不会创建设备。安装完成后，应用启动时只需检查设备，不必再次安装。完整命令见[管理指南](cli.md)。
2. **取得客户端 SDK。** 从包含 SDK 的版本下载 `xiaoji-client-sdk-<版本>-win-x64.zip`，可用 `Get-FileHash` 将下载文件的 SHA-256 与 `SHA256SUMS.txt` 比对；也可从源码运行 `.\scripts\build.cmd x64 sdk`，输出在 `x64/sdk/`，只需要 MSVC x64 和 Windows SDK。`XIAOJI_OUT_DIR` 可覆盖输出目录。SDK 与未签名驱动 ZIP 是不同的包。集成或再分发时请查看包内的 AGPLv3 许可证及第三方声明。
3. **集成到自己的 x64 应用。** 把 `xiaoji_client.dll` 放在可信的应用目录。C/C++ 包含同版 `xiaoji_client.h`、链接 `xiaoji_client.lib`；C# 用 P/Invoke，Python 3 用随包的 `xiaoji.py`（仅标准库）。不要从当前工作目录或不可信搜索路径随意加载 DLL。首次调用先检查 ABI 版本是否为 `2`。

## 日常调用顺序

**查询 → 检查要用的设备 → 按需发送 → 检查每次返回值。** 查询不发送输入；发送需要以管理员权限运行调用进程，SDK 不会弹出提权或安装提示。不需要 INF 路径、`oemNN.inf` 编号或设备句柄。SDK 只接受固定实例、硬件 ID、所有者配置、控制器索引、绑定包名及启动状态符合预期的设备。状态查询不是对文件签名的重新验证。退出程序不会卸载驱动。若使用会话，调用方须负责松开按键并关闭会话。

### Python

把 `xiaoji.py` 与客户端 DLL 放在一起，使用 **64 位 Python**。可先运行 `python xiaoji.py` 做只读设备检查。下例默认也只读；取消注释的行会向**当前桌面**发送真实输入，而不是投递给某个后台窗口：

```python
from xiaoji import Xiaoji

client = Xiaoji()
devices = client.devices()
if devices & Xiaoji.KEYBOARD:
    print("键盘已启动")
    # client.check_connection(Xiaoji.KEYBOARD)  # 管理员权限；等待驱动就绪，不发送输入
    # client.tap(0x1B)           # X 物理键的一次按下和释放，不带 Shift
    # client.type_ascii("Hi!")  # 使用 US 键位映射
if devices & Xiaoji.MOUSE:
    print("鼠标已启动")
    # client.move(12, -5)       # 相对移动
    # client.move(0, 0, 1)      # 滚轮一格
```

### C/C++ 和 C#

包内 `sdk/examples/status.c` 和 `sdk/examples/Status.cs` 都是可运行的只读示例。从解压目录用 x64 Native Tools Command Prompt 编译 C 示例：

```cmd
cl /nologo /W4 /I. sdk\examples\status.c xiaoji_client.lib /Fe:status.exe
status.exe
```

C# 将 `Status.cs` 加入 x64 控制台项目，把 DLL 放在程序输出目录。示例的 `Check()` 读取原生错误；实际要点按 X 时调用 `Check(XiaojiTapKey(0, 0x1B))`。C/C++ 对应调用为 `XiaojiTapKey(0, 0x1B)`，返回非零时立即读取 `XiaojiLastError()`。

### Rust（Windows x64 MSVC）

SDK 附带零第三方依赖的 `xiaoji-client` 库（Rust 1.82+）。将整个 SDK 解压到应用的 `vendor/xiaoji/`，在应用的 `Cargo.toml` 中添加：

```toml
[dependencies]
xiaoji-client = { path = "vendor/xiaoji/sdk/rust" }
```

编译时需要 `xiaoji_client.lib`。保持 ZIP 原目录结构时 Cargo 会自动找到它；若直接引用源码仓库的 `sdk/rust/`，先运行 `.\scripts\build.cmd x64 sdk`，再从仓库根目录设置 `$env:XIAOJI_SDK_DIR = (Resolve-Path .\x64\sdk).Path`。运行和分发时，将同版 `xiaoji_client.dll` 放在**自己的 EXE 同目录**。不需要分发 `.lib` 或安装 Rust 工具链给最终用户。

```rust
use xiaoji_client::{Client, Error};

fn main() -> Result<(), Error> {
    let client = Client::new()?;
    let devices = client.devices()?;
    if devices.keyboard {
        println!("键盘已启动");
        // 可选：以管理员身份验证键盘通道，不发送输入。
        // client.check_connection(xiaoji_client::Device::Keyboard)?;
        // 仅在用户主动触发输入时调用：
        // client.tap_key(0, 0x1B)?;  // 点按 X 物理键
        // client.type_ascii("Hi!")?;
        // 或使用 ABI 2 会话，按下和松开是两个独立操作：
        // let mut session = xiaoji_client::Session::open(1, 1000, 200)?;
        // session.key(0x1B, true)?;
        // session.key(0x1B, false)?;
        // session.close()?;
    }
    if devices.mouse {
        println!("鼠标已启动");
        // client.move_mouse(12, -5, 0)?;
    }
    Ok(())
}
```

从**解压后的 SDK 根目录**可直接编译和运行只读示例（PowerShell）：

```powershell
cargo build --offline --locked --target x86_64-pc-windows-msvc --manifest-path .\sdk\rust\Cargo.toml --target-dir .\rust-target --example status
$bin = '.\rust-target\x86_64-pc-windows-msvc\debug\examples'
Copy-Item .\xiaoji_client.dll $bin
& "$bin\status.exe"
```

`Client::new()` 和 `Session::open()` 要求 ABI 2。操作返回 `Result`，原生错误会立即复制为 Rust 自有字符串。`Error::Unconfirmed` 与 `Error::Driver` 不同：前者表示已经发布，不能重试。`Session` 不能跨线程移动或共享；`Drop` 会关闭尚未关闭的会话，但不会发送松开报文。

## ABI 1 兼容接口（Windows x64）

| 接口 | 约定 |
| --- | --- |
| `XiaojiGetAbiVersion()` | 返回客户端 ABI 版本，不访问设备。 |
| `XiaojiGetDevices(&mask)` | 成功时 `mask` 的位 `1` 表示键盘，`2` 表示鼠标；`0` 表示两者均不存在，`3` 表示两者均已启动。节点存在但身份不符或启动异常时返回失败，而非报告未安装。 |
| `XiaojiCheckConnection(device)` | 传 `1`（键盘）或 `2`（鼠标），与发送操作使用同一权限、锁和共享内存检查，最多等待 10 秒让驱动确认通道就绪，不提交输入报告。适合第一次接入排错，不必每次发送前重复调用。 |
| `XiaojiTapKey(modifiers, usage)` | 自动按下再释放；`usage` 是 HID Keyboard/Keypad 页的 `0x04..0x65`，不是 Windows VK。`modifiers` 的位 0–3 为左 Ctrl/Shift/Alt/GUI，位 4–7 为右侧对应键。例：左 Shift + X 为 `(0x02, 0x1B)`。 |
| `XiaojiTypeAscii(text, byte_count)` | 1–256 字节；可打印 ASCII、LF 和 Tab。不传终止 NUL，不接受 CR、中文或 Unicode 文本。输入前校验整段；字符效果受目标键盘布局影响。 |
| `XiaojiMoveMouse(dx, dy, wheel)` | 三个有符号整数均限 `-127..127`；X/Y 是 HID 相对计数，不是屏幕像素；滚轮单位是格，不是 Windows 消息的 120。 |
| `XiaojiLastError()` | 当前线程最近一次操作的 UTF-8 错误文本，由 DLL 持有；下一次操作会覆盖。不要释放；需要保留时复制。 |

除版本和错误文本查询外，各接口同步返回 `0` 成功、`1` 参数错误、`2` 运行失败；Python 对后两种情况抛出 `ValueError` 或 `RuntimeError`。成功发送表示 HID 已确认报告，**不是**目标应用接收或处理的回执。不要在 UI 主线程等待输入；多线程或多进程同时发送可能因宿主操作锁被占用而失败，应串行调用。报错或超时可能已发送部分报告，**不要自动重发**。停用设备后应提示用户通过管理器检查设备或签名，不要在输入循环中重装驱动、导入证书或删除设备。

## ABI 2 会话

需要分开发送按下和松开、同时按住多个键，或判断失败时是否已经发布输入时，使用以下同步 C 接口。`XiaojiSession` 是不透明句柄，所有操作（包括关闭）都必须在打开线程执行。

| 函数 | 参数与作用 |
| --- | --- |
| `XiaojiSessionOpen(devices, ready_timeout_ms, ack_timeout_ms, &session)` | `devices` 为 `1` 键盘、`2` 鼠标、`3` 两者；等待驱动就绪 `1–10000` 毫秒，每帧等待确认 `1–2000` 毫秒。输出指针不得为空；失败时将输出句柄置空。打开不发帧。 |
| `XiaojiSessionKey(session, usage, down)` | HID Keyboard/Keypad 用法，**不是 Windows VK**。普通键 `0x04–0x9F`，修饰键 `0xE0–0xE7`。`down` 非零为按下，零为松开。 |
| `XiaojiSessionButton(session, button, down)` | `1–5` 依次为左、右、中、按键 4、按键 5；`down` 与键盘相同。 |
| `XiaojiSessionWheel(session, detents)` | `−127..127` 且非零，单位为格，不是 Windows 消息的 120。整次滚动只发一帧，保留按键位，X、Y 为零。 |
| `XiaojiSessionClose(session)` | 释放句柄、共享对象和互斥体，不写帧，**不松开按键**。请先自行发布需要的松开操作；成功关闭后不要再使用该句柄。 |

| 返回码 | 调用方可以确定什么 |
| --- | --- |
| `0` `XIAOJI_OK` | 操作成功，每个已发布帧均已确认。HID 类取走报文并发起下一次 `READ_REPORT` 时，驱动才写确认序号；这不表示目标应用已经处理输入。 |
| `1` `XIAOJI_INVALID_ARGUMENT` | 没有发布任何帧，状态不变。检查指针、取值、设备掩码和线程。从其他线程关闭也返回 `1`，会话仍然打开。 |
| `2` `XIAOJI_FAILED` | 没有发布任何帧，状态不变。例如没有权限、锁被占用、布局不符或驱动未就绪。 |
| `3` `XIAOJI_UNCONFIRMED` | 序号已发布，但通知失败、等待失败或确认超时。设备**可能已经动作，不得重试这次操作**，尤其不能重发滚轮；下一次独立操作以这份已发布状态为基础。 |

非零返回后，在当前线程立即读取 `XiaojiLastError()`。上述「`2` 表示没有发布」只适用于会话函数；ABI 1 函数保留原语义，永远不返回 `3`。

打开要求调用进程已经具有管理员权限，且所选设备已安装、已启动、能确认输入，并实际运行新版布局（键盘 `nkro-28`、鼠标 5 键）。检测到旧布局，请先用新版 `xiaoji.exe` 执行 `install`；无法确定布局时也拒绝打开，不尝试降级。每进程最多一个会话，持有 `Global\XiaojiHostOperation` 直到关闭。会话打开期间，其他进程中需要该锁的操作（包括 `xiaoji.exe install`、`xiaoji.exe uninstall`、GUI 和 `xiaoji-host.exe` 的输入命令）都会因锁被占用而失败，应先关闭会话。同进程的 `XiaojiTapKey`、`XiaojiTypeAscii`、`XiaojiMoveMouse`、`XiaojiCheckConnection` 此时返回 `2`；只读 `XiaojiGetDevices` 不受影响。

**每帧都是对应设备的整份按住状态**。键盘会话只用修饰键字节和 160 位位图，六键数组保持为零。重复按下已按住的键、松开未按住的键也会发布新序号并等待确认，不会被当作空操作。鼠标按键帧不携带位移或滚轮；滚轮增量只属于当前帧。

本版未接入驱动的 owner pipe 崩溃释放机制。进程异常退出后，按住的键在对应设备下一帧送达前一直保持，Windows 可能自动重复。仅重新打开会话不会清理残留；新会话从全松开状态构建其第一帧，该帧会清除同一设备上未被重新按住的键。键盘帧不会清除鼠标按键，反之亦然。
