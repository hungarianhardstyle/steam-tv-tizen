// See https://aka.ms/new-console-template for more information
using System.Net.Sockets;
using System.Text;
using System.Net;
using System.IO.Compression;
using TizenSdb.SdbClient;
using TizenSdb.SigningManager;

using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
var work = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "../../../.."));
SdbTcpDevice.KeyDirectory = Path.Combine(work, "sdb-key");
await using var device = new SdbTcpDevice(IPAddress.Parse("192.168.1.6"));
await device.ConnectAsync(timeout.Token);
Console.WriteLine($"SDB: {device.DeviceId}");
var capabilities = await device.CapabilityAsync(timeout.Token);
foreach (var pair in capabilities) Console.WriteLine($"{pair.Key}: {pair.Value}");
if (args.Length == 0) return;
if (args[0] == "debug-probe") {
    await device.ShellCommandAsync("0 was_kill SteamTV001.Probe", timeout.Token);
    var debugReply = await device.ShellCommandAsync("0 debug SteamTV001.Probe", timeout.Token);
    Console.WriteLine(debugReply);
    var port = System.Text.RegularExpressions.Regex.Match(debugReply, @"port:\s*(\d+)");
    if (!port.Success) throw new InvalidOperationException("TV did not return an inspector port.");
    await File.WriteAllTextAsync(Path.Combine(work, "tv-inspector-port.txt"), port.Groups[1].Value);
    return;
}
if (args[0] == "shell") {
    // Diagnostics: run one command on the TV and print its output. This is how the app's own log
    // is read (dlogutil) without having to launch it in debug mode.
    var reply = await device.ShellCommandAsync("0 " + string.Join(" ", args.Skip(1)), timeout.Token);
    Console.WriteLine(reply);
    return;
}
if (args[0] != "install-probe") throw new ArgumentException("Only install-probe is supported.");
var stage = Path.Combine(work, "steam-tv");
var package = Path.Combine(work, "SteamTVProbe.wgt");
if (!File.Exists(Path.Combine(stage, "ihs.wasm"))) throw new FileNotFoundException("Build the WASM probe first.");
if (File.Exists(package)) File.Delete(package);
using (var zip = ZipFile.Open(package, ZipArchiveMode.Create)) {
    foreach (var file in Directory.EnumerateFiles(stage)) {
        if (Path.GetExtension(file) is ".js" or ".html" or ".wasm" or ".xml" or ".mem" or ".png") zip.CreateEntryFromFile(file, Path.GetFileName(file));
    }
}
var certificates = "C:/Users/deero/AppData/Roaming/Apps2Samsung/Certificate/Jelly2Sams - Public";
var password = (await File.ReadAllTextAsync(Path.Combine(certificates, "password.txt"))).Trim();
await TizenWgtSigner.ReSignWgtWithCertsInPlace(package, Path.Combine(certificates, "author.p12"), Path.Combine(certificates, "distributor.p12"), password);
Console.WriteLine("Probe package signed using the installed Apps2Samsung certificates.");
var remoteDirectory = capabilities.GetValueOrDefault("sdk_toolpath") ?? "/home/owner/share/tmp/sdk_tools";
if (remoteDirectory != "/home/owner/share/tmp/sdk_tools") throw new InvalidDataException("Unexpected TV staging path.");
var remotePackage = remoteDirectory.TrimEnd('/') + "/SteamTVProbe.wgt";
await using (var stream = File.OpenRead(package)) await device.PushAsync(stream, remotePackage, ct: timeout.Token);
var installReply = await device.ShellCommandAsync($"0 vd_appinstall SteamTV001.Probe {remotePackage}", timeout.Token);
Console.WriteLine(installReply);
if (!installReply.Contains("install completed", StringComparison.OrdinalIgnoreCase)) throw new InvalidOperationException("The TV did not confirm installation.");
