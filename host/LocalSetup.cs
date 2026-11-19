using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.AccessControl;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Security.Principal;
using Microsoft.Win32;

internal static class LocalSetup
{
    const string Subject = "CN=xiaoji UMDF2 Local Test";
    const string Settings = @"SOFTWARE\XiaojiUmdf2LocalSigning";
    const string Inf = "xiaoji.inf", Dll = "xiaoji.dll", Cat = "xiaoji.cat";
    internal static readonly string[] PackageFiles = { Inf, Dll, Cat };

    internal static string FileHash(string path)
    {
        using (var hash = SHA256.Create())
        using (var file = File.OpenRead(path)) return BitConverter.ToString(hash.ComputeHash(file)).Replace("-", "");
    }

    internal static string[] RemovalReceipt()
    {
        var hashes = Registry.GetValue(@"HKEY_LOCAL_MACHINE\" + Settings, "InstalledHashes", null);
        if (hashes == null) return null;
        var values = hashes as string[];
        if (values == null || values.Length != PackageFiles.Length || values.Any(value => value.Length != 64 || !value.All(Uri.IsHexDigit)))
            throw new InvalidDataException("本机驱动包安装记录无效，未执行卸载。");
        return values;
    }

    internal static void SaveRemovalReceipt(string[] hashes)
    {
        using (var settings = Registry.LocalMachine.CreateSubKey(Settings))
            settings.SetValue("InstalledHashes", hashes, RegistryValueKind.MultiString);
    }

    internal static void Tool(string file, string args, string work, Action<string> output = null)
    {
        var start = new ProcessStartInfo(file, args) { WorkingDirectory = work, UseShellExecute = false,
            CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        start.EnvironmentVariables["TEMP"] = start.EnvironmentVariables["TMP"] = work;
        using (var process = Process.Start(start)) {
            process.OutputDataReceived += (sender, line) => {
                if (line.Data == null) return;
                if (output == null) Console.WriteLine(line.Data); else output(line.Data);
            };
            process.ErrorDataReceived += (sender, line) => { if (line.Data != null) Console.Error.WriteLine(line.Data); };
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
            process.WaitForExit();
            if (process.ExitCode != 0)
                throw new InvalidOperationException(Path.GetFileName(file) + " exit=" + process.ExitCode +
                    (process.ExitCode == 3010 ? " (Windows requests a restart)" : ""));
        }
    }

    static string Quote(string path) { return "\"" + path + "\""; }

    static void CreateWorkDirectory(string path, bool elevated)
    {
        if (Directory.Exists(path) || File.Exists(path)) throw new IOException("目标路径已存在：" + path);
        if (elevated) {
            var acl = new DirectorySecurity();
            acl.SetSecurityDescriptorSddlForm("O:BAG:BAD:P(A;OICI;FA;;;BA)(A;OICI;FA;;;SY)");
            Directory.CreateDirectory(path, acl);
        } else Directory.CreateDirectory(path);
    }

    static void Extract(string tools, string package)
    {
        Directory.CreateDirectory(tools);
        CreateWorkDirectory(package, IsAdministrator());
        var assembly = Assembly.GetExecutingAssembly();
        foreach (var name in assembly.GetManifestResourceNames()) {
            if (Path.GetFileName(name) != name) throw new InvalidDataException(name);
            using (var input = assembly.GetManifestResourceStream(name))
            using (var output = new FileStream(Path.Combine(name == Inf || name == Dll ? package : tools, name),
                                               FileMode.CreateNew, FileAccess.Write, FileShare.None))
                input.CopyTo(output);
        }
    }

    static X509Certificate2 Find(StoreName name, string thumbprint)
    {
        using (var store = new X509Store(name, StoreLocation.LocalMachine)) {
            store.Open(OpenFlags.ReadOnly | OpenFlags.OpenExistingOnly);
            var certs = store.Certificates.Find(X509FindType.FindByThumbprint, thumbprint, false);
            return certs.Count == 0 ? null : certs[0];
        }
    }

    static void Add(StoreName name, X509Certificate2 cert)
    {
        using (var store = new X509Store(name, StoreLocation.LocalMachine)) {
            store.Open(OpenFlags.ReadWrite);
            store.Add(cert);
        }
    }

    static bool IsTrusted(string thumbprint)
    {
        using (var root = Find(StoreName.Root, thumbprint))
        using (var publisher = Find(StoreName.TrustedPublisher, thumbprint))
            return root != null && publisher != null;
    }

    static X509Certificate2 CreateSigner()
    {
        var options = new CngKeyCreationParameters {
            Provider = CngProvider.MicrosoftSoftwareKeyStorageProvider,
            KeyCreationOptions = CngKeyCreationOptions.MachineKey,
            ExportPolicy = CngExportPolicies.None, KeyUsage = CngKeyUsages.Signing
        };
        options.Parameters.Add(new CngProperty("Length", BitConverter.GetBytes(3072), CngPropertyOptions.None));
        using (var key = CngKey.Create(CngAlgorithm.Rsa, "xiaoji-" + Guid.NewGuid().ToString("N"), options)) {
            try {
                var acl = new RawSecurityDescriptor("O:BAG:BAD:P(A;;GA;;;BA)(A;;GA;;;SY)");
                var encodedAcl = new byte[acl.BinaryLength];
                acl.GetBinaryForm(encodedAcl, 0);
                key.SetProperty(new CngProperty("Security Descr", encodedAcl,
                    (CngPropertyOptions)(SecurityInfos.Owner | SecurityInfos.DiscretionaryAcl)));
                using (var rsa = new RSACng(key)) {
                    var request = new CertificateRequest(Subject, rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
                    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
                    request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature, true));
                    request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(
                        new OidCollection { new Oid("1.3.6.1.5.5.7.3.3") }, true));
                    var cert = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddYears(1));
                    Add(StoreName.My, cert);
                    return cert;
                }
            } catch { key.Delete(); throw; }
        }
    }

    static X509Certificate2 Signer(Func<string, bool> confirmTrust)
    {
        string thumbprint = Registry.GetValue(@"HKEY_LOCAL_MACHINE\" + Settings, "Thumbprint", null) as string;
        X509Certificate2 cert = thumbprint == null ? null : Find(StoreName.My, thumbprint);
        if (thumbprint != null && (cert == null || cert.Subject != Subject || !cert.HasPrivateKey ||
            DateTime.Now < cert.NotBefore || DateTime.Now >= cert.NotAfter))
            throw new InvalidOperationException("记录的 xiaoji 测试证书丢失、无私钥或已过期；请核对指纹：" + thumbprint);
        if (cert != null && IsTrusted(thumbprint))
            return cert;

        Console.WriteLine("仅用于测试。这会让整台电脑信任 xiaoji 的代码签名证书（不只信任本驱动）。");
        Console.WriteLine("证书：{0}；指纹：{1}。不更改 Secure Boot 或 TESTSIGNING。", Subject, thumbprint ?? "首次生成");
        bool approved;
        if (confirmTrust != null) approved = confirmTrust(Subject + "\n指纹：" + (thumbprint ?? "首次生成"));
        else {
            Console.Write("确认更改 LocalMachine\\Root 和 TrustedPublisher？请输入 TRUST，其他输入取消：");
            approved = !Console.IsInputRedirected && Console.ReadLine() == "TRUST";
        }
        if (!approved)
            throw new OperationCanceledException("未获得交互确认；未创建证书或更改信任。");
        if (cert == null) {
            cert = CreateSigner();
            using (var settings = Registry.LocalMachine.CreateSubKey(Settings))
                settings.SetValue("Thumbprint", cert.Thumbprint, RegistryValueKind.String);
        }
        using (var publicCert = new X509Certificate2(cert.RawData)) {
            Add(StoreName.Root, publicCert);
            Add(StoreName.TrustedPublisher, publicCert);
        }
        Console.WriteLine("已信任本机测试证书：" + cert.Thumbprint);
        return cert;
    }

    static string CachePath(string package, X509Certificate2 signer)
    {
        using (var digest = SHA256.Create()) {
            var bytes = digest.ComputeHash(File.ReadAllBytes(Path.Combine(package, Inf)))
                .Concat(digest.ComputeHash(File.ReadAllBytes(Path.Combine(package, Dll))))
                .Concat(signer.GetCertHash()).ToArray();
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles),
                "xiaoji-umdf2-" + BitConverter.ToString(digest.ComputeHash(bytes)).Replace("-", ""));
        }
    }

    static void Verify(string sign, string package, string thumbprint, string work)
    {
        string verify = "verify /pa /sha1 " + thumbprint + " ";
        string cat = Quote(Path.Combine(package, Cat));
        Tool(sign, verify + Quote(Path.Combine(package, Dll)), work);
        Tool(sign, verify + cat, work);
        foreach (string file in new[] { Inf, Dll })
            Tool(sign, verify + "/c " + cat + " " + Quote(Path.Combine(package, file)), work);
    }

    static void Catalog(string tools, string package)
    {
        Tool(Path.Combine(tools, "Inf2Cat.exe"), "/driver:" + Quote(package) + " /os:10_X64", tools);
        if (!File.Exists(Path.Combine(package, Cat))) throw new InvalidDataException("Inf2Cat 未生成 xiaoji.cat");
    }

    static void VerifyCache(string tools, string cache, string thumbprint)
    {
        foreach (string path in new[] { cache, Path.Combine(cache, Dll), Path.Combine(cache, Inf), Path.Combine(cache, Cat) })
            if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("拒绝缓存中的重解析点：" + path);
        Verify(Path.Combine(tools, "signtool.exe"), cache, thumbprint, tools);
    }

    internal static string CheckLocalCache(string tools, string package, bool allowMissing = false)
    {
        var thumbprint = Registry.GetValue(@"HKEY_LOCAL_MACHINE\" + Settings, "Thumbprint", null) as string;
        if (String.IsNullOrEmpty(thumbprint)) throw new InvalidOperationException("没有 xiaoji 本地签名指纹记录。请先在测试机安装此构建。");
        using (var cert = Find(StoreName.Root, thumbprint)) {
            if (cert == null || cert.Subject != Subject || !IsTrusted(thumbprint))
                throw new InvalidOperationException("本机未完整信任所记录的 xiaoji 证书：" + thumbprint);
            string cache = CachePath(package, cert);
            if (allowMissing) {
                try { File.GetAttributes(cache); }
                catch (FileNotFoundException) { return null; }
                catch (DirectoryNotFoundException) { return null; }
            }
            Console.WriteLine("证书指纹：{0}\n本构建的缓存：{1}", thumbprint, cache);
            VerifyCache(tools, cache, cert.Thumbprint);
            return cache;
        }
    }

    internal static string[] VerifyPublishedFiles(string tools, string package)
    {
        var thumbprint = Registry.GetValue(@"HKEY_LOCAL_MACHINE\" + Settings, "Thumbprint", null) as string;
        if (String.IsNullOrEmpty(thumbprint) || !IsTrusted(thumbprint)) throw new InvalidDataException("未确认已发布包来自本机信任的 xiaoji 发布者。");
        using (var signer = Find(StoreName.Root, thumbprint)) {
            if (signer == null || signer.Subject != Subject) throw new InvalidDataException("本机记录的证书不是 xiaoji 测试证书。");
            string[] hashes = DriverStore.HashesIn(package);
            VerifyCache(tools, package, signer.Thumbprint);
            if (!DriverStore.HashesIn(package).SequenceEqual(hashes)) throw new IOException("已发布包在验签期间发生变化。");
            return hashes;
        }
    }

    internal sealed class StagedPackage
    {
        internal readonly DriverStore.Package Published;
        internal readonly bool IsExactBuild;
        internal readonly string[] Hashes;

        internal StagedPackage(DriverStore.Package published, string[] hashes, bool isExactBuild)
        {
            Published = published;
            Hashes = hashes;
            IsExactBuild = isExactBuild;
        }
    }

    internal static StagedPackage ResolveImport(string tools, string cache)
    {
        var expected = DriverStore.HashesIn(cache);
        var published = DriverStore.ResolveStaged(expected);
        string[] actual = VerifyPublishedFiles(tools, published.Folder);
        if (actual[0] != expected[0]) throw new IOException("已发布 INF 与刚暂存的 INF 不同。");
        return new StagedPackage(published, actual, actual.SequenceEqual(expected));
    }

    static StagedPackage Run(bool check, string tools, string package, Func<string, bool> confirmTrust)
    {
        string sign = Path.Combine(tools, "signtool.exe");
        if (check) {
            Tool(sign, "verify /pa " + Quote(sign), tools);
            Catalog(tools, package);
            Console.WriteLine("xiaoji: 离线打包检查通过；未签名、未修改信任、未安装驱动。");
            return null;
        }
        string programFiles = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        using (var installLock = new FileStream(Path.Combine(programFiles, "xiaoji-umdf2-install.lock"),
                    FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None, 4096, FileOptions.DeleteOnClose))
        using (var cert = Signer(confirmTrust)) {
            string cache = CachePath(package, cert);
            if (!Directory.Exists(cache)) {
                string args = "sign /sm /s My /sha1 " + cert.Thumbprint + " /fd SHA256 ";
                Tool(sign, args + Quote(Path.Combine(package, Dll)), tools);
                Catalog(tools, package);
                Tool(sign, args + Quote(Path.Combine(package, Cat)), tools);
                Verify(sign, package, cert.Thumbprint, tools);
                Directory.Move(package, cache);
            } else {
                VerifyCache(tools, cache, cert.Thumbprint);
            }
            string inf = Path.Combine(cache, Inf);
            Tool(Path.Combine(Environment.SystemDirectory, "pnputil.exe"), "/add-driver " + Quote(inf), tools);
            var staged = ResolveImport(tools, cache);
            SaveRemovalReceipt(staged.Hashes);
            Console.WriteLine("实际发布包：{0}（DriverVer={1}）。", staged.Published.PublishedName, staged.Published.Version);
            if (!staged.IsExactBuild)
                Console.WriteLine("[复用] Windows 保留了同一 INF 的已验签包；DLL 或 CAT 与当前构建不同。若需要替换驱动，请更新 DriverVer 后重新构建并签名。");
            Console.WriteLine("已验签的 Driver Store INF：\n" + staged.Published.InfPath);
            return staged;
        }
    }

    internal static bool IsAdministrator()
    {
        using (var identity = WindowsIdentity.GetCurrent())
            return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    internal sealed class EmbeddedPackage : IDisposable
    {
        readonly string work;
        internal readonly string Tools, Package;
        internal EmbeddedPackage()
        {
            bool elevated = IsAdministrator();
            string parent = elevated ? Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles) : Path.GetTempPath();
            work = Path.Combine(parent, "xiaoji-session-" + Guid.NewGuid().ToString("N"));
            Tools = Path.Combine(work, "tools"); Package = Path.Combine(work, "package");
            CreateWorkDirectory(work, elevated);
            try { Extract(Tools, Package); } catch { Dispose(); throw; }
        }
        public void Dispose() { Directory.Delete(work, true); }
    }

    internal static StagedPackage RunSetup(bool check, Func<string, bool> confirmTrust = null)
    {
        if (!check && !IsAdministrator()) throw new UnauthorizedAccessException("此操作需要管理员权限。");
        using (var payload = new EmbeddedPackage()) return Run(check, payload.Tools, payload.Package, confirmTrust);
    }

    public static int Main(string[] args)
    {
        try {
            if (args.Length != 1 || (args[0] != "install" && args[0] != "--check"))
                throw new ArgumentException("xiaoji-setup.exe install | --check");
            RunSetup(args[0] == "--check");
            return 0;
        } catch (Exception error) {
            Console.Error.WriteLine("xiaoji: " + error.Message);
            return 1;
        }
    }
}
