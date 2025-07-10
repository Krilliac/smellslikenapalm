// data/scripts/ScriptHelpers.cs

using System;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Text.Json;

/// <summary>
/// A “kitchen sink” helper library for RS2V C# scripts:
/// - Admin checks & chat commands
/// - Debug draw stubs
/// - EAC memory read/write/alloc
/// - Live metrics & history
/// - JSON persistence
/// - Throttling, debouncing, scheduling
/// - File, script toggling & listing
/// - Utility formatting & random strings
/// </summary>
public static class ScriptHelpers
{
    #region Native Bindings

    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerAdminLevel(string playerId);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void SendChatToPlayer(string playerId, string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BroadcastChat(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogInfo(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogWarning(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void LogError(string message);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void ScheduleCallback(float delaySeconds, string methodName);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern string GetDataDirectory();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern int GetPlayerCount();
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool IsPlayerOnline(string playerId);

    // EAC memory
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool RemoteRead(uint clientId, ulong address, uint length);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool RemoteWrite(uint clientId, ulong address, byte[] buffer, uint length);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern bool RemoteAlloc(uint clientId, uint length, ulong protection);
    [DllImport("RS2VNativePlugin", CallingConvention = CallingConvention.Cdecl)]
    private static extern void BroadcastRemoteRead(ulong address, uint length);

    #endregion

    #region Permissions

    public static bool RequireAdmin(string playerId, int minLevel = 2)
    {
        if (GetPlayerAdminLevel(playerId) < minLevel)
        {
            SendChatToPlayer(playerId, $"Insufficient permission (need ≥{minLevel}).");
            return false;
        }
        return true;
    }

    public static bool HasAdmin(string playerId, int level = 2)
        => GetPlayerAdminLevel(playerId) >= level;

    public static void AdminOnly(string playerId, int level, Action action)
    {
        if (RequireAdmin(playerId, level)) action();
    }

    #endregion

    #region Chat Command Parsing

    private static Dictionary<string, Action<string[],string>> _cmds
        = new Dictionary<string, Action<string[],string>>();

    public static void RegisterCommands(Dictionary<string,Action<string[],string>> cmds)
        => _cmds = cmds;

    public static void ProcessChatCmd(string playerId, string msg)
    {
        if (!msg.StartsWith("!")) return;
        var parts = msg.Substring(1).Split(' ');
        if (_cmds.TryGetValue(parts[0], out var h))
        {
            if (!RequireAdmin(playerId)) return;
            h(parts.Skip(1).ToArray(), playerId);
        }
    }

    #endregion

    #region Debug Draw (Stubs)

    public static void DebugLine(float x1,float y1,float z1,
                                 float x2,float y2,float z2,
                                 float dur=1f,float thr=1f,
                                 float r=1f,float g=1f,float b=1f)
        => BroadcastChat($"[DbgLine] {x1},{y1},{z1}->{x2},{y2},{z2} dur={dur}");

    public static void DebugSphere(float x,float y,float z,
                                   float rad=1f,float dur=1f,
                                   float r=1f,float g=1f,float b=1f)
        => BroadcastChat($"[DbgSphere] {x},{y},{z} r={rad} dur={dur}");

    public static void DebugText(float x,float y,float z,
                                 string txt,float dur=1f,
                                 float r=1f,float g=1f,float b=1f)
        => BroadcastChat($"[DbgText] {txt} @({x},{y},{z})");

    #endregion

    #region EAC Memory Ops

    public static void ReadMem(uint cid,ulong addr,uint len)
    {
        if (!RemoteRead(cid,addr,len))
            LogError($"ReadMem fail cid={cid}, addr=0x{addr:X}, len={len}");
    }

    public static void WriteMem(uint cid,ulong addr,byte[] buf)
    {
        if (!RemoteWrite(cid,addr,buf,(uint)buf.Length))
            LogError($"WriteMem fail cid={cid}, addr=0x{addr:X}");
    }

    public static void AllocMem(uint cid,uint size,ulong prot)
    {
        if (!RemoteAlloc(cid,size,prot))
            LogError($"AllocMem fail cid={cid}, size={size}");
    }

    public static void ReadAllMem(ulong addr,uint len)
        => BroadcastRemoteRead(addr,len);

    #endregion

    #region Throttle & Debounce

    private static Dictionary<string,DateTime> _last = new();
    private static Dictionary<string,Action> _deb = new();
    private static readonly object _tl = new();

    public static bool Throttle(string key,int s)
    {
        lock(_tl){
            var n=DateTime.UtcNow;
            if(_last.TryGetValue(key,out var t)&& (n-t).TotalSeconds<s) return false;
            _last[key]=n; return true;
        }
    }

    public static void Debounce(string key,int ms,Action act)
    {
        if(!Throttle(key,ms/1000))return;
        _deb[key]=act;
        ScheduleCallback(ms/1000f,$"ScriptHelpers.ExecuteDeb_{key}");
    }

    public static void ExecuteDeb(string key)
    {
        if(_deb.TryGetValue(key,out var a)){a();_deb.Remove(key);}
    }

    #endregion

    #region Metrics & Persistence

    private static readonly string _metf="metrics";
    public struct Metric{public DateTime T;public float V;}

    public static void Track(string name,float val)
    {
        var d=Load<Dictionary<string,List<Metric>>>(_metf,new());
        if(!d.ContainsKey(name))d[name]=new();
        d[name].Add(new Metric{T=DateTime.UtcNow,V=val});
        if(d[name].Count>1000)d[name].RemoveRange(0,d[name].Count-1000);
        Save(_metf,d);
    }

    public static float Avg(string name,int mins)
    {
        var d=Load<Dictionary<string,List<Metric>>>(_metf,new());
        if(!d.ContainsKey(name))return 0;
        var co=DateTime.UtcNow.AddMinutes(-mins);
        var r=d[name].Where(x=>x.T>co).Select(x=>x.V);
        return r.Any()?r.Average():0;
    }

    public static void Save<T>(string f,T data)
    {
        try{
            var p=Path.Combine(GetDataDirectory(),"persistent");
            Directory.CreateDirectory(p);
            File.WriteAllText(Path.Combine(p,f+".json"),
                JsonSerializer.Serialize(data,new(){WriteIndented=true}));
        }catch(Exception e){LogError($"Save {f}: {e.Message}");}
    }

    public static T Load<T>(string f,T def)
    {
        try{
            var path=Path.Combine(GetDataDirectory(),"persistent",f+".json");
            if(!File.Exists(path))return def;
            return JsonSerializer.Deserialize<T>(File.ReadAllText(path));
        }catch{ return def; }
    }

    #endregion

    #region Script File Management

    public static void ToggleScript(string name)
    {
        var r=Path.Combine(GetDataDirectory(),"scripts");
        var e=Path.Combine(r,"enabled",name+".cs");
        var d=Path.Combine(r,"disabled",name+".cs");
        try{
            if(File.Exists(e))File.Move(e,d);
            else if(File.Exists(d))File.Move(d,e);
            BroadcastChat($"Toggled {name}");
        }catch(Exception ex){LogError($"ToggleScript: {ex.Message}");}
    }

    public static void ListScripts()
    {
        var r=Path.Combine(GetDataDirectory(),"scripts");
        var e=Directory.GetFiles(Path.Combine(r,"enabled"),"*.cs").Select(Path.GetFileNameWithoutExtension);
        var d=Directory.GetFiles(Path.Combine(r,"disabled"),"*.cs").Select(Path.GetFileNameWithoutExtension);
        BroadcastChat($"Enabled: {string.Join(",",e)}");
        BroadcastChat($"Disabled: {string.Join(",",d)}");
    }

    #endregion

    #region Scheduled Tasks

    private static Dictionary<string,(DateTime next,TimeSpan iv,Action a,bool rep)> _tsk
        = new();

    public static string ScheduleRecurring(TimeSpan iv,Action a)
    {
        var id=Guid.NewGuid().ToString();
        _tsk[id]=(DateTime.UtcNow+iv,iv,a,true);
        return id;
    }

    public static void Cancel(string id)=>_tsk.Remove(id);

    public static void ProcessTasks()
    {
        var n=DateTime.UtcNow;
        foreach(var kv in _tsk.ToList())
        {
            var(id,t)=kv;
            if(n>=t.next){
                try{t.a();}catch(Exception e){LogError($"Task {id}: {e.Message}");}
                if(t.rep)_tsk[id]=(n+t.iv,t.iv,t.a,true);
                else _tsk.Remove(id);
            }
        }
    }

    #endregion

    #region Utilities

    public static string FmtSpan(TimeSpan ts)
    {
        if(ts.TotalDays>=1)return $"{(int)ts.TotalDays}d{ts.Hours}h{ts.Minutes}m";
        if(ts.TotalHours>=1)return $"{ts.Hours}h{ts.Minutes}m";
        if(ts.TotalMinutes>=1)return $"{ts.Minutes}m{ts.Seconds}s";
        return $"{ts.Seconds}s";
    }

    public static string FmtBytes(long b)
    {
        string[] s={"B","KB","MB","GB","TB"};double len=b;int o=0;
        while(len>=1024&&o<s.Length-1){o++;len/=1024;}return $"{len:0.##}{s[o]}";
    }

    public static string RandStr(int l)
    {
        const string ch="ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
        var r=new Random();
        return new string(Enumerable.Range(0,l).Select(_=>ch[r.Next(ch.Length)]).ToArray());
    }

    #endregion
}