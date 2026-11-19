# xiaoji 驱动

xiaoji 是基于 [HIDMaestro](https://github.com/hifihedgehog/HIDMaestro) 的 Windows x64 用户态 HID 驱动。安装后，Windows 会识别一个虚拟键盘和一个虚拟鼠标。测试台通过宿主把报告写入共享内存；`xiaoji.dll` 在 UMDF 2.15 下运行，通过系统的 `mshidumdf` 把报告交给 Windows HID 输入栈。

## 构建和启动

编译驱动需要 MSVC x64 工具和配套的 Windows SDK / WDK（含 UMDF 2.15）。在仓库根目录运行：

```powershell
.\scripts\build.cmd x64 driver
```

驱动 DLL 和 INF 输出到 `x64/driver/`，不构建宿主、安装器或测试台。追加 `package` 可用 Inf2Cat 同时生成未签名的 `xiaoji.cat`。

构建并运行完整测试台还需要 .NET Framework 4.8、SignTool 和 Inf2Cat：

```powershell
.\scripts\build.cmd x64 package
.\build\xiaoji-test.exe
```

`build/` 下的 `xiaoji.inf` 是安装说明，`xiaoji.dll` 是用户态驱动，`xiaoji.cat` 是文件校验目录。**这里的 DLL 和 CAT 尚未签名**；构建不会安装驱动或修改证书库。中文测试台 `xiaoji-test.exe` 已内嵌驱动、宿主和所需工具，可单独在装有 .NET Framework 4.8 的 x64 Windows 测试机上运行。

## 用测试台操作设备

| 操作 | 实际效果 |
| --- | --- |
| 离线自检 | 检查内嵌包，不安装设备、不发送输入。 |
| 安装驱动 | 需要管理员权限。首次确认整机证书信任后，安装器签名并暂存驱动包，创建或复用固定的键盘和鼠标设备。关闭测试台不会移除它们。 |
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

驱动和 INF 位于 [`driver/`](driver/)，原生宿主与本地签名安装器位于 [`host/`](host/)，GUI 和命令行测试程序源码位于 [`tests/`](tests/)。构建和验证脚本统一放在 [`scripts/`](scripts/)。设备协议、命令行参数和 Raw Input 验证方法见 [xiaoji 技术说明](docs/xiaoji.md)。

## 发布未签名预览包

推送到 `main`、`feature/**` 或向 `main` 提交 PR 时，GitHub Actions 在 Windows x64 上编译驱动、生成 CAT 并校验 ZIP 内容。普通提交和手动运行只生成 Actions 构件，不创建 Release。

发布时，在已进入 `main` 的提交上创建并推送四段版本标签。标签须与 `driver/xiaoji.inf` 的 `DriverVer` 和 DLL 文件版本一致。例如，版本 `1.0.0.1` 的发布命令是 `git tag v1.0.0.1 <提交哈希>`，再运行 `git push origin refs/tags/v1.0.0.1`。工作流验证通过后才创建 GitHub **Pre-release**，不会覆盖已发布版本。

附件为 `xiaoji-driver-<版本>-win-x64-unsigned.zip` 和 `SHA256SUMS.txt`。ZIP 仅含同次构建的 INF、DLL、CAT 及许可证和上游声明；不含安装器、GUI、证书或 SDK/WDK 工具。**这是未签名的开发者包，不是可直接安装的正式驱动。**

仓库根许可证为 [AGPLv3](LICENSE)。派生驱动保留 HIDMaestro 的 [MIT 许可证](third_party/HIDMaestro/LICENSE)及[上游版本与归属声明](third_party/HIDMaestro/NOTICE.md)。
