// lhchannel: read or set a SteamVR Base Station 2.0's channel over the PC's own Bluetooth LE, for setups without a
// Lighthouse headset (SteamVR reaches base stations through the headset's radio and says "Bluetooth not available").
//   dotnet run                       list the base stations in range and their channels
//   dotnet run -- lhb-xxxxxxxx 7     set that station to channel 7 (1-16); refuses a channel another station uses
//   dotnet run -- lhb-xxxxxxxx blink flash that station's LED, to see which one it is
// Base station 2.0 GATT (service 00001523-...): mode = channel (00001524-...), identify (00008421-...).
// One connection per station for the whole run: Windows won't hand out a station's service a second time while the
// first one is open.
using System.Collections.Concurrent;
using Windows.Devices.Bluetooth;
using Windows.Devices.Bluetooth.Advertisement;
using Windows.Devices.Bluetooth.GenericAttributeProfile;
using Windows.Storage.Streams;

sealed class Station : IDisposable
{
    public string Name = "";
    public BluetoothLEDevice? Dev;
    public GattDeviceService? Svc;
    public string Why = "";  // why it couldn't be opened
    public void Dispose() { Svc?.Dispose(); Dev?.Dispose(); }
}

static class Program
{
    static readonly Guid ServiceId = new("00001523-1212-efde-1523-785feabcd124");
    static readonly Guid ModeId = new("00001524-1212-efde-1523-785feabcd124");
    static readonly Guid IdentifyId = new("00008421-1212-efde-1523-785feabcd124");

    static async Task<int> Main(string[] args)
    {
        string? name = args.Length > 0 ? args[0] : null;
        string? action = args.Length > 1 ? args[1].ToLowerInvariant() : null;
        bool force = args.Any(a => a.Equals("force", StringComparison.OrdinalIgnoreCase));

        Console.WriteLine("Looking for base stations over Bluetooth (up to 10 s)...");
        var seen = await Scan(TimeSpan.FromSeconds(10));
        if (seen.Count == 0)
        {
            Console.WriteLine("No base station seen. Is the PC's Bluetooth on and the stations awake? Close SpaceSync and try again.");
            return 1;
        }

        var open = new Dictionary<string, Station>(StringComparer.OrdinalIgnoreCase);
        try
        {
            var channels = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            foreach (var kv in seen.OrderBy(k => k.Key, StringComparer.OrdinalIgnoreCase))
            {
                var st = await Open(kv.Key, kv.Value);
                open[kv.Key] = st;
                int ch = await ReadChannel(st);
                channels[kv.Key] = ch;
                Console.WriteLine($"  {kv.Key}: channel {(ch > 0 ? ch.ToString() : "? (" + st.Why + ")")}");
            }
            if (name == null) return 0;

            string? key = seen.Keys.FirstOrDefault(k => k.Equals(name, StringComparison.OrdinalIgnoreCase));
            if (key == null) { Console.WriteLine($"{name} wasn't seen. The names above are the ones in range."); return 1; }
            var s = open[key];

            if (action == "blink")
            {
                var (c, why) = await Char(s, IdentifyId);
                if (c == null) { Console.WriteLine($"Couldn't reach its identify characteristic: {why}"); return 1; }
                var r = await Write(c, 1);
                Console.WriteLine(r == "" ? $"{key}: LED flashing." : $"{key}: write failed: {r}");
                return r == "" ? 0 : 1;
            }

            if (!int.TryParse(action, out int want) || want < 1 || want > 16) { Console.WriteLine("Give a channel from 1 to 16, or 'blink'."); return 2; }
            var clash = channels.Where(kv => !kv.Key.Equals(key, StringComparison.OrdinalIgnoreCase) && kv.Value == want).Select(kv => kv.Key).ToList();
            if (clash.Count > 0 && !force) { Console.WriteLine($"Channel {want} is {string.Join(", ", clash)}'s. Pick another (or add 'force')."); return 2; }
            if (channels[key] == want) { Console.WriteLine($"{key} is on channel {want} already."); return 0; }

            var (mode, mwhy) = await Char(s, ModeId);
            if (mode == null) { Console.WriteLine($"Couldn't reach its channel characteristic: {mwhy}"); return 1; }
            var w = await Write(mode, (byte)want);
            if (w != "") { Console.WriteLine($"Write failed: {w}"); return 1; }
            await Task.Delay(1500);
            int now = await ReadChannel(s);
            Console.WriteLine(now == want
                ? $"{key}: channel {channels[key]} -> {want}. Restart SteamVR."
                : $"{key}: wrote {want}, but it reads back {(now > 0 ? now.ToString() : "? (" + s.Why + ")")}. Run 'dotnet run' again in a minute to check.");
            return now == want ? 0 : 1;
        }
        finally
        {
            foreach (var st in open.Values) st.Dispose();
        }
    }

