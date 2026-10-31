using Atrium.Probe;

// Hard stop so a probe can never keep windows or system changes alive if it hangs.
int limit = args.Length > 1 && int.TryParse(args[1], out var s) ? s : 90;
var watchdog = new Thread(() =>
{
    Thread.Sleep(limit * 1000);
    Console.Error.WriteLine($"watchdog: probe exceeded {limit}s, exiting");
    SafetyNet.RunAll();
    Environment.Exit(3);
}) { IsBackground = true };
watchdog.Start();
AppDomain.CurrentDomain.ProcessExit += (_, _) => SafetyNet.RunAll();
AppDomain.CurrentDomain.UnhandledException += (_, _) => SafetyNet.RunAll();
Console.CancelKeyPress += (_, _) => SafetyNet.RunAll();

var cmd = args.Length > 0 ? args[0] : "env";
try
{
    switch (cmd)
    {
        case "env": EnvProbe.Run(); break;
        case "wda": WdaProbe.Run(); break;
        case "glass": GlassProbe.Run(args.Length > 2 ? args[2] : ""); break;
        case "wincap": WinCapProbe.Run(); break;
        case "border": BorderProbe.Run(); break;
        case "optionc": OptionCProbe.Run(); break;
        case "input": InputProbe.Run(); break;
        case "taskbar": TaskbarProbe.Run(); break;
        case "taskbar2": TaskbarProbe.RunNoAutoHide(); break;
        case "entry": ShellEntryProbe.Run(); break;
        case "trayclient": TrayClient.Run(args); break;
        case "trayhost": TrayHost.Run(); break;
        case "desktop": DesktopProbe.Run(); break;
        case "notify": NotifyProbe.Run(); break;
        case "tasks": TasksProbe.Run(); break;
        case "echo": EchoProbe.Run(); break;
        case "trayfail": TrayFailureProbe.Run(); break;
        case "smto": SmtoProbe.Run(); break;
        case "recovery": RecoveryProbe.Run(args.Length > 2 ? args[2] : "native/build/win-x64/Release/Atrium.Guard.exe", args.Length > 3 ? args[3] : ""); break;
        case "txcore": RecoveryProbe.Core(args); break;
        case "holdlock": RecoveryProbe.HoldLock(args); break;
        case "desktop2": DesktopEventProbe.Run(); break;
        case "optionc2": OptionCDynamicProbe.Run(); break;
        case "perf": PerfProbe.Run(args.Length > 2 ? args[2] : ""); break;
        case "sendto": SmtoProbe.SendTo(args); break;
        case "faketray": TrayFailureProbe.FakeTray(); break;
        case "demo": DemoProbe.Run(args.Length > 2 ? args[2] : Report.ResultsDir); break;
        case "tbcore": TaskbarProbe.Core(args.Skip(1).Prepend("x").ToArray()); break;
        case "tbguard": TaskbarProbe.Guard(args.Skip(1).Prepend("x").ToArray()); break;
        case "clicktarget": InputProbe.ClickTarget(args.Skip(1).Prepend("x").ToArray()); break;
        default: Console.Error.WriteLine("unknown probe " + cmd); return 2;
    }
}
finally
{
    SafetyNet.RunAll();
}
return 0;

// Restore actions registered by probes that touch system state. They run on
// normal exit, on exceptions, on Ctrl+C and from the watchdog.
internal static class SafetyNet
{
    private static readonly List<(string Name, Action Undo)> s_actions = new();
    private static readonly object s_lock = new();

    public static void Register(string name, Action undo)
    {
        lock (s_lock) s_actions.Add((name, undo));
    }

    public static void Unregister(string name)
    {
        lock (s_lock) s_actions.RemoveAll(a => a.Name == name);
    }

    public static void RunAll()
    {
        List<(string Name, Action Undo)> todo;
        lock (s_lock) { todo = new(s_actions); s_actions.Clear(); }
        for (int i = todo.Count - 1; i >= 0; i--)
        {
            try { todo[i].Undo(); Console.WriteLine("restored: " + todo[i].Name); }
            catch (Exception e) { Console.Error.WriteLine($"restore {todo[i].Name} failed: {e.Message}"); }
        }
    }
}
