using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

internal static class DriverStore
{
    const string InfName = "xiaoji.inf";
    static readonly Regex OemName = new Regex(@"\Aoem[0-9]+\.inf\z", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);
    static readonly Regex VersionLine = new Regex(@"^[ \t]*DriverVer[ \t]*=[ \t]*([0-9/]+[ \t]*,[ \t]*[0-9.]+)[ \t]*$",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant | RegexOptions.Multiline);

    internal sealed class Package
    {
        internal readonly string PublishedName, InfPath;
        internal string Folder { get { return Path.GetDirectoryName(InfPath); } }
        internal string Version { get { return VersionLine.Match(ReadInf(InfPath)).Groups[1].Value; } }
        internal string[] Hashes() { return HashesIn(Folder); }

        internal Package(string publishedName, string infPath)
        {
            PublishedName = publishedName;
            InfPath = infPath;
        }
    }

    [DllImport("setupapi.dll", CharSet = CharSet.Unicode, ExactSpelling = true, SetLastError = true)]
    static extern bool SetupGetInfDriverStoreLocationW(string publishedInf, IntPtr platform, IntPtr locale,
        StringBuilder result, uint capacity, out uint required);

    internal static string[] HashesIn(string folder)
    {
        return LocalSetup.PackageFiles.Select(name => LocalSetup.FileHash(Path.Combine(folder, name))).ToArray();
    }

    static string ReadInf(string path) { return File.ReadAllText(path).Replace("\r\n", "\n"); }

    internal static bool SameFamily(string candidate, string expected)
    {
        var candidateInf = ReadInf(candidate);
        var expectedInf = ReadInf(expected);
        return candidateInf == expectedInf || (VersionLine.Matches(candidateInf).Count == 1 && VersionLine.Matches(expectedInf).Count == 1 &&
            VersionLine.Replace(candidateInf, "DriverVer=") == VersionLine.Replace(expectedInf, "DriverVer="));
    }

    static Package Resolve(string name)
    {
        if (!OemName.IsMatch(name)) throw new InvalidDataException("OEM INF 名称不符合预期：" + name);
        var location = new StringBuilder(260);
        uint required;
        if (!SetupGetInfDriverStoreLocationW(name, IntPtr.Zero, IntPtr.Zero, location, 260, out required))
            throw new Win32Exception(Marshal.GetLastWin32Error(), "无法解析已发布的 INF：" + name);
        return new Package(name, location.ToString());
    }

    internal static Package FromBinding(string name, string templateInf)
    {
        var package = Resolve(name);
        if (!IsXiaoji(package) || !SameFamily(package.InfPath, templateInf))
            throw new InvalidDataException("固定节点绑定的不是预期的 xiaoji INF：" + name);
        return package;
    }

    static bool IsXiaoji(Package package)
    {
        return String.Equals(Path.GetFileName(package.InfPath), InfName, StringComparison.OrdinalIgnoreCase);
    }

    static IEnumerable<Package> Find(Func<string, bool> matchesInf)
    {
        string folder = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Windows), "INF");
        foreach (string file in Directory.EnumerateFiles(folder, "oem*.inf")) {
            string name = Path.GetFileName(file);
            if (!OemName.IsMatch(name) || !matchesInf(file)) continue;
            var package = Resolve(name);
            if (IsXiaoji(package) && matchesInf(package.InfPath)) yield return package;
        }
    }

    internal static IEnumerable<Package> RelatedPackages(string templateInf)
    {
        return Find(path => SameFamily(path, templateInf));
    }

    static IEnumerable<Package> WithInfHash(string hash)
    {
        return Find(path => String.Equals(LocalSetup.FileHash(path), hash, StringComparison.OrdinalIgnoreCase));
    }

    internal static IEnumerable<Package> ExactPackages(string[] hashes)
    {
        return WithInfHash(hashes[0]).Where(package => package.Hashes().SequenceEqual(hashes, StringComparer.OrdinalIgnoreCase));
    }

    internal static Package ResolveStaged(string[] expectedHashes)
    {
        var candidates = WithInfHash(expectedHashes[0]).OrderBy(package => package.PublishedName, StringComparer.OrdinalIgnoreCase).ToArray();
        var exact = candidates.FirstOrDefault(package => package.Hashes().SequenceEqual(expectedHashes, StringComparer.OrdinalIgnoreCase));
        if (exact != null) return exact;
        if (candidates.Length == 1) return candidates[0];
        if (candidates.Length == 0) throw new IOException("暂存命令已返回，但未找到对应的已发布 xiaoji INF；未更新安装记录。");
        throw new IOException("多个已发布包使用同一份 xiaoji INF，且都与候选包不同；未自动选择或更新安装记录。");
    }
}
