using Windows.UI.Notifications;
using Windows.UI.Notifications.Management;

namespace Atrium.Probe;

// P05 (minimal): can an unpackaged desktop process use UserNotificationListener?
// Never requests access. Reads only the NUMBER of current toasts and the app
// count, never titles or text.
internal static class NotifyProbe
{
    public static void Run()
    {
        var rep = new Report("p05-notification-listener");
        string status;
        var listener = UserNotificationListener.Current;
        try { status = listener.GetAccessStatus().ToString(); }
        catch (Exception e) { status = $"{e.GetType().Name}: 0x{e.HResult:X8} {e.Message.Trim()}"; }
        rep.Set("unpackaged.accessStatus", status);

        string read;
        try
        {
            var list = listener.GetNotificationsAsync(NotificationKinds.Toast).AsTask().GetAwaiter().GetResult();
            read = $"ok: {list.Count} toasts from {list.Select(n => n.AppInfo?.Id ?? "?").Distinct().Count()} apps";
        }
        catch (Exception e) { read = $"{e.GetType().Name}: 0x{e.HResult:X8} {e.Message.Trim()}"; }
        rep.Set("unpackaged.getNotifications", read);

        string evt;
        try
        {
            listener.NotificationChanged += (_, _) => { };
            evt = "subscribed";
        }
        catch (Exception e) { evt = $"{e.GetType().Name}: 0x{e.HResult:X8} {e.Message.Trim()}"; }
        rep.Set("unpackaged.notificationChangedEvent", evt);

        rep.Verdict("unpackaged-access", status == "Allowed" ? "Passed" : "Failed", "access status for an unpackaged process: " + status);
        rep.Verdict("unpackaged-read", read.StartsWith("ok") ? "Passed" : "Failed", "reading current toasts (count only): " + read);
        rep.Verdict("unpackaged-change-event", evt == "subscribed" ? "Passed" : "Failed", "NotificationChanged without package identity: " + evt);
        rep.Save();
    }
}
