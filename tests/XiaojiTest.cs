using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

internal static class XiaojiTest
{
    internal const string Keyboard = @"ROOT\XIAOJI\KBD0001", Mouse = @"ROOT\XIAOJI\MOU0001";
    const uint NoSuchDevnode = 0x0d;

    [StructLayout(LayoutKind.Sequential)]
    internal struct PropertyKey {
        public Guid format;
        public uint id;
        public PropertyKey(string format, uint id) { this.format = new Guid(format); this.id = id; }
    }

    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    internal static extern uint CM_Locate_DevNodeW(out uint node, string instance, uint flags);
    [DllImport("cfgmgr32.dll")]
    static extern uint CM_Get_DevNode_Status(out uint status, out uint problem, uint node, uint flags);
    [DllImport("cfgmgr32.dll", ExactSpelling = true)]
    internal static extern uint CM_Get_DevNode_PropertyW(uint node, ref PropertyKey key, out uint type,
        [Out] byte[] data, ref uint size, uint flags);

    internal static string ReadString(uint result, uint type, byte[] bytes, uint size, string operation)
    {
        if (result != 0) throw new IOException(String.Format("{0}: CONFIGRET=0x{1:X}", operation, result));
        if (type != 18 || size < 2 || size > bytes.Length || size % 2 != 0 || bytes[size-1] != 0 || bytes[size-2] != 0)
            throw new InvalidDataException(operation + " 未返回有效的设备属性字符串。");
        return Encoding.Unicode.GetString(bytes, 0, (int)size - 2);
    }

    internal static HashSet<string> FindPackages(string[] receipt)
    {
        return new HashSet<string>(DriverStore.ExactPackages(receipt).Select(package => package.PublishedName), StringComparer.OrdinalIgnoreCase);
    }

    internal static int Devices(HashSet<string> expected)
    {
        int present = 0;
        foreach (string instance in new[] { Keyboard, Mouse }) {
            uint node, status, problem;
            uint result = CM_Locate_DevNodeW(out node, instance, 0);
            if (result == NoSuchDevnode) { Console.WriteLine("[未连接] " + instance); continue; }
            if (result == 0) result = CM_Get_DevNode_Status(out status, out problem, node, 0);
            else throw new IOException(String.Format("{0}: CM_Locate_DevNode=0x{1:X}", instance, result));
            if (result != 0) throw new IOException(String.Format("{0}: CM_Get_DevNode_Status=0x{1:X}", instance, result));
            Console.WriteLine("{0}: status=0x{1:X}, problem={2}", instance, status, problem);
            if ((status & 8) == 0 || (status & 0x400) != 0 || problem != 0) throw new IOException("设备未正常启动：" + instance);
            var infKey = new PropertyKey("a8b865dd-2e3d-4094-ad97-e593a70c75d6", 5);
            byte[] data = new byte[520];
            uint size = (uint)data.Length, type;
            result = CM_Get_DevNode_PropertyW(node, ref infKey, out type, data, ref size, 0);
            string bound = ReadString(result, type, data, size, "DriverInfPath");
            Console.WriteLine("[已启动] 绑定 INF：" + bound);
            if (expected != null && !expected.Contains(Path.GetFileName(bound))) throw new IOException("此设备未绑定本次验签的驱动包。");
            ++present;
        }
        return present;
    }

    internal static Dictionary<string, string[]> VerifiedBindings(string tools, string package)
    {
        var receipts = new Dictionary<string, string[]>(StringComparer.OrdinalIgnoreCase);
        foreach (string instance in new[] { Keyboard, Mouse }) {
            uint node, type;
            uint rc = CM_Locate_DevNodeW(out node, instance, 0);
            if (rc == NoSuchDevnode) continue;
            if (rc != 0) throw new IOException("无法读取固定设备节点：" + instance + "，CONFIGRET=" + rc);
            var key = new PropertyKey("a8b865dd-2e3d-4094-ad97-e593a70c75d6", 5);
            byte[] data = new byte[520];
            uint size = (uint)data.Length;
            rc = CM_Get_DevNode_PropertyW(node, ref key, out type, data, ref size, 0);
            if (rc == 0x25) continue;
            string name = ReadString(rc, type, data, size, "DriverInfPath");
            if (receipts.ContainsKey(name)) continue;
            receipts.Add(name, VerifyPublished(tools, DriverStore.FromBinding(name, Path.Combine(package, "xiaoji.inf"))));
        }
        return receipts;
    }

    static string[] VerifyPublished(string tools, DriverStore.Package package)
    {
        var hashes = LocalSetup.VerifyPublishedFiles(tools, package.Folder);
        Console.WriteLine("[已验签] Driver Store：{0}（DriverVer={1}）。", package.PublishedName, package.Version);
        return hashes;
    }

