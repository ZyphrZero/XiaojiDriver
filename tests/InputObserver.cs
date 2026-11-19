using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Windows.Forms;

internal sealed class InputObserver : Form
{
    const uint Failed = UInt32.MaxValue;
    [StructLayout(LayoutKind.Sequential)]
    struct Registration { public ushort page, usage; public uint flags; public IntPtr target; }
    [StructLayout(LayoutKind.Sequential)]
    struct Header { public uint type, size; public IntPtr device, parameter; }
    static readonly uint HeaderSize = (uint)Marshal.SizeOf(typeof(Header));

    [DllImport("user32.dll", SetLastError = true)]
    static extern bool RegisterRawInputDevices(Registration[] devices, uint count, uint size);
    [DllImport("user32.dll", EntryPoint = "GetRawInputData", SetLastError = true)]
    static extern uint GetHeader(IntPtr input, uint command, out Header header, ref uint size, uint headerSize);
    [DllImport("user32.dll", SetLastError = true)]
    static extern uint GetRawInputData(IntPtr input, uint command, [Out] byte[] data, ref uint size, uint headerSize);
    [DllImport("user32.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    static extern uint GetRawInputDeviceInfoW(IntPtr device, uint command, StringBuilder name, ref uint size);
    [DllImport("cfgmgr32.dll", CharSet = CharSet.Unicode, ExactSpelling = true)]
    static extern uint CM_Get_Device_Interface_PropertyW(string path, ref XiaojiTest.PropertyKey key,
        out uint type, [Out] byte[] data, ref uint size, uint flags);

    readonly bool keyboard;
    readonly Timer timeout = new Timer { Interval = 30000 };
    readonly KeyboardReceipt receipt = new KeyboardReceipt();
    bool finished;
    public int Result { get; private set; }
    public InputObserver(bool keyboard)
    {
        this.keyboard = keyboard;
        Result = 3;
        Text = keyboard ? "xiaoji 键盘报告测试" : "xiaoji 鼠标报告测试";
        Width = 540; Height = 155; StartPosition = FormStartPosition.CenterScreen;
        Controls.Add(new Label { Dock = DockStyle.Fill, Padding = new Padding(12), Text =
            "请让本窗口保持在前台。30 秒内等待 xiaoji 虚拟设备" +
            (keyboard ? "按下并释放 X 键。" : "发送一次非零相对移动。") +
            "\r\n\r\n本窗口只观察，不发送输入。物理键鼠的操作不计入结果。" });
        timeout.Tick += (sender, args) => Finish(3, "超时；没有观察到完整的目标输入。");
    }

    protected override void OnShown(EventArgs args)
    {
        base.OnShown(args);
        var registration = new Registration { page = 1, usage = (ushort)(keyboard ? 6 : 2), target = Handle };
        if (!RegisterRawInputDevices(new[] { registration }, 1, (uint)Marshal.SizeOf(typeof(Registration)))) {
            Finish(1, new Win32Exception(Marshal.GetLastWin32Error()).Message);
            return;
        }
        timeout.Start();
    }

    void Finish(int result, string detail)
    {
        if (finished) return;
        finished = true; Result = result; timeout.Stop();
        Console.WriteLine((result == 0 ? "[通过] " : "[未通过] ") + detail);
        BeginInvoke(new Action(Close));
    }

    bool FromTarget(IntPtr device)
    {
        if (device == IntPtr.Zero) return false;
        uint chars = 0;
        if (GetRawInputDeviceInfoW(device, 0x20000007, null, ref chars) == Failed) throw new Win32Exception(Marshal.GetLastWin32Error());
        if (chars == 0 || chars > 32768) throw new InvalidDataException("无效的 Raw Input 设备路径长度。");
        var path = new StringBuilder((int)chars);
        if (GetRawInputDeviceInfoW(device, 0x20000007, path, ref chars) == Failed) throw new Win32Exception(Marshal.GetLastWin32Error());
        var instanceKey = new XiaojiTest.PropertyKey("78c34fc8-104a-4aca-9ea4-524d52996e57", 256);
        byte[] bytes = new byte[1024];
        uint type, node, size = (uint)bytes.Length;
        uint result = CM_Get_Device_Interface_PropertyW(path.ToString(), ref instanceKey, out type, bytes, ref size, 0);
        if (result == 0x37 || result == 0x0d) return false;
        string instance = XiaojiTest.ReadString(result, type, bytes, size, "Device_InstanceId");
        result = XiaojiTest.CM_Locate_DevNodeW(out node, instance, 0);
        if (result == 0x0d) return false;
        if (result != 0) throw new IOException("CM_Locate_DevNode: " + result);
        var parentKey = new XiaojiTest.PropertyKey("4340a6c5-93fa-4706-972c-7b648008a5a7", 8);
        size = (uint)bytes.Length;
        result = XiaojiTest.CM_Get_DevNode_PropertyW(node, ref parentKey, out type, bytes, ref size, 0);
        if (result == 0x0d || result == 0x25) return false;
        return String.Equals(XiaojiTest.ReadString(result, type, bytes, size, "Device_Parent"),
                             keyboard ? XiaojiTest.Keyboard : XiaojiTest.Mouse, StringComparison.OrdinalIgnoreCase);
    }

    void ReadReport(IntPtr input)
    {
        Header header;
        uint size = HeaderSize;
        if (GetHeader(input, 0x10000005, out header, ref size, HeaderSize) != HeaderSize)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        if (header.type != (keyboard ? 1u : 0u) || !FromTarget(header.device)) return;
        size = header.size;
        if (size < HeaderSize + (keyboard ? 16u : 24u) || size > 4096) throw new InvalidDataException("Raw Input 报告长度异常。");
        var bytes = new byte[size];
        if (GetRawInputData(input, 0x10000003, bytes, ref size, HeaderSize) != header.size)
            throw new Win32Exception(Marshal.GetLastWin32Error());
        int offset = (int)HeaderSize;
        if (keyboard) {
            if (receipt.Accept(header.device, BitConverter.ToUInt16(bytes, offset + 6), BitConverter.ToUInt16(bytes, offset + 2)))
                Finish(0, "从 xiaoji 键盘收到了 X 的按下和释放。");
        } else {
            int dx = BitConverter.ToInt32(bytes, offset + 12), dy = BitConverter.ToInt32(bytes, offset + 16);
            if (IsRelativeMovement(BitConverter.ToUInt16(bytes, offset), dx, dy))
                Finish(0, String.Format("从 xiaoji 鼠标收到相对位移：({0}, {1})。", dx, dy));
        }
    }

    internal sealed class KeyboardReceipt
    {
        IntPtr pressedOn;
        internal bool Accept(IntPtr device, ushort virtualKey, ushort flags)
        {
            if (device == IntPtr.Zero || virtualKey != 0x58) return false;
            if ((flags & 1) == 0) { pressedOn = device; return false; }
            if (pressedOn != device) return false;
            pressedOn = IntPtr.Zero;
            return true;
        }
    }

    internal static bool IsRelativeMovement(ushort flags, int dx, int dy)
    {
        return (flags & 1) == 0 && (dx != 0 || dy != 0);
    }

    protected override void WndProc(ref Message message)
    {
        if (!finished && message.Msg == 0x00ff && message.WParam == IntPtr.Zero) {
            try { ReadReport(message.LParam); }
            catch (Exception error) { Finish(1, error.Message); }
        }
        base.WndProc(ref message);
    }

    protected override void OnFormClosed(FormClosedEventArgs args)
    {
        timeout.Dispose();
        if (!finished) Console.WriteLine("[未完成] 窗口已关闭；未确认输入。");
        base.OnFormClosed(args);
    }
}
