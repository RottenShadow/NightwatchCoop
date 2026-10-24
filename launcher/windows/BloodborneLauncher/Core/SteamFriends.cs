using System.IO;
using System.Runtime.InteropServices;

namespace BloodborneLauncher.Core;

/// <summary>
/// Reads the signed-in Steam account for the Steam P2P status row. The launcher deliberately
/// does not enumerate friends: joining happens in the Steam overlay after the game starts. A
/// package without steam_api64.dll still starts and reports that Steam is unavailable.
/// </summary>
public static class SteamAccount
{
    public sealed record Snapshot(ulong LocalId, string LocalName, string Error);

    private static readonly object LoadGate = new();
    private static string? _loaded;

    /// <summary>The Steam persona as the engine's party name: 1-16 of [A-Za-z0-9_-], spaces
    /// become underscores, other characters are dropped, and an empty result is "Hunter".</summary>
    public static string PartyName(string persona)
    {
        var name = "";
        foreach (var c in persona)
        {
            if (name.Length == 16) break;
            if (c is >= 'A' and <= 'Z' or >= 'a' and <= 'z' or >= '0' and <= '9' or '_' or '-')
                name += c;
            else if (c == ' ')
                name += '_';
        }
        return name.Length == 0 ? "Hunter" : name;
    }

    /// <summary>Why Play cannot use Steam P2P, from a failed account read.</summary>
    public static string Problem(Snapshot snapshot)
    {
        if (snapshot.LocalId != 0) return "";
        if (snapshot.Error.Contains("steam_api64.dll", StringComparison.OrdinalIgnoreCase))
            return "Steam P2P is unavailable: " + snapshot.Error;
        return "Steam is not open. Start the Steam client and sign in, then press Play again.";
    }

    public static Snapshot Read(uint appid)
    {
        var dll = FindDll();
        if (dll == null)
            return new Snapshot(0, "", "steam_api64.dll is not installed next to the launcher.");

        Environment.SetEnvironmentVariable("SteamAppId", appid.ToString());
        Environment.SetEnvironmentVariable("SteamGameId", appid.ToString());

        try
        {
            lock (LoadGate)
            {
                if (_loaded != dll)
                {
                    NativeLibrary.Load(dll);
                    _loaded = dll;
                }
            }
        }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException)
        {
            return new Snapshot(0, "", "steam_api64.dll could not be loaded.");
        }

        var error = Marshal.AllocHGlobal(1024);
        try
        {
            Marshal.WriteByte(error, 0);
            if (SteamAPI_InitFlat(error) != 0)
            {
                var message = Marshal.PtrToStringUTF8(error);
                return new Snapshot(0, "",
                    string.IsNullOrWhiteSpace(message) ? "Steam is not running or no account is signed in." : message);
            }
            try
            {
                SteamAPI_RunCallbacks();
                var user = SteamAPI_SteamUser_v023();
                var friends = SteamAPI_SteamFriends_v017();
                var localId = SteamAPI_ISteamUser_GetSteamID(user);
                var localName = Text(SteamAPI_ISteamFriends_GetPersonaName(friends));
                if (friends == IntPtr.Zero || localId == 0)
                    return new Snapshot(0, "", "Steam did not return the signed-in account.");
                return new Snapshot(localId, localName.Length > 0 ? localName : localId.ToString(), "");
            }
            finally
            {
                SteamAPI_Shutdown();
            }
        }
        catch (Exception e) when (e is DllNotFoundException or BadImageFormatException or EntryPointNotFoundException)
        {
            return new Snapshot(0, "", "steam_api64.dll does not match this launcher.");
        }
        finally
        {
            Marshal.FreeHGlobal(error);
        }
    }

    private static string? FindDll()
    {
        foreach (var dir in new[] { AppContext.BaseDirectory, Paths.BinariesDir })
        {
            try
            {
                var path = Path.Combine(dir, "steam_api64.dll");
                if (File.Exists(path)) return path;
            }
            catch (Exception) { }
        }
        return null;
    }

    private static string Text(IntPtr utf8) => utf8 == IntPtr.Zero ? "" : Marshal.PtrToStringUTF8(utf8) ?? "";

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern int SteamAPI_InitFlat(IntPtr error);

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SteamAPI_RunCallbacks();

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SteamAPI_Shutdown();

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr SteamAPI_SteamUser_v023();

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern ulong SteamAPI_ISteamUser_GetSteamID(IntPtr self);

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr SteamAPI_SteamFriends_v017();

    [DllImport("steam_api64.dll", CallingConvention = CallingConvention.Cdecl)]
    private static extern IntPtr SteamAPI_ISteamFriends_GetPersonaName(IntPtr self);
}
