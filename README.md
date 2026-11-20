# XiaojiDriver

xiaoji 是基于 [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) 的 Windows x64 用户态 HID 驱动，提供持久的虚拟键盘与鼠标。用轻量命令行 `xiaoji.exe` 管理设备，业务程序通过客户端 SDK 发送输入；两者都不依赖 GUI。

## 命令行管理

独立 CLI 没有内嵌驱动、签名工具或图形界面。首次使用时，准备受目标电脑信任的签名包，在管理员终端安装，再检查设备状态：

```powershell
.\scripts\build.cmd x64 cli
cd .\x64\cli
.\xiaoji.exe install 'D:\已签名驱动包\xiaoji.inf'
.\xiaoji.exe status
.\xiaoji.exe verify
```

签名包须包含 INF、DLL 和 CAT，CLI 不替包签名。卸载前可用 `uninstall --dry-run` 预览，再在管理员终端执行 `uninstall`；`uninstall --keep-package` 只移除设备及配置。完整参数、退出码和本地测试签名方式见[命令行指南](docs/cli.md)。

新版设备的键盘报文可同时表示全部支持的按键，鼠标支持 5 键；已安装的机器需用新版 CLI 再执行一次 `install <已签名 INF>` 升级布局。现有 ABI 1 输入接口不变。

## 构建和启动

编译驱动需要 MSVC x64 工具和配套的 Windows SDK / WDK（含 UMDF 2.15）。在仓库根目录运行：

```powershell
.\scripts\build.cmd x64 driver
```

驱动 DLL 和 INF 输出到 `x64/driver/`，不构建宿主、安装器或管理器。追加 `package` 可用 Inf2Cat 同时生成未签名的 `xiaoji.cat`。

只有构建含可选 GUI 和本地签名工具的完整版才需要 .NET Framework 4.8、SignTool 和 Inf2Cat：

```powershell
.\scripts\build.cmd x64 package
.\build\xiaoji-gui.exe
```

`build/` 下的 `xiaoji.inf` 是安装说明，`xiaoji.dll` 是用户态驱动，`xiaoji.cat` 是文件校验目录。**构建生成的 DLL 和 CAT 尚未签名**；光是打开管理器不会更改驱动或证书库。内嵌 SDK/WDK 工具的 GUI 和本地签名宿主不作为公开 Release 附件。

## 第三方程序调用

使用 [x64 客户端 SDK](docs/developer.md)，无需启动 GUI。驱动在目标机上安装一次；第三方程序加载的是 **`xiaoji_client.dll`**，而不是驱动 `xiaoji.dll`。

```powershell
.\scripts\build.cmd x64 sdk
python .\x64\sdk\xiaoji.py
```

第二行只查询已启动的键盘和鼠标，不发送输入。SDK 提供设备查询、按键点按（含修饰键）、ASCII 输入、相对移动和滚轮；发送输入需要管理员权限。Rust 项目可直接引用 SDK 中的 [`xiaoji-client` Cargo 库](sdk/rust/)。[开发者接入指南](docs/developer.md)提供 Rust、C/C++、C# 和 Python 示例及错误处理说明。

ABI 2 会话可分开发送按下和松开、同时按住多个键或鼠标按键，并区分「没有发布」与「已发布但未确认」；后者不得重试。

## 可选图形界面

| 操作 | 实际效果 |
| --- | --- |
| 离线自检 | 检查内嵌包，不安装设备、不发送输入。 |
| 安装驱动 | 需要管理员权限。首次确认整机证书信任后，安装器签名并暂存驱动包，创建或复用固定的键盘和鼠标设备。关闭管理器不会移除它们。 |
| 验证安装 | 验签 Windows Driver Store 中实际发布的包，显示已启动的设备数；若存在当前构建的签名缓存，再比较 INF、DLL、CAT 的摘要。「与当前构建不同」不等于签名无效，包已暂存也不等于设备已启动。 |
| 键盘、鼠标、按键连发 | 点击即向当前桌面发送真实输入，不弹确认框。连发可选字母、数字或空格，并设定次数和间隔；点「停止连发」或关闭窗口时，会尝试释放正在按下的键。完成次数指 HID 已确认的点按，不代表前台应用已收到。 |
| 卸载驱动 | 显式确认后移除本项目的固定设备及配置，并删除与安装记录匹配的 OEM 包。不强删其他设备占用的包，也不移除测试证书或签名缓存。 |

**签名仅用于你控制的测试机。** 安装器用本机的专用测试证书签 DLL，重新生成并签 CAT。证书公钥会加入整机的 `LocalMachine\Root` 和 `LocalMachine\TrustedPublisher`；这不是 Microsoft 正式签名，也不会启用 `TESTSIGNING`。Windows 分配的 `oem*.inf` 名称因机器而异，不能仅凭编号判断归属。撤销测试信任和更新驱动版本的步骤见 [签名与安装详解](docs/xiaoji.md#测试机本地签名)。

## 验证与源码

从仓库根目录运行以下检查，无需安装驱动，不会向桌面发送输入：

```powershell
.\scripts\xiaoji_setup_smoke.ps1
powershell.exe -NoProfile -STA -File .\scripts\xiaoji_gui_smoke.ps1
```

管理器窗口位于 [`app/`](app/)，驱动和 INF 位于 [`driver/`](driver/)，原生宿主与本地签名安装器位于 [`host/`](host/)。命令行诊断和 Raw Input 观察程序位于 [`tests/`](tests/)，构建和验证脚本位于 [`scripts/`](scripts/)。协议和命令行参数见 [xiaoji 技术说明](docs/xiaoji.md)。

## 发布未签名预览包

推送到 `main`、`feature/**` 或向 `main` 提交 PR 时，GitHub Actions 在 Windows x64 上编译驱动、生成 CAT 并校验 ZIP 内容。普通提交和不指定标签的手动运行只生成 Actions 构件，不创建 Release。

发布标签和附件名使用仓库根目录 [`VERSION`](VERSION) 中的三段产品版本号。在已进入 `main` 的提交上创建对应标签，例如 `git tag v0.1.0 <提交哈希>`，再运行 `git push origin refs/tags/v0.1.0`。工作流验证通过后才创建 GitHub **Pre-release**，不会覆盖已发布版本。

发布任务在同一台 Windows runner 上从标签源码构建、测试、核对 SHA-256 并直接上传附件，无需跨任务下载构件。如果标签已推送但尚未发布，可从 `main` 执行 `gh workflow run driver-release.yml --ref main -f release_tag=v0.1.0` 来使用最新流程重跑；打包的源码仍是原标签，不需要删除或移动标签。

Windows 驱动单独使用四段版本号：产品 `0.1.0` 包含的驱动版本为 `1.0.0.1`。打包时仍会检查 `driver/xiaoji.inf` 的 `DriverVer` 与驱动 DLL 文件版本一致。

发布附件分开提供：`xiaoji-cli-<版本>-win-x64.zip` 是独立管理命令；`xiaoji-driver-<版本>-win-x64-unsigned.zip` 是未签名驱动包；`xiaoji-client-sdk-<版本>-win-x64.zip` 是第三方调用库和示例。`SHA256SUMS.txt` 列出附件摘要，三者都不带 GUI、证书或微软 SDK/WDK 工具。**未签名驱动包不是可直接安装的正式驱动。**

仓库根许可证为 [AGPLv3](LICENSE)。派生驱动保留 HIDMaestro 的 [MIT 许可证](third_party/HIDMaestro/LICENSE)及[上游版本与归属声明](third_party/HIDMaestro/NOTICE.md)。
