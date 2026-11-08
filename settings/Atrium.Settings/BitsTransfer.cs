using System.Runtime.InteropServices;

namespace Atrium.Settings;

// Windows owns the transfer. Releasing these COM proxies never cancels the job.
internal sealed class BitsTransfer : IDisposable
{
    internal enum State { Queued, Connecting, Transferring, Suspended, Error, TransientError, Transferred, Acknowledged, Cancelled }
    [StructLayout(LayoutKind.Sequential)]
    internal struct Progress { public ulong Total, Transferred; public uint FilesTotal, FilesTransferred; }
    private readonly IJob job;
    internal Guid Id { get; }
    private BitsTransfer(Guid id, IJob job) { Id = id; this.job = job; }

    private static IManager Manager() => (IManager)Activator.CreateInstance(
        Type.GetTypeFromCLSID(new Guid("4991d34b-80a1-4291-83b6-3328366b9097"), true)!)!;

    internal static BitsTransfer Create(string name)
    {
        var manager = Manager();
        try { manager.CreateJob(name, 0, out var id, out var value); return new(id, (IJob)value); }
        finally { Marshal.FinalReleaseComObject(manager); }
    }

    internal static BitsTransfer? Open(Guid id)
    {
        var manager = Manager();
        try { manager.GetJob(ref id, out var value); return new(id, (IJob)value); }
        catch (COMException e) when (e.HResult == unchecked((int)0x80200001)) { return null; }
        finally { Marshal.FinalReleaseComObject(manager); }
    }

    internal State CurrentState { get { job.GetState(out var state); return state; } }
    internal Progress CurrentProgress { get { job.GetProgress(out var progress); return progress; } }
    internal void Add(Uri source, string target) => job.AddFile(source.AbsoluteUri, target);
    internal void OnTransferred(string program)
    {
        job.SetNotifyCmdLine(program, $"\"{program}\" --finish-update");
        job.SetNotifyFlags(3); // transferred or permanent error; transient errors retry in BITS
        job.SetMinimumRetryDelay(30);
    }
    internal void Resume() => job.Resume();
    internal void Complete() => job.Complete();
    internal void Cancel() => job.Cancel();
    public void Dispose() => Marshal.FinalReleaseComObject(job);

    [ComImport, Guid("5ce34c0d-0dc9-4c1f-897c-daa1b78cee7c"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IManager
    {
        void CreateJob([MarshalAs(UnmanagedType.LPWStr)] string name, int type, out Guid id, [MarshalAs(UnmanagedType.IUnknown)] out object job);
        void GetJob(ref Guid id, [MarshalAs(UnmanagedType.IUnknown)] out object job);
    }

    // IBackgroundCopyJob2's ABI includes all IBackgroundCopyJob methods first.
    [ComImport, Guid("54b50739-686f-45eb-9dff-d6a9a0faa9af"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    private interface IJob
    {
        void AddFileSet(uint count, IntPtr files);
        void AddFile([MarshalAs(UnmanagedType.LPWStr)] string remote, [MarshalAs(UnmanagedType.LPWStr)] string local);
        void EnumFiles(out IntPtr files);
        void Suspend();
        void Resume();
        void Cancel();
        void Complete();
        void GetId(out Guid id);
        void GetJobType(out int type);
        void GetProgress(out Progress progress);
        void GetTimes(out IntPtr times);
        void GetState(out State state);
        void GetError(out IntPtr error);
        void GetOwner(out IntPtr owner);
        void SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string name);
        void GetDisplayName(out IntPtr name);
        void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string description);
        void GetDescription(out IntPtr description);
        void SetPriority(int priority);
        void GetPriority(out int priority);
        void SetNotifyFlags(uint flags);
        void GetNotifyFlags(out uint flags);
        void SetNotifyInterface(IntPtr callback);
        void GetNotifyInterface(out IntPtr callback);
        void SetMinimumRetryDelay(uint seconds);
        void GetMinimumRetryDelay(out uint seconds);
        void SetNoProgressTimeout(uint seconds);
        void GetNoProgressTimeout(out uint seconds);
        void GetErrorCount(out uint count);
        void SetProxySettings(int usage, IntPtr list, IntPtr bypass);
        void GetProxySettings(out int usage, out IntPtr list, out IntPtr bypass);
        void TakeOwnership();
        void SetNotifyCmdLine([MarshalAs(UnmanagedType.LPWStr)] string program, [MarshalAs(UnmanagedType.LPWStr)] string arguments);
    }
}
