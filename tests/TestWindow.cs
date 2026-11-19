using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;

internal sealed class TestWindow : Form
{
    readonly FlowLayoutPanel actions = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true, WrapContents = true };
    readonly FlowLayoutPanel inputs = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true };
    readonly FlowLayoutPanel repeatOptions = new FlowLayoutPanel { AutoSize = true, Margin = Padding.Empty };
    readonly ComboBox repeatKey = new ComboBox { Name = "RepeatKey", AccessibleName = "连发按键", DropDownStyle = ComboBoxStyle.DropDownList, Width = 76 };
    readonly NumericUpDown repeatCount = new NumericUpDown { Name = "RepeatCount", AccessibleName = "连发次数", Minimum = 1, Maximum = 1000, Value = 10, Width = 74 };
    readonly NumericUpDown repeatGap = new NumericUpDown { Name = "RepeatGap", AccessibleName = "释放后间隔（毫秒）", Minimum = 10, Maximum = 5000, Value = 100, Width = 74 };
    readonly Button stopRepeat = new Button { Name = "RepeatStop", Text = "停止连发", Enabled = false, AutoSize = true };
    EventWaitHandle repeatStopEvent;
    bool closeAfterRepeat;
    readonly Label status = new Label { Name = "Status", AutoSize = true, Text = "就绪 · 尚未执行测试", Margin = new Padding(0, 5, 0, 7) };
    readonly TextBox log = new TextBox { Name = "Log", Multiline = true, ReadOnly = true, Dock = DockStyle.Fill,
        ScrollBars = ScrollBars.Vertical, WordWrap = true, BackColor = Color.White, AccessibleName = "测试记录" };
    readonly bool admin = LocalSetup.IsAdministrator();
    bool busy;
    LocalSetup.EmbeddedPackage runtime;
    string installedNames;

    public TestWindow()
    {
        Text = "xiaoji 驱动测试台";
        Font = new Font("Microsoft YaHei UI", 10);
        AutoScaleMode = AutoScaleMode.Font;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(820, 610);
        MinimumSize = new Size(720, 530);
        BackColor = Color.FromArgb(246, 248, 250);
        var grid = new TableLayoutPanel { Dock = DockStyle.Fill, Padding = new Padding(22), ColumnCount = 1, RowCount = 10 };
        grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        for (int row = 0; row < grid.RowCount; ++row)
            grid.RowStyles.Add(row == 9 ? new RowStyle(SizeType.Percent, 100) : new RowStyle(SizeType.AutoSize));
        Controls.Add(grid);
        var title = new Label { Text = Text, AutoSize = true, Font = new Font(Font.FontFamily, 20, FontStyle.Bold), Margin = Padding.Empty };
        grid.Controls.Add(title, 0, 0);
        grid.Controls.Add(new Label { Text = "UMDF2 虚拟 HID    /    x64    /    " + (admin ? "管理员模式" : "普通权限"),
            AutoSize = true, ForeColor = Color.FromArgb(61, 91, 115), Margin = new Padding(1, 3, 0, 14) }, 0, 1);
        grid.Controls.Add(new Label { Text = "驱动与签名", AutoSize = true, Margin = new Padding(0, 4, 0, 3) }, 0, 2);
        grid.Controls.Add(actions, 0, 3);
        AddButton(actions, "离线自检", "Offline", delegate { return RunWork("离线自检", () => {
            XiaojiTest.CheckReceiptRules(); LocalSetup.RunSetup(true); return "通过 · 未执行安装"; }); });
        AddButton(actions, "设备状态", "Devices", delegate { return RunWork("设备状态", () => {
            return XiaojiTest.Devices(null) == 0 ? "未连接 · 当前没有在线的 xiaoji 设备" : "已检测到在线设备"; }); });
        AddButton(actions, "验证安装", "Verify", () => RunWork("验证安装", XiaojiTest.VerifyInstalled));
        AddButton(actions, "安装驱动…", "Install", Install);
        AddButton(actions, "卸载驱动…", "Uninstall", Uninstall);
        if (!admin) AddButton(actions, "以管理员身份重开", "Elevate", Elevate);

        grid.Controls.Add(new Label { Text = "真实输入测试", AutoSize = true, Margin = new Padding(0, 14, 0, 3) }, 0, 4);
        grid.Controls.Add(inputs, 0, 5);
        AddButton(inputs, "键盘：点按 X", "Keyboard", () => SendInput(true));
        AddButton(inputs, "鼠标：向右移动", "Mouse", () => SendInput(false));
        grid.Controls.Add(new Label { Text = "按键连发测试", AutoSize = true, Margin = new Padding(0, 12, 0, 3) }, 0, 6);
        var repeatRow = new FlowLayoutPanel { Dock = DockStyle.Fill, AutoSize = true };
        grid.Controls.Add(repeatRow, 0, 7);
        repeatRow.Controls.Add(repeatOptions);
        foreach (char key in "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890") repeatKey.Items.Add(key.ToString());
        repeatKey.Items.Add("空格"); repeatKey.SelectedItem = "X";
        AddRepeatField("按键", repeatKey); AddRepeatField("次数", repeatCount); AddRepeatField("间隔 (ms)", repeatGap);
        AddButton(repeatOptions, "开始连发", "RepeatStart", StartRepeat);
        repeatRow.Controls.Add(stopRepeat);
        stopRepeat.Click += (sender, args) => StopRepeat();
        grid.Controls.Add(status, 0, 8);
        grid.Controls.Add(log, 0, 9);
        Append("建议先点「离线自检」。打开本窗口不会签名、安装驱动或发送输入。");
        Console.SetOut(new WindowWriter(this));
        Console.SetError(Console.Out);
    }

    void AddRepeatField(string caption, Control field)
    {
        repeatOptions.Controls.Add(new Label { Text = caption, AutoSize = true, Margin = new Padding(2, 8, 3, 2) });
        repeatOptions.Controls.Add(field);
    }

    void AddButton(FlowLayoutPanel panel, string text, string name, Func<Task> action)
    {
        var button = new Button { Text = text, Name = name, AccessibleName = text, AutoSize = true,
            AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(8, 4, 8, 4),
            Margin = new Padding(0, 3, 8, 3), UseVisualStyleBackColor = true };
        panel.Controls.Add(button);
        button.Click += async (sender, args) => {
            if (busy) return;
            busy = true;
            actions.Enabled = false;
            inputs.Enabled = false;
            repeatOptions.Enabled = false;
            try { await action(); }
            catch (OperationCanceledException error) { Report("已取消", error.Message); }
            catch (Exception error) { closeAfterRepeat = false; Report("未通过", error.Message); }
            finally {
                busy = false;
                if (!IsDisposed) {
                    actions.Enabled = true; inputs.Enabled = true; repeatOptions.Enabled = true;
                    if (closeAfterRepeat) { closeAfterRepeat = false; BeginInvoke(new Action(Close)); }
                }
            }
        };
    }

    void Append(string line)
    {
        if (IsDisposed || Disposing || String.IsNullOrWhiteSpace(line)) return;
        if (InvokeRequired) { BeginInvoke(new Action<string>(Append), line); return; }
        if (log.TextLength > 64000) log.Text = "[已裁剪较早记录]\r\n" + log.Text.Substring(log.TextLength - 32000);
        log.AppendText(DateTime.Now.ToString("HH:mm:ss") + "  " + line.Replace("\r\n", "\n").Replace("\n", "\r\n") + "\r\n");
    }

    void Report(string state, string detail)
    {
        status.Text = state;
        status.ForeColor = state == "未通过" ? Color.Firebrick : Color.FromArgb(32, 79, 88);
        Append(detail);
    }

    async Task RunWork(string title, Func<string> work)
    {
        Report("正在" + title + "…", "开始：" + title);
        string result = await Task.Run(work);
        Report(result, title + "：" + result);
    }

    bool Confirm(string title, string detail)
    {
        return MessageBox.Show(this, detail, title, MessageBoxButtons.YesNo, MessageBoxIcon.Warning,
            MessageBoxDefaultButton.Button2) == DialogResult.Yes;
    }

    void RequireAdmin()
    {
        if (!admin) throw new UnauthorizedAccessException("请先点「以管理员身份重开」，再选择此操作。");
    }

    Task Elevate()
    {
        try {
            using (Process.Start(new ProcessStartInfo(Application.ExecutablePath) { UseShellExecute = true, Verb = "runas" })) { }
            BeginInvoke(new Action(Close));
        } catch (Win32Exception error) {
            if (error.NativeErrorCode != 1223) throw;
            Report("已取消", "已取消 Windows 提权，当前窗口仍可使用。");
        }
        return Task.FromResult(0);
    }

    Task Install()
    {
        RequireAdmin();
        if (!Confirm("安装 xiaoji 驱动？", "将在本机安装驱动包，并创建或复用 xiaoji 键盘和鼠标。关闭窗口不会移除设备。继续吗？"))
            throw new OperationCanceledException("未启动安装器。");
        return RunWork("安装和验证", () => {
            using (LifecycleLock()) {
                installedNames = null;
                var prior = XiaojiTest.VerifiedBindings(Runtime.Tools, Runtime.Package);
                var staged = LocalSetup.RunSetup(false, info => (bool)Invoke(new Func<bool>(() => Confirm("信任本机测试证书？",
                    info + "\n\n将向本机 Root 和 TrustedPublisher 添加证书。这会让整台电脑信任此发布者，不只信任本驱动。继续吗？"))));
                var names = staged.Published.PublishedName.ToLowerInvariant();
                var packages = new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase) { names };
                Native("ensure-devices " + names + " " + (prior.Count == 0 ? "none" : String.Join(",", prior.Keys).ToLowerInvariant()));
                if (XiaojiTest.Devices(packages) != 2)
                    throw new IOException("安装未完成：未能确认两个设备都已启动。可重试安装或卸载。");
                installedNames = names;
                return staged.IsExactBuild ? "已安装 · 键盘和鼠标已启动" : "已安装 · 两个设备已启动，复用已验签的不同构建";
            }
        });
    }

    static FileStream LifecycleLock()
    {
        var path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "xiaoji-device-lifecycle.lock");
        return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 4096, FileOptions.DeleteOnClose);
    }

    LocalSetup.EmbeddedPackage Runtime { get { return runtime ?? (runtime = new LocalSetup.EmbeddedPackage()); } }

    string VerifiedNames()
    {
        var bindings = XiaojiTest.VerifiedBindings(Runtime.Tools, Runtime.Package);
        if (bindings.Count == 0) throw new InvalidOperationException("未找到已安装的 xiaoji 设备，请先点「安装驱动」。");
        return String.Join(",", bindings.Keys).ToLowerInvariant();
    }

    void Native(string args)
    {
        LocalSetup.Tool(Path.Combine(Runtime.Tools, "xiaoji-host.exe"), args, Runtime.Tools);
    }

    Task SendInput(bool keyboard)
    {
        RequireAdmin();
        return RunWork(keyboard ? "点按 X" : "向右移动", () => {
            using (LifecycleLock()) {
                if (installedNames == null) installedNames = VerifiedNames();
                Native((keyboard ? "send-key " : "send-mouse ") + installedNames);
            }
            return "已发送 · HID 已确认报告";
        });
    }

    void StopRepeat()
    {
        if (repeatStopEvent == null || !stopRepeat.Enabled) return;
        repeatStopEvent.Set();
        stopRepeat.Enabled = false;
        Report("正在停止连发…", "已请求停止，等待宿主完成当前按键的释放。");
    }

    async Task StartRepeat()
    {
        RequireAdmin();
        int selection = repeatKey.SelectedIndex;
        if (selection < 0) throw new InvalidOperationException("请选择连发按键。");
        int usage = selection == 36 ? 0x2c : 4 + selection;
        int count = (int)repeatCount.Value, gap = (int)repeatGap.Value;
        string key = repeatKey.Text, token = Guid.NewGuid().ToString("N");
        bool created;
        using (var stop = new EventWaitHandle(false, EventResetMode.ManualReset, @"Local\XiaojiRepeatStop-" + token, out created)) {
            if (!created) throw new IOException("连发停止事件名称冲突，未开始发送。");
            repeatStopEvent = stop;
            log.Focus();
            stopRepeat.Enabled = true;
            try {
                await RunWork(String.Format("连发 {0}（{1} 次，释放后间隔 {2} ms）", key, count, gap), () => {
                    int confirmed = 0;
                    string terminal = null;
                    using (LifecycleLock()) {
                        if (installedNames == null && !stop.WaitOne(0)) installedNames = VerifiedNames();
                        if (stop.WaitOne(0)) return String.Format("已停止 · 0/{0} 次", count);
                        using (var owner = Process.GetCurrentProcess()) {
                            string args = String.Format(CultureInfo.InvariantCulture, "repeat-key {0} {1} {2} {3} {4} {5}",
                                installedNames, usage, count, gap, token, owner.Id);
                            try {
                                LocalSetup.Tool(Path.Combine(Runtime.Tools, "xiaoji-host.exe"), args, Runtime.Tools, line => {
                                    string[] fields = line.Split(' ');
                                    int done;
                                    if (fields.Length != 2 || !Int32.TryParse(fields[1], NumberStyles.None, CultureInfo.InvariantCulture, out done) ||
                                        done < confirmed || done > count) return;
                                    if (fields[0] == "FINISHED" || fields[0] == "STOPPED") terminal = fields[0];
                                    else if (fields[0] != "PROGRESS") return;
                                    confirmed = done;
                                    if (terminal == null) BeginInvoke(new Action(() => {
                                        if (repeatStopEvent == stop && stopRepeat.Enabled) status.Text = String.Format("正在连发 {0} · {1}/{2} 次", key, done, count);
                                    }));
                                });
                            } catch (Exception error) {
                                throw new IOException(String.Format("连发中断，已确认完整点按 {0}/{1} 次。{2}", confirmed, count, error.Message), error);
                            }
                        }
                    }
                    if (terminal != "STOPPED" && (terminal != "FINISHED" || confirmed != count))
                        throw new IOException("未收到完整的连发结果，不能判定成功。");
                    return String.Format("{0} · {1} 键已确认 {2}/{3} 次", terminal == "STOPPED" ? "已停止" : "已完成", key, confirmed, count);
                });
            } finally {
                repeatStopEvent = null;
                stopRepeat.Enabled = false;
            }
        }
    }

    Task Uninstall()
    {
        RequireAdmin();
        if (!Confirm("卸载 xiaoji 驱动？", "移除 xiaoji 键盘、鼠标及对应的已发布驱动包。不会强制删除其他设备在用的包；本地测试证书和签名缓存保留。"))
            throw new OperationCanceledException("未执行卸载。");
        return RunWork("卸载驱动", () => {
            using (LifecycleLock()) {
                installedNames = null;
                var hashes = LocalSetup.RemovalReceipt();
                var names = hashes == null ? new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase) : XiaojiTest.FindPackages(hashes);
                if (names.Count == 0) {
                    var bindings = XiaojiTest.VerifiedBindings(Runtime.Tools, Runtime.Package);
                    if (bindings.Count > 0) {
                        hashes = bindings.Values.First();
                        if (bindings.Values.Any(value => !value.SequenceEqual(hashes))) throw new IOException("旧节点绑定了不同的包；请先安装统一版本再卸载。");
                        names = XiaojiTest.FindPackages(hashes);
                        if (!bindings.Keys.All(names.Contains)) throw new IOException("设备绑定的包在卸载准备期间发生变化。");
                        Console.WriteLine("按实际已验签的设备绑定修正卸载目标：" + String.Join(", ", bindings.Keys));
                    } else {
                        if (hashes == null) {
                            var cache = LocalSetup.CheckLocalCache(Runtime.Tools, Runtime.Package, allowMissing: true);
                            if (cache != null) {
                                hashes = DriverStore.HashesIn(cache);
                                names = XiaojiTest.FindPackages(hashes);
                            }
                        }
                        if (names.Count == 0 && DriverStore.RelatedPackages(Path.Combine(Runtime.Package, "xiaoji.inf")).Any())
                            throw new IOException("存在已发布的 xiaoji 包，但没有匹配的安装记录或设备绑定；未自动删除历史包。");
                    }
                }
                var knownNames = String.Join(",", names).ToLowerInvariant();
                if (names.Count != 0) LocalSetup.SaveRemovalReceipt(hashes);
                Native("remove-devices " + (knownNames.Length == 0 ? "none" : knownNames));
                foreach (var name in names) {
                    if (!XiaojiTest.FindPackages(hashes).Contains(name)) throw new IOException("驱动包在卸载时发生变化：" + name);
                    LocalSetup.Tool(Path.Combine(Environment.SystemDirectory, "pnputil.exe"), "/delete-driver " + name, Runtime.Tools);
                }
                if ((hashes != null && XiaojiTest.FindPackages(hashes).Count != 0) || XiaojiTest.Devices(null) != 0)
                    throw new IOException("卸载尚未完成，请关闭占用设备的程序后重试。");
                return "已卸载 · 两个节点和对应的驱动包均不在系统中";
            }
        });
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing && runtime != null) { runtime.Dispose(); runtime = null; }
        base.Dispose(disposing);
    }

    protected override void OnFormClosing(FormClosingEventArgs args)
    {
        if (busy) {
            args.Cancel = true;
            if (repeatStopEvent != null) { closeAfterRepeat = true; StopRepeat(); }
            else Append("当前操作尚未结束，请完成或取消确认后再关闭。");
        }
        base.OnFormClosing(args);
    }

    sealed class WindowWriter : TextWriter
    {
        readonly TestWindow window;
        public WindowWriter(TestWindow window) { this.window = window; }
        public override Encoding Encoding { get { return Encoding.UTF8; } }
        public override void WriteLine(string line) { window.Append(line); }
    }

    [STAThread]
    public static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        using (var window = new TestWindow()) Application.Run(window);
    }
}
