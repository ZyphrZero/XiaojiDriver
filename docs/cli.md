# xiaoji 命令行管理

`xiaoji.exe` 是 Windows x64 原生命令行程序，可以单文件运行，不依赖 GUI、客户端 DLL、.NET、SignTool 或 WDK。独立 CLI 附件 `xiaoji-cli-<版本>-win-x64.zip` 不包含驱动包。源码构建命令是 `scripts\build.cmd x64 cli`，默认输出 `x64\cli\xiaoji.exe`。

以下示例在 `xiaoji.exe` 所在目录的 PowerShell 执行。**安装和卸载**需要管理员终端；帮助、状态、验签和卸载预览只读，不会自动提权。

```powershell
.\xiaoji.exe --help
.\xiaoji.exe verify 'D:\驱动包\xiaoji.inf'
.\xiaoji.exe install 'D:\驱动包\xiaoji.inf'
.\xiaoji.exe status
.\xiaoji.exe verify
.\xiaoji.exe uninstall --dry-run
.\xiaoji.exe uninstall
```

- **首次安装：** 将同包的 `xiaoji.inf`、`xiaoji.dll`、`xiaoji.cat` 放在一起。`verify <INF>` 只读检查包的标识、目录签名和成员，不表示已安装。`install <INF>` 进行同样的检查，通过 Windows SetupAPI 暂存，核对实际发布的文件，再创建两个固定设备。如果 Windows 复用了同 INF 但内容不同的已暂存包，不会继续绑定。此默认路径不生成证书、不修改信任、不启用测试模式，不能直接使用未签名包。
- **升级已有设备：** 用新版 CLI 再执行一次 `install <INF>`。安装器核对绑定包和控制器归属，更新旧描述符并重启相应设备，期间该设备会短暂离线；已使用新布局的设备无需重启。安装器从运行中的 HID 子设备确认升级结果，需要重启计算机或未能确认时会报错。
- **检查安装：** `status` 对缺失设备只显示 `keyboard: not present` 或 `mouse: not present`；对已启动设备显示 OEM INF 和运行中的 HID 布局，不检查签名。不带参数的 `verify` 还会核验已启动设备实际绑定包的 CAT 签名，并检查 INF、DLL 的摘要。新键盘显示 `layout: nkro-28`，新鼠标显示 `buttons: 5`；旧版分别显示 `layout: legacy-8` 和 `buttons: 3`，并提示运行 `install`。两台设备均已启动且其余检查通过时，即使使用旧布局，两个命令仍以 `0` 退出。若任一已启动设备的布局无法确定，`status` 和无参数 `verify` 会显示 `unknown` 和原因，并以 `2` 退出。
- **本地开发自签：** 如果只有源码和未签名包，在自己控制的测试机上做完整构建，然后在管理员终端运行 `xiaoji-host.exe install`。首次建立整机测试证书信任需要交互确认。该命令只负责本地签名和暂存；将它输出的已签名 INF 路径传给 `xiaoji.exe install` 即可完成设备安装。该开发工具不属于独立 CLI 附件。
- **卸载：** 建议先运行 `uninstall --dry-run` 查看当前绑定包。`uninstall` 仅移除本项目的固定节点和所属配置，对刚确认绑定的包使用非强制删除。要保留包以便重装，改用 `uninstall --keep-package`。没有固定设备绑定时不猜测、不清理历史包。若包仍被其他设备占用，命令报错并报告保留下来的包名；不会强删，也不会移除证书或签名缓存。
- **脚本退出码：** `0` 成功；`1` 用法错误；`2` 权限、身份、验签、PnP 或其他操作错误（包括已启动设备的布局为 `unknown`）；`3` 表示 `status` 或无参数 `verify` 发现至少一个设备缺失，且无其他错误。用 `$LASTEXITCODE` 判断，不必解析本地化错误文字。

需要图形管理界面时可使用 `xiaoji-gui.exe`。第三方应用通过 `xiaoji_client.dll` 发送输入，不需要常驻的管理进程。

## 供程序调用（JSON）

```text
xiaoji.exe status --json
xiaoji.exe install --local-sign <驱动包目录> --trust-local-certificate --json
xiaoji.exe uninstall --json [--remove-local-trust]
```

宿主按参数分别传值，目录不是 INF 文件名。查询不要求管理员；安装、升级、卸载从宿主继承管理员权限，不弹 UAC、不读取 stdin。stdout 是 **UTF-8、无 BOM、每行一个 JSON 对象**；stderr 为空。按 `type` 和枚举字段分支，不要解析面向人的 `text` / `detail`。

- `status` 只返回一行，例如 `{"type":"status","state":"needs_upgrade","keyboard":"legacy","mouse":"current","elevated":true,"detail":"keyboard: legacy-8; mouse: 5"}`。`elevated` 表示当前进程是否有管理员权限。键鼠各自为 `absent`（无实例）、`current`、`legacy`（已启动的新/旧布局）、`not_started` 或 `unknown`（身份或运行布局无法确认）。
- 两者均不存在为 `not_installed`；均已启动且均为新布局是 `ready`；均已启动且布局已识别、至少一个旧布局是 `needs_upgrade`；其他组合为 `broken`。上述均退出 **0**，`detail` 说明两台设备的观测结果。查询本身失败时仅输出 `{"type":"error","detail":"..."}`，退出 **2**。
- `install --local-sign` 使用构建时生成的 CAT，不在目标机重建目录。**调用方必须先取得用户对整机信任的明确确认**，才能传 `--trust-local-certificate`；它授权将证书加入本机 Root 和 TrustedPublisher，而不只是信任这份驱动。缺少开关时在加锁、创建文件或证书前返回错误。宿主应从可信渠道验证插件包来源，并保护提权执行的 EXE 和包；INF 身份与包内哈希不能证明发布者身份。
- 安装按阶段输出 `{"type":"progress","step":"sign","text":"Sign and verify the package"}`。`step` 依次为 `verify`（包身份和内容）、`certificate`、`sign`、`stage`（Windows 实际发布包）、`devices`（包括旧布局迁移）、`confirm`。只有最终 `{"type":"result","state":"ready","detail":"..."}` 且退出 **0** 才表示两台设备的新布局均已确认。
- `uninstall --json` 删除固定设备和所属配置，仅对确认绑定的包请求非强制删除。正常阶段是 `verify`、`devices`、`stage`、`confirm`。加 `--remove-local-trust` 才会在确认没有设备或已发布的 xiaoji 包之后进入 `certificate`，清理 CLI 有记录的缓存和与 GUI 共用的证书、私钥及签名记录。不会按名称批量删除证书或强删历史包。成功终行为 `{"type":"result","state":"not_installed","detail":"..."}`，退出 **0**。
- 安装/卸载参数、权限、锁或操作失败：最后一行 `{"type":"error","step":"<当前阶段>","detail":"..."}`，退出 **2**；可能尚未输出任何进度行。锁忙时先停止输入会话、关闭正在安装或卸载的程序，再重试。失败不等于所有先前步骤已回滚；按 `detail` 检查保留的包、证书或缓存。
