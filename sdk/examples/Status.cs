using System;
using System.Runtime.InteropServices;
using System.Text;

[assembly: DefaultDllImportSearchPaths(DllImportSearchPath.AssemblyDirectory | DllImportSearchPath.System32)]

class Status
{
    const string Library = "xiaoji_client.dll";
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    static extern uint XiaojiGetAbiVersion();
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    static extern int XiaojiGetDevices(out uint devices);
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    static extern IntPtr XiaojiLastError();
    [DllImport(Library, CallingConvention = CallingConvention.Cdecl)]
    internal static extern int XiaojiTapKey(byte modifiers, byte usage);

    internal static void Check(int result)
    {
        if (result == 0) return;
        IntPtr error = XiaojiLastError();
        int length = 0;
        while (Marshal.ReadByte(error, length) != 0) ++length;
        var bytes = new byte[length];
        Marshal.Copy(error, bytes, 0, length);
        throw new InvalidOperationException(Encoding.UTF8.GetString(bytes));
    }

    static void Main()
    {
        if (XiaojiGetAbiVersion() != 2) throw new NotSupportedException("Expected xiaoji client ABI 2");
        uint devices;
        Check(XiaojiGetDevices(out devices));
        Console.WriteLine("keyboard={0} mouse={1}", (devices & 1) != 0, (devices & 2) != 0);
    }
}