    // LHB-xxxxxxxx names (from their advertisements) and addresses
    static async Task<Dictionary<string, ulong>> Scan(TimeSpan time)
    {
        var seen = new ConcurrentDictionary<string, ulong>(StringComparer.OrdinalIgnoreCase);
        var watcher = new BluetoothLEAdvertisementWatcher { ScanningMode = BluetoothLEScanningMode.Active };
        watcher.Received += (s, e) =>
        {
            string n = e.Advertisement.LocalName;
            if (!string.IsNullOrEmpty(n) && n.StartsWith("LHB-", StringComparison.OrdinalIgnoreCase)) seen[n] = e.BluetoothAddress;
        };
        watcher.Start();
        var until = DateTime.UtcNow + time;
        int stable = 0, last = 0;
        while (DateTime.UtcNow < until && watcher.Status != BluetoothLEAdvertisementWatcherStatus.Aborted)
        {
            await Task.Delay(500);
            stable = seen.Count == last && last > 0 ? stable + 1 : 0;  // no new station for 3 s: that's all of them
            last = seen.Count;
            if (stable >= 6) break;
        }
        if (watcher.Status == BluetoothLEAdvertisementWatcherStatus.Aborted) Console.WriteLine("Bluetooth scan aborted: is the PC's Bluetooth on?");
        watcher.Stop();
        return new Dictionary<string, ulong>(seen, StringComparer.OrdinalIgnoreCase);
    }

    static async Task<Station> Open(string name, ulong addr)
    {
        var st = new Station { Name = name };
        st.Dev = await BluetoothLEDevice.FromBluetoothAddressAsync(addr);
        if (st.Dev == null) { st.Why = "device not found"; return st; }
        for (int attempt = 0; attempt < 4; attempt++)
        {
            var sr = await st.Dev.GetGattServicesForUuidAsync(ServiceId, BluetoothCacheMode.Uncached);
            if (sr.Status == GattCommunicationStatus.Success && sr.Services.Count > 0) { st.Svc = sr.Services[0]; st.Why = ""; return st; }
            st.Why = sr.Status == GattCommunicationStatus.Success ? "no base station service" : $"services: {sr.Status}";
            await Task.Delay(1000);
        }
        return st;
    }

    static async Task<(GattCharacteristic? c, string why)> Char(Station st, Guid id)
    {
        if (st.Svc == null) return (null, st.Why);
        string why = "";
        for (int attempt = 0; attempt < 4; attempt++)
        {
            var cr = await st.Svc.GetCharacteristicsForUuidAsync(id, BluetoothCacheMode.Uncached);
            if (cr.Status == GattCommunicationStatus.Success && cr.Characteristics.Count > 0) return (cr.Characteristics[0], "");
            why = cr.Status == GattCommunicationStatus.Success ? "no such characteristic (old firmware?)" : $"characteristics: {cr.Status}";
            await Task.Delay(1000);
        }
        return (null, why);
    }

    static async Task<int> ReadChannel(Station st)
    {
        var (c, why) = await Char(st, ModeId);
        if (c == null) { st.Why = why; return -1; }
        var r = await c.ReadValueAsync(BluetoothCacheMode.Uncached);
        if (r.Status != GattCommunicationStatus.Success || r.Value.Length < 1) { st.Why = $"read: {r.Status}"; return -1; }
        return DataReader.FromBuffer(r.Value).ReadByte();
    }

    // "" on success, else why
    static async Task<string> Write(GattCharacteristic c, byte value)
    {
        var w = new DataWriter();
        w.WriteByte(value);
        var res = await c.WriteValueWithResultAsync(w.DetachBuffer(), GattWriteOption.WriteWithResponse);
        if (res.Status == GattCommunicationStatus.Success) return "";
        return res.ProtocolError.HasValue ? $"{res.Status} (protocol error {res.ProtocolError.Value})" : res.Status.ToString();
    }
}