    internal static string VerifyInstalled()
    {
        using (var payload = new LocalSetup.EmbeddedPackage()) {
            var actual = VerifiedBindings(payload.Tools, payload.Package);
            if (actual.Count == 0) {
                foreach (var item in DriverStore.RelatedPackages(Path.Combine(payload.Package, "xiaoji.inf")))
                    actual.Add(item.PublishedName, VerifyPublished(payload.Tools, item));
            }
            if (actual.Count == 0) throw new InvalidOperationException("Driver Store 未找到已发布的 xiaoji 驱动包。签名缓存不代表已安装。");
            int started = Devices(new HashSet<string>(actual.Keys, StringComparer.OrdinalIgnoreCase));
            string deviceState = started == 0 ? "已暂存 · 无活动设备" : String.Format("已安装 · {0}/2 个设备已启动", started);
            string cache = LocalSetup.CheckLocalCache(payload.Tools, payload.Package, allowMissing: true);
            if (cache == null) {
                Console.WriteLine("[未比对] 没有本构建的签名缓存；实际发布包已验签，未确认与本构建逐字节一致。");
                return deviceState + " · 未比对当前构建";
            }
            string[] expected = DriverStore.HashesIn(cache);
            bool allExact = true;
            foreach (var entry in actual) {
                var different = LocalSetup.PackageFiles.Where((name, index) => entry.Value[index] != expected[index]).ToArray();
                if (different.Length == 0) Console.WriteLine("[同一构建] {0} 的 INF、DLL、CAT 摘要均与本构建一致。", entry.Key);
                else {
                    allExact = false;
                    Console.WriteLine("[不同构建] {0} 已验签，但与本构建不同的文件：{1}。", entry.Key, String.Join("、", different));
                }
            }
            return deviceState + (allExact ? " · 与当前构建完全一致" : " · 与当前构建不同");
        }
    }

    static void Usage()
    {
        Console.WriteLine("xiaoji-test-cli.exe [offline | verify | devices | install-test | observe-keyboard | observe-mouse]");
        Console.WriteLine("默认 offline 只做离线自测。verify 核验本地自签安装；devices 查询在线节点。\n" +
            "install-test 需要管理员交互确认，实际更改系统。observe-* 只在前台观察目标输入。");
    }

    internal static void CheckReceiptRules()
    {
        var receipt = new InputObserver.KeyboardReceipt();
        var keyboard = new IntPtr(1);
        if (receipt.Accept(keyboard, 0x58, 1) || receipt.Accept(keyboard, 0x58, 0) ||
            receipt.Accept(new IntPtr(2), 0x58, 1) || receipt.Accept(keyboard, 0x59, 1) ||
            !receipt.Accept(keyboard, 0x58, 1) || receipt.Accept(keyboard, 0x58, 1))
            throw new InvalidDataException("键盘按下/释放判定自测失败。");
        if (InputObserver.IsRelativeMovement(0, 0, 0) || InputObserver.IsRelativeMovement(1, 10, -1) ||
            !InputObserver.IsRelativeMovement(0, 0, -1) || !InputObserver.IsRelativeMovement(0, 10, 0))
            throw new InvalidDataException("相对鼠标位移判定自测失败。");
        Console.WriteLine("[离线通过] 键鼠报告判定规则；未读取或产生真实输入。");
    }

    [STAThread]
    public static int Main(string[] args)
    {
        try {
            if (args.Length > 1) { Usage(); return 2; }
            switch (args.Length == 0 ? "offline" : args[0]) {
            case "offline": CheckReceiptRules(); return LocalSetup.Main(new[] { "--check" });
            case "verify": Console.WriteLine(VerifyInstalled()); return 0;
            case "devices": return Devices(null) == 0 ? 3 : 0;
            case "install-test":
                Console.WriteLine("仅限隔离测试机：此操作会调用真实安装器，可能修改整机证书信任和 Driver Store。请输入 INSTALL 继续：");
                if (Console.IsInputRedirected || Console.ReadLine() != "INSTALL") { Console.WriteLine("已取消，未启动安装器。"); return 2; }
                if (LocalSetup.Main(new[] { "install" }) != 0) return 1;
                Console.WriteLine(VerifyInstalled()); return 0;
            case "observe-keyboard": case "observe-mouse":
                Application.EnableVisualStyles();
                using (var window = new InputObserver(args[0] == "observe-keyboard")) {
                    Application.Run(window);
                    return window.Result;
                }
            case "--help": Usage(); return 0;
            default: Usage(); return 2;
            }
        } catch (Exception error) {
            Console.Error.WriteLine("[失败] " + error.Message);
            return 1;
        }
    }
}
