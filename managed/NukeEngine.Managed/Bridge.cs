// The engine's managed bridge — hosted by the NukeCSharp native module (hostfxr/CoreCLR).
//
// Flow: the native side loads THIS assembly through hostfxr and grabs the
// [UnmanagedCallersOnly] statics below by name. Game scripts live in a SEPARATE assembly
// (GameScripts.dll — compiled from the project's .cs files by the module, or read from the
// pak) which loads into a COLLECTIBLE AssemblyLoadContext: hot reload = unload that ALC and
// load the freshly compiled bytes; Electron instances are recreated by their
// CSharpScript components on the next update, so classes keep working through PIE.
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Runtime.Loader;

namespace NukeEngine;

// The C# script base class — the counterpart of a Lua script table; named after the
// engine's own nomenclature (a game object is an Atom — an Electron is the moving part
// that gives it behaviour). Game classes derive from this; a CSharpScript component
// instantiates them by class name.
public abstract class Electron
{
    public long AtomId { get; internal set; }

    public virtual void Start() {}
    public virtual void Update(double dt) {}

    // Engine API. `Atom` is the owning atom (transform, components); any reflected
    // component is reachable by type name — see the Atom/Component classes below.
    public void Log(string text) => Native.Log(text);
    public Atom Atom => _atom ??= new Atom(AtomId);
    Atom? _atom;

    public Vector3    Position    { get => Atom.Position;    set => Atom.Position    = value; }
    public Quaternion Rotation    { get => Atom.Rotation;    set => Atom.Rotation    = value; }   // QUATERNION, like the engine
    public Vector3    EulerAngles { get => Atom.EulerAngles; set => Atom.EulerAngles = value; }   // degrees, XYZ
    public Vector3    Scale       { get => Atom.Scale;       set => Atom.Scale       = value; }
    public Component? GetComponent(string type) => Atom.GetComponent(type);
    public T? GetComponent<T>() where T : Component => Atom.GetComponent<T>();
    public T? AddComponent<T>() where T : Component => Atom.AddComponent<T>();
}

// The native function table (filled by NukeCSharp.dll at Init) — managed -> engine calls.
// LAYOUT MUST MATCH the C++ NativeApi exactly; extend only by appending on both sides.
[StructLayout(LayoutKind.Sequential)]
internal unsafe struct NativeApi
{
    public delegate* unmanaged[Cdecl]<byte*, void>              log;          // utf8 text
    public delegate* unmanaged[Cdecl]<long, double*, void>      getPosition;  // atomId, double[3]
    public delegate* unmanaged[Cdecl]<long, double*, void>      setPosition;  // atomId, double[3]
    public delegate* unmanaged[Cdecl]<byte*, long>              findAtom;     // name -> id (0 = none)
    public delegate* unmanaged[Cdecl]<long, byte*, long>        getComponent; // atomId, type -> compId
    public delegate* unmanaged[Cdecl]<long, byte*, long>        addComponent;
    public delegate* unmanaged[Cdecl]<long, long, byte*, byte*, int, int> getProp;   // -> needed len (0 = fail)
    public delegate* unmanaged[Cdecl]<long, long, byte*, byte*, int>      setProp;   // -> 1 = ok
    public delegate* unmanaged[Cdecl]<long, long, byte*, byte*, byte*, int, int> invoke; // -> ret len / -1
    public delegate* unmanaged[Cdecl]<long, double*, void>      getTransform; // double[9]: pos, euler deg, scale
    public delegate* unmanaged[Cdecl]<long, double*, void>      setTransform;
    public delegate* unmanaged[Cdecl]<double*, double*, void>   timeInfo;     // delta, elapsed
    public delegate* unmanaged[Cdecl]<long, long, byte*, byte*, byte*, int, int> getObjProp; // sub-object channel
    public delegate* unmanaged[Cdecl]<long, long, byte*, byte*, byte*, int>      setObjProp;
    // FULL OBJECT MODEL: engine-owned handles over EVERY reflected Model class.
    public delegate* unmanaged[Cdecl]<byte*, long>                    createObject;
    public delegate* unmanaged[Cdecl]<byte*, long>                    objectFromGuid;
    public delegate* unmanaged[Cdecl]<byte*, byte*, long>             findAsset;      // type, name
    public delegate* unmanaged[Cdecl]<long, byte*, int, int>          objectGuid;
    public delegate* unmanaged[Cdecl]<long, byte*, int, int>          objectType;
    public delegate* unmanaged[Cdecl]<long, byte*, byte*, int, int>   objGet;
    public delegate* unmanaged[Cdecl]<long, byte*, byte*, int>        objSet;
    public delegate* unmanaged[Cdecl]<long, byte*, byte*, byte*, int, int> objInvoke;
    public delegate* unmanaged[Cdecl]<long, long, byte*, long>        componentObject;
    public delegate* unmanaged[Cdecl]<long, int, int, byte*, int, int> setTexturePixels;
    public delegate* unmanaged[Cdecl]<byte*, byte*, int, int>         readContent;
    // Reflected STATIC [[nuke::func]] methods by TYPE name (facades: Audio, Physics, ...).
    public delegate* unmanaged[Cdecl]<byte*, byte*, byte*, byte*, int, int> staticInvoke;
    // Mesh CONTENT: unindexed triangle list (normals/uvs may be null -> computed/zeroed).
    public delegate* unmanaged[Cdecl]<long, int, float*, float*, float*, int> setMeshGeometry;
    // Sound CONTENT: play encoded audio bytes (ogg/wav/mp3/flac) -> voice handle (0 = fail).
    public delegate* unmanaged[Cdecl]<byte*, long, double, int, int, double> playAudioData;
    // The atom's live Transform wrapped as a reflected OBJECT handle (0 = dead atom).
    public delegate* unmanaged[Cdecl]<long, long> atomTransformObject;
}

internal static unsafe class Native
{
    internal static NativeApi Api;

    static byte[] Utf8(string s) => System.Text.Encoding.UTF8.GetBytes(s + "\0");

    internal static void Log(string text)
    {
        if (Api.log == null) return;
        fixed (byte* p = Utf8(text)) Api.log(p);
    }
    internal static void GetPosition(long atomId, out Vector3 v)
    {
        v = default;
        if (Api.getPosition == null) return;
        fixed (Vector3* p = &v) Api.getPosition(atomId, (double*)p);
    }
    internal static void SetPosition(long atomId, in Vector3 v)
    {
        if (Api.setPosition == null) return;
        fixed (Vector3* p = &v) Api.setPosition(atomId, (double*)p);
    }
    internal static long FindAtom(string name)
    {
        if (Api.findAtom == null) return 0;
        fixed (byte* p = Utf8(name)) return Api.findAtom(p);
    }
    internal static long GetComponent(long atomId, string type)
    {
        if (Api.getComponent == null) return 0;
        fixed (byte* p = Utf8(type)) return Api.getComponent(atomId, p);
    }
    internal static long AddComponent(long atomId, string type)
    {
        if (Api.addComponent == null) return 0;
        fixed (byte* p = Utf8(type)) return Api.addComponent(atomId, p);
    }
    internal static string? GetProp(long atomId, long compId, string name)
    {
        if (Api.getProp == null) return null;
        fixed (byte* n = Utf8(name))
        {
            int need = Api.getProp(atomId, compId, n, null, 0);
            if (need <= 0) return null;
            var buf = new byte[need];
            fixed (byte* b = buf) Api.getProp(atomId, compId, n, b, need);
            return System.Text.Encoding.UTF8.GetString(buf);
        }
    }
    internal static bool SetProp(long atomId, long compId, string name, string valueJson)
    {
        if (Api.setProp == null) return false;
        fixed (byte* n = Utf8(name))
        fixed (byte* v = Utf8(valueJson))
            return Api.setProp(atomId, compId, n, v) == 1;
    }
    internal static (bool ok, string? ret) Invoke(long atomId, long compId, string method, string argsJson)
    {
        if (Api.invoke == null) return (false, null);
        fixed (byte* m = Utf8(method))
        fixed (byte* a = Utf8(argsJson))
        {
            int need = Api.invoke(atomId, compId, m, a, null, 0);
            if (need < 0) return (false, null);
            if (need == 0) return (true, null);   // void
            var buf = new byte[need];
            fixed (byte* b = buf) Api.invoke(atomId, compId, m, a, b, need);
            return (true, System.Text.Encoding.UTF8.GetString(buf));
        }
    }
    internal static string? GetObjProp(long atomId, long compId, string path, string name)
    {
        if (Api.getObjProp == null) return null;
        fixed (byte* p = Utf8(path))
        fixed (byte* n = Utf8(name))
        {
            int need = Api.getObjProp(atomId, compId, p, n, null, 0);
            if (need <= 0) return null;
            var buf = new byte[need];
            fixed (byte* b = buf) Api.getObjProp(atomId, compId, p, n, b, need);
            return System.Text.Encoding.UTF8.GetString(buf);
        }
    }
    internal static bool SetObjProp(long atomId, long compId, string path, string name, string valueJson)
    {
        if (Api.setObjProp == null) return false;
        fixed (byte* p = Utf8(path))
        fixed (byte* n = Utf8(name))
        fixed (byte* v = Utf8(valueJson))
            return Api.setObjProp(atomId, compId, p, n, v) == 1;
    }
    internal static long CreateObject(string type)
    {
        if (Api.createObject == null) return 0;
        fixed (byte* t = Utf8(type)) return Api.createObject(t);
    }
    internal static long ObjectFromGuid(string guid)
    {
        if (Api.objectFromGuid == null || guid.Length == 0) return 0;
        fixed (byte* g = Utf8(guid)) return Api.objectFromGuid(g);
    }
    internal static long FindAsset(string type, string name)
    {
        if (Api.findAsset == null) return 0;
        fixed (byte* t = Utf8(type))
        fixed (byte* n = Utf8(name)) return Api.findAsset(t, n);
    }
    static string SizedString(delegate* unmanaged[Cdecl]<long, byte*, int, int> fn, long id)
    {
        if (fn == null) return "";
        int need = fn(id, null, 0);
        if (need <= 0) return "";
        var buf = new byte[need];
        fixed (byte* b = buf) fn(id, b, need);
        return System.Text.Encoding.UTF8.GetString(buf);
    }
    internal static string ObjectGuid(long id) => SizedString(Api.objectGuid, id);
    internal static string ObjectType(long id) => SizedString(Api.objectType, id);
    internal static string? ObjGet(long id, string name)
    {
        if (Api.objGet == null) return null;
        fixed (byte* n = Utf8(name))
        {
            int need = Api.objGet(id, n, null, 0);
            if (need <= 0) return null;
            var buf = new byte[need];
            fixed (byte* b = buf) Api.objGet(id, n, b, need);
            return System.Text.Encoding.UTF8.GetString(buf);
        }
    }
    internal static bool ObjSet(long id, string name, string valueJson)
    {
        if (Api.objSet == null) return false;
        fixed (byte* n = Utf8(name))
        fixed (byte* v = Utf8(valueJson)) return Api.objSet(id, n, v) == 1;
    }
    internal static (bool ok, string? ret) ObjInvoke(long id, string method, string argsJson)
    {
        if (Api.objInvoke == null) return (false, null);
        fixed (byte* m = Utf8(method))
        fixed (byte* a = Utf8(argsJson))
        {
            int need = Api.objInvoke(id, m, a, null, 0);
            if (need < 0) return (false, null);
            if (need == 0) return (true, null);
            var buf = new byte[need];
            fixed (byte* b = buf) Api.objInvoke(id, m, a, b, need);
            return (true, System.Text.Encoding.UTF8.GetString(buf));
        }
    }
    internal static long ComponentObject(long atomId, long compId, string path)
    {
        if (Api.componentObject == null) return 0;
        fixed (byte* p = Utf8(path)) return Api.componentObject(atomId, compId, p);
    }
    internal static bool SetTexturePixels(long id, int w, int h, byte[] rgba)
    {
        if (Api.setTexturePixels == null) return false;
        fixed (byte* b = rgba) return Api.setTexturePixels(id, w, h, b, rgba.Length) == 1;
    }
    internal static byte[]? ReadContent(string rel)
    {
        if (Api.readContent == null) return null;
        fixed (byte* r = Utf8(rel))
        {
            int need = Api.readContent(r, null, 0);
            if (need <= 0) return null;
            var buf = new byte[need];
            fixed (byte* b = buf) Api.readContent(r, b, need);
            return buf;
        }
    }
    internal static (bool ok, string? ret) StaticInvoke(string type, string method, string argsJson)
    {
        if (Api.staticInvoke == null) return (false, null);
        fixed (byte* t = Utf8(type))
        fixed (byte* m = Utf8(method))
        fixed (byte* a = Utf8(argsJson))
        {
            int need = Api.staticInvoke(t, m, a, null, 0);
            if (need < 0) return (false, null);
            if (need == 0) return (true, null);
            var buf = new byte[need];
            fixed (byte* b = buf) Api.staticInvoke(t, m, a, b, need);
            return (true, System.Text.Encoding.UTF8.GetString(buf));
        }
    }
    internal static bool SetMeshGeometry(long id, float[] verts, float[]? normals, float[]? uvs)
    {
        if (Api.setMeshGeometry == null || verts.Length == 0 || verts.Length % 9 != 0) return false;
        int numVerts = verts.Length / 3;
        fixed (float* v = verts)
        fixed (float* n = normals)
        fixed (float* u = uvs)
            return Api.setMeshGeometry(id, numVerts, v, n, u) == 1;
    }
    internal static double PlayAudioData(byte[] bytes, double volume, bool loop, int bus)
    {
        if (Api.playAudioData == null || bytes.Length == 0) return 0;
        fixed (byte* b = bytes) return Api.playAudioData(b, bytes.Length, volume, loop ? 1 : 0, bus);
    }
    internal static long AtomTransformObject(long atomId)
        => Api.atomTransformObject != null ? Api.atomTransformObject(atomId) : 0;
    internal static void GetTransform(long atomId, double* t9) { if (Api.getTransform != null) Api.getTransform(atomId, t9); }
    internal static void SetTransform(long atomId, double* t9) { if (Api.setTransform != null) Api.setTransform(atomId, t9); }
    internal static void TimeInfo(out double delta, out double elapsed)
    {
        delta = 0; elapsed = 0;
        if (Api.timeInfo == null) return;
        fixed (double* d = &delta) fixed (double* e = &elapsed) Api.timeInfo(d, e);
    }
}

// ---- the game-facing API ----------------------------------------------------------------------

// Base of EVERY reflected engine object exposed to C# (Material, Texture, Mesh, Shader,
// AnimClip, ...): an engine-owned HANDLE — created assets register into ResDB and live
// like imported ones; wrapped instances belong to their engine owners. Never a guid in
// user code: objects assign to objects.
public class NukeObject
{
    public long ObjectId { get; }
    protected internal NukeObject(long objectId) { ObjectId = objectId; }

    public string Guid => Native.ObjectGuid(ObjectId);
    public string TypeName => Native.ObjectType(ObjectId);

    protected static long CreateHandle(string typeName) => Native.CreateObject(typeName);
    protected static long HandleFromGuid(string guid) => Native.ObjectFromGuid(guid);
    protected static long HandleByName(string typeName, string name) => Native.FindAsset(typeName, name);

    public double? GetNumber(string prop)
    {
        var js = Native.ObjGet(ObjectId, prop);
        return js != null && double.TryParse(js, System.Globalization.CultureInfo.InvariantCulture, out var d) ? d : null;
    }
    public bool? GetBool(string prop)
    {
        var js = Native.ObjGet(ObjectId, prop);
        return js == "true" ? true : js == "false" ? false : null;
    }
    public string? GetString(string prop)
    {
        var js = Native.ObjGet(ObjectId, prop);
        if (js == null) return null;
        try { return System.Text.Json.JsonSerializer.Deserialize<string>(js); } catch { return null; }
    }
    public double[]? GetVector(string prop)
    {
        var js = Native.ObjGet(ObjectId, prop);
        if (js == null || !js.StartsWith('[')) return null;
        try { return System.Text.Json.JsonSerializer.Deserialize<double[]>(js); } catch { return null; }
    }
    // A [[nuke::func]] method on this object (JSON literal in/out, null = void).
    public string? Call(string method, params object?[] args)
    {
        var (ok, ret) = Native.ObjInvoke(ObjectId, method,
                                         System.Text.Json.JsonSerializer.Serialize(args));
        if (!ok) Native.Log($"[NukeCSharp] {TypeName}.{method} call failed (unknown method or bad args)");
        return ret;
    }
    public Vector3 GetVector3(string prop) { var v = GetVector(prop); return v is { Length: >= 3 } ? new Vector3(v[0], v[1], v[2]) : default; }
    public Vector2 GetVector2(string prop) { var v = GetVector(prop); return v is { Length: >= 2 } ? new Vector2(v[0], v[1]) : default; }
    public Vector4 GetVector4(string prop) { var v = GetVector(prop); return v is { Length: >= 4 } ? new Vector4(v[0], v[1], v[2], v[3]) : default; }
    public Quaternion GetQuat(string prop) { var v = GetVector(prop); return v is { Length: >= 4 } ? new Quaternion(v[0], v[1], v[2], v[3]) : Quaternion.Identity; }
    public Color GetColor(string prop)
    {
        var v = GetVector(prop);
        return v is { Length: >= 4 } ? new Color(v[0], v[1], v[2], v[3])
             : v is { Length: >= 3 } ? new Color(v[0], v[1], v[2]) : default;
    }
    public bool Set(string prop, double v)     => Native.ObjSet(ObjectId, prop, v.ToString(System.Globalization.CultureInfo.InvariantCulture));
    public bool Set(string prop, bool v)       => Native.ObjSet(ObjectId, prop, v ? "true" : "false");
    public bool Set(string prop, string v)     => Native.ObjSet(ObjectId, prop, System.Text.Json.JsonSerializer.Serialize(v));
    public bool Set(string prop, double[] v)    => Native.ObjSet(ObjectId, prop, System.Text.Json.JsonSerializer.Serialize(v));
    public bool Set(string prop, Vector3 v)    => Set(prop, new double[] { v.X, v.Y, v.Z });
    public bool Set(string prop, Vector2 v)    => Set(prop, new double[] { v.X, v.Y });
    public bool Set(string prop, Vector4 v)    => Set(prop, new double[] { v.X, v.Y, v.Z, v.W });
    public bool Set(string prop, Quaternion v) => Set(prop, new double[] { v.X, v.Y, v.Z, v.W });
    public bool Set(string prop, Color v)      => Set(prop, new double[] { v.R, v.G, v.B, v.A });
    protected bool SetPixelsInternal(int w, int h, byte[] rgba) => Native.SetTexturePixels(ObjectId, w, h, rgba);   // generated Texture wrapper calls this

    // Mesh CONTENT (generated Mesh.SetGeometry): an unindexed TRIANGLE LIST — vertices.Length
    // must be a multiple of 3; normals (optional, same length) default to flat per-triangle,
    // uvs (optional, one per vertex) default to zeros.
    protected bool SetGeometryInternal(Vector3[] vertices, Vector3[]? normals, Vector2[]? uvs)
    {
        if (vertices.Length == 0 || vertices.Length % 3 != 0) return false;
        if (normals != null && normals.Length != vertices.Length) return false;
        if (uvs != null && uvs.Length != vertices.Length) return false;
        var v = new float[vertices.Length * 3];
        for (int i = 0; i < vertices.Length; ++i)
        {
            v[i * 3 + 0] = (float)vertices[i].X;
            v[i * 3 + 1] = (float)vertices[i].Y;
            v[i * 3 + 2] = (float)vertices[i].Z;
        }
        float[]? n = null;
        if (normals != null)
        {
            n = new float[normals.Length * 3];
            for (int i = 0; i < normals.Length; ++i)
            {
                n[i * 3 + 0] = (float)normals[i].X;
                n[i * 3 + 1] = (float)normals[i].Y;
                n[i * 3 + 2] = (float)normals[i].Z;
            }
        }
        float[]? u = null;
        if (uvs != null)
        {
            u = new float[uvs.Length * 2];
            for (int i = 0; i < uvs.Length; ++i)
            {
                u[i * 2 + 0] = (float)uvs[i].X;
                u[i * 2 + 1] = (float)uvs[i].Y;
            }
        }
        return Native.SetMeshGeometry(ObjectId, v, n, u);
    }

    // Reflected STATIC [[nuke::func]] methods (generated facade wrappers: Audio.Play, ...).
    protected static string? StaticCall(string type, string method, params object?[] args)
    {
        var (ok, ret) = Native.StaticInvoke(type, method,
                                            System.Text.Json.JsonSerializer.Serialize(args));
        if (!ok) Native.Log($"[NukeCSharp] {type}.{method} static call failed (unknown method or bad args)");
        return ret;
    }

    // Sound CONTENT (generated Audio.PlayData): encoded audio bytes -> voice handle.
    protected static double PlayAudioDataInternal(byte[] bytes, double volume, bool loop, int bus)
        => Native.PlayAudioData(bytes, volume, loop, bus);
}

// The asset database face: look assets up by NAME (internal asset name or file stem) —
// guids never appear in user code. T = a generated wrapper class named as the type.
public static class Assets
{
    public static T? Find<T>(string name) where T : NukeObject
    {
        long id = Native.FindAsset(typeof(T).Name, name);
        return id != 0 ? (T)Activator.CreateInstance(typeof(T), id)! : null;
    }
    public static NukeObject? Find(string name)
    {
        long id = Native.FindAsset("", name);
        return id != 0 ? new NukeObject(id) : null;
    }
}

// Content access through the SAME layered resolution the engine uses (raw project or
// mounted paks + mods) — scripts read game files without touching the filesystem.
public static class Packages
{
    public static byte[]? Read(string contentRelativePath) => Native.ReadContent(contentRelativePath);
    public static string? ReadText(string contentRelativePath)
    {
        var b = Native.ReadContent(contentRelativePath);
        return b != null ? System.Text.Encoding.UTF8.GetString(b) : null;
    }
}

public static class NukeTime
{
    public static double Delta   { get { Native.TimeInfo(out var d, out _); return d; } }
    public static double Elapsed { get { Native.TimeInfo(out _, out var e); return e; } }
}

// A live atom of the current world, held by STALE-SAFE id (resolved on every access —
// a dead atom degrades to no-ops/defaults, it can never touch freed memory).
public sealed unsafe class Atom
{
    public long Id { get; }
    public Atom(long id) { Id = id; }

    public static Atom? Find(string name)
    {
        long id = Native.FindAtom(name);
        return id != 0 ? new Atom(id) : null;
    }

    public Vector3 Position
    {
        get { Native.GetPosition(Id, out var v); return v; }
        set { Native.SetPosition(Id, in value); }
    }
    // The atom's LIVE Transform as a full reflected object handle (the generated
    // Transform class adds typed props — position/rotation/eulerHint/scale — and every
    // [[nuke::func]] math method via `atom.GetTransform()`; see EngineTypes.g.cs).
    public long TransformHandle => Native.AtomTransformObject(Id);

    public Quaternion Rotation   // QUATERNION — same as the engine's Transform.rotation
    {
        get
        {
            var r = Native.ObjGet(TransformHandle, "rotation");
            try
            {
                var v = r != null ? System.Text.Json.JsonSerializer.Deserialize<double[]>(r) : null;
                return v is { Length: >= 4 } ? new Quaternion(v[0], v[1], v[2], v[3]) : Quaternion.Identity;
            }
            catch { return Quaternion.Identity; }
        }
        set => Native.ObjSet(TransformHandle, "rotation",
                             System.Text.Json.JsonSerializer.Serialize(new[] { value.X, value.Y, value.Z, value.W }));
    }
    public Vector3 EulerAngles   // degrees, XYZ (the euler VIEW of the rotation)
    {
        get { double* t = stackalloc double[9]; Native.GetTransform(Id, t); return new Vector3 { X = t[3], Y = t[4], Z = t[5] }; }
        set { double* t = stackalloc double[9]; Native.GetTransform(Id, t); t[3] = value.X; t[4] = value.Y; t[5] = value.Z; Native.SetTransform(Id, t); }
    }
    public Vector3 Scale
    {
        get { double* t = stackalloc double[9]; Native.GetTransform(Id, t); return new Vector3 { X = t[6], Y = t[7], Z = t[8] }; }
        set { double* t = stackalloc double[9]; Native.GetTransform(Id, t); t[6] = value.X; t[7] = value.Y; t[8] = value.Z; Native.SetTransform(Id, t); }
    }

    // Any REFLECTED component by type name ("Light", "Rigidbody", "AudioSource", ...) —
    // the same registry the inspector and Lua ride; no per-class wrappers anywhere.
    public Component? GetComponent(string type)
    {
        long cid = Native.GetComponent(Id, type);
        return cid != 0 ? new Component(Id, cid, type) : null;
    }
    public Component? AddComponent(string type)
    {
        long cid = Native.AddComponent(Id, type);
        return cid != 0 ? new Component(Id, cid, type) : null;
    }

    // TYPED access: T is a generated wrapper (EngineTypes.g.cs, emitted by the module from
    // the reflection registry) named exactly like the reflected type — GetComponent<Light>().
    public T? GetComponent<T>() where T : Component
    {
        long cid = Native.GetComponent(Id, typeof(T).Name);
        return cid != 0 ? (T)Activator.CreateInstance(typeof(T), Id, cid)! : null;
    }
    public T? AddComponent<T>() where T : Component
    {
        long cid = Native.AddComponent(Id, typeof(T).Name);
        return cid != 0 ? (T)Activator.CreateInstance(typeof(T), Id, cid)! : null;
    }
}

// A reflected component: every [[nuke::prop]] is readable/writable by name, every
// [[nuke::func]] is callable. Values travel as JSON literals under the hood.
public class Component   // base of the GENERATED typed wrappers (EngineTypes.g.cs)
{
    readonly long _atom, _id;
    public string Type { get; }
    // protected internal: the generated typed wrappers (another assembly) derive from this.
    protected internal Component(long atom, long id, string type) { _atom = atom; _id = id; Type = type; }
    protected long OwnerAtomId => _atom;   // for generated sub-object properties (Material etc.)
    protected long OwnerCompId => _id;
    // A component-OWNED reflected instance (a MeshRenderer's material) as an object handle.
    protected long OwnedHandle(string path) => Native.ComponentObject(_atom, _id, path);

    // Helpers the GENERATED wrappers build their typed properties on.
    public Vector3 GetVector3(string prop)
    {
        var v = GetVector(prop);
        return v is { Length: >= 3 } ? new Vector3(v[0], v[1], v[2]) : default;
    }
    public Vector2 GetVector2(string prop)
    {
        var v = GetVector(prop);
        return v is { Length: >= 2 } ? new Vector2(v[0], v[1]) : default;
    }
    public Vector4 GetVector4(string prop)
    {
        var v = GetVector(prop);
        return v is { Length: >= 4 } ? new Vector4(v[0], v[1], v[2], v[3]) : default;
    }
    public Quaternion GetQuat(string prop)
    {
        var v = GetVector(prop);
        return v is { Length: >= 4 } ? new Quaternion(v[0], v[1], v[2], v[3]) : Quaternion.Identity;
    }
    public Color GetColor(string prop)
    {
        var v = GetVector(prop);
        return v is { Length: >= 4 } ? new Color(v[0], v[1], v[2], v[3])
             : v is { Length: >= 3 } ? new Color(v[0], v[1], v[2]) : default;
    }
    public bool Set(string prop, Vector3 v)    => Set(prop, new double[] { v.X, v.Y, v.Z });
    public bool Set(string prop, Vector2 v)    => Set(prop, new double[] { v.X, v.Y });
    public bool Set(string prop, Vector4 v)    => Set(prop, new double[] { v.X, v.Y, v.Z, v.W });
    public bool Set(string prop, Quaternion v) => Set(prop, new double[] { v.X, v.Y, v.Z, v.W });
    public bool Set(string prop, Color v)      => Set(prop, new double[] { v.R, v.G, v.B, v.A });
    public Atom? GetAtom(string prop)
    {
        var n = GetNumber(prop);
        return n is > 0 ? new Atom((long)n.Value) : null;
    }

    // Typed accessors (numbers/bools/strings; vectors come back as float[]).
    public double? GetNumber(string prop)
    {
        var js = Native.GetProp(_atom, _id, prop);
        return js != null && double.TryParse(js, System.Globalization.CultureInfo.InvariantCulture, out var d) ? d : null;
    }
    public bool? GetBool(string prop)
    {
        var js = Native.GetProp(_atom, _id, prop);
        return js == "true" ? true : js == "false" ? false : null;
    }
    public string? GetString(string prop)
    {
        var js = Native.GetProp(_atom, _id, prop);
        if (js == null) return null;
        try { return System.Text.Json.JsonSerializer.Deserialize<string>(js); } catch { return null; }
    }
    public double[]? GetVector(string prop)
    {
        var js = Native.GetProp(_atom, _id, prop);
        if (js == null || !js.StartsWith('[')) return null;
        try { return System.Text.Json.JsonSerializer.Deserialize<double[]>(js); } catch { return null; }
    }
    public bool Set(string prop, double v)   => Native.SetProp(_atom, _id, prop, v.ToString(System.Globalization.CultureInfo.InvariantCulture));
    public bool Set(string prop, bool v)     => Native.SetProp(_atom, _id, prop, v ? "true" : "false");
    public bool Set(string prop, string v)   => Native.SetProp(_atom, _id, prop, System.Text.Json.JsonSerializer.Serialize(v));
    public bool Set(string prop, double[] v)  => Native.SetProp(_atom, _id, prop, System.Text.Json.JsonSerializer.Serialize(v));

    // Call a [[nuke::func]] method. Args: numbers/bools/strings/float[] — returns the
    // method's result as a JSON literal string (null for void).
    // Reflected STATIC [[nuke::func]] methods (generated wrappers on component types).
    protected static string? StaticCall(string type, string method, params object?[] args)
    {
        var (ok, ret) = Native.StaticInvoke(type, method,
                                            System.Text.Json.JsonSerializer.Serialize(args));
        if (!ok) Native.Log($"[NukeCSharp] {type}.{method} static call failed (unknown method or bad args)");
        return ret;
    }

    public string? Call(string method, params object?[] args)
    {
        var (ok, ret) = Native.Invoke(_atom, _id, method,
                                      System.Text.Json.JsonSerializer.Serialize(args));
        if (!ok) Native.Log($"[NukeCSharp] {Type}.{method} call failed (unknown method or bad args)");
        return ret;
    }
}

// The game-assembly holder: a collectible ALC per load — unloading it IS the hot reload.
// SEVERAL script assemblies may live in one ALC: the game's GameScripts.dll plus a mod
// session's / each mod's own assembly (which REFERENCES GameScripts by name).
internal sealed class GameAlc : AssemblyLoadContext
{
    public GameAlc() : base("NukeGameScripts", isCollectible: true) {}
    // Bytes-loaded assemblies are invisible to default probing — register them so a mod
    // assembly's reference to "GameScripts" resolves to the loaded copy.
    public readonly Dictionary<string, Assembly> ByName = new(StringComparer.OrdinalIgnoreCase);
    protected override Assembly? Load(AssemblyName name)
    {
        // The bridge itself lives in hostfxr's ISOLATED component context (not Default) —
        // game scripts must resolve NukeEngine.Managed to THIS loaded copy, or the two
        // sides get different Electron types and nothing casts.
        if (name.Name == typeof(Bridge).Assembly.GetName().Name) return typeof(Bridge).Assembly;
        if (name.Name != null && ByName.TryGetValue(name.Name, out var known)) return known;
        return null;   // the rest falls through to the default context (framework)
    }
}

public static unsafe class Bridge
{
    static GameAlc?   _alc;
    static readonly List<Assembly> _games = new();   // GameScripts + session/mod assemblies

    // ---- native entry points (looked up by name through hostfxr) ----------------------

    [UnmanagedCallersOnly]
    public static int Init(nint apiTable)
    {
        Native.Api = Marshal.PtrToStructure<NativeApi>(apiTable);
        Native.Log("[NukeCSharp] managed bridge up (" + RuntimeInformation.FrameworkDescription + ")");
        return 1;
    }

    // Shared bytes->assembly step: loads into the CURRENT ALC and registers the name so
    // other script assemblies can reference it. Returns the Electron class count.
    static int LoadIntoAlc(nint bytes, int size, string what)
    {
        var buf = new byte[size];
        Marshal.Copy(bytes, buf, 0, size);
        _alc ??= new GameAlc();
        using var ms = new MemoryStream(buf);
        var asm = _alc.LoadFromStream(ms);
        _games.Add(asm);
        var nm = asm.GetName().Name;
        if (nm != null) _alc.ByName[nm] = asm;
        int n = 0;
        foreach (var t in asm.GetTypes())
            if (typeof(Electron).IsAssignableFrom(t) && !t.IsAbstract) ++n;
        Native.Log($"[NukeCSharp] {what}: {nm}, {n} electron class(es)");
        return n;
    }

    // Load (or hot-RELOAD) the game scripts assembly from raw bytes. Returns the number of
    // Electron classes found, -1 on failure. Existing instances die with the old ALC —
    // components recreate them on the next update.
    [UnmanagedCallersOnly]
    public static int LoadGameAssembly(nint bytes, int size)
    {
        try
        {
            UnloadInternal();
            return LoadIntoAlc(bytes, size, "game scripts loaded");
        }
        catch (ReflectionTypeLoadException e)
        {
            var why = e.LoaderExceptions.Length > 0 ? e.LoaderExceptions[0]?.Message : e.Message;
            Native.Log("[NukeCSharp] game assembly load FAILED: " + why);
            return -1;
        }
        catch (Exception e)
        {
            Native.Log("[NukeCSharp] game assembly load FAILED: " + e.Message);
            return -1;
        }
    }

    // ADD another scripts assembly into the CURRENT ALC — a mounted session's overlay
    // scripts or a mod's shipped assembly. The game's classes STAY loaded; the addition
    // references GameScripts by name (resolved through the ALC registry).
    [UnmanagedCallersOnly]
    public static int AddGameAssembly(nint bytes, int size)
    {
        try
        {
            return LoadIntoAlc(bytes, size, "scripts assembly added");
        }
        catch (ReflectionTypeLoadException e)
        {
            var why = e.LoaderExceptions.Length > 0 ? e.LoaderExceptions[0]?.Message : e.Message;
            Native.Log("[NukeCSharp] scripts assembly add FAILED: " + why);
            return -1;
        }
        catch (Exception e)
        {
            Native.Log("[NukeCSharp] scripts assembly add FAILED: " + e.Message);
            return -1;
        }
    }

    [UnmanagedCallersOnly]
    public static void UnloadGameAssembly() => UnloadInternal();

    static void UnloadInternal()
    {
        _games.Clear();
        if (_alc != null)
        {
            var weak = new WeakReference(_alc);
            _alc.Unload();
            _alc = null;
            // Nudge the collector so the old code actually goes away (best effort).
            for (int i = 0; weak.IsAlive && i < 8; ++i) { GC.Collect(); GC.WaitForPendingFinalizers(); }
        }
    }

    // ---- reflected props: the class's public fields/properties are the INSPECTOR props --

    static Type? FindClass(string name)
    {
        foreach (var asm in _games)
        {
            var t = asm.GetType(name, throwOnError: false, ignoreCase: true);
            if (t == null)   // bare name convenience: match by simple name
                foreach (var c in asm.GetTypes())
                    if (typeof(Electron).IsAssignableFrom(c) && !c.IsAbstract
                        && string.Equals(c.Name, name, StringComparison.OrdinalIgnoreCase)) { t = c; break; }
            if (t != null && typeof(Electron).IsAssignableFrom(t) && !t.IsAbstract) return t;
        }
        return null;
    }

    static bool Editable(Type t) =>
        t == typeof(float) || t == typeof(double) || t == typeof(int) || t == typeof(long)
        || t == typeof(bool) || t == typeof(string);

    // Every editable member the class DECLARES (base Electron plumbing excluded).
    static IEnumerable<(string name, Type type, Func<object, object?> get, Action<object, object?> set)> Members(Type t)
    {
        foreach (var f in t.GetFields(BindingFlags.Public | BindingFlags.Instance))
            if (f.DeclaringType != typeof(Electron) && Editable(f.FieldType))
                yield return (f.Name, f.FieldType, f.GetValue, f.SetValue);
        foreach (var p in t.GetProperties(BindingFlags.Public | BindingFlags.Instance))
            if (p.DeclaringType != typeof(Electron) && p.CanRead && p.CanWrite && Editable(p.PropertyType))
                yield return (p.Name, p.PropertyType, p.GetValue, p.SetValue);
    }

    static void ApplyProps(Electron obj, string propsJson)
    {
        if (string.IsNullOrWhiteSpace(propsJson)) return;
        try
        {
            using var doc = System.Text.Json.JsonDocument.Parse(propsJson);
            foreach (var (name, type, _, set) in Members(obj.GetType()))
                if (doc.RootElement.TryGetProperty(name, out var v))
                {
                    try
                    {
                        if      (type == typeof(float))  set(obj, (float)v.GetDouble());
                        else if (type == typeof(double)) set(obj, v.GetDouble());
                        else if (type == typeof(int))    set(obj, (int)v.GetDouble());
                        else if (type == typeof(long))   set(obj, (long)v.GetDouble());
                        else if (type == typeof(bool))   set(obj, v.GetBoolean());
                        else if (type == typeof(string)) set(obj, v.GetString() ?? "");
                    }
                    catch {}   // a stale prop of a changed type: skip, keep the default
                }
        }
        catch (Exception e) { Native.Log("[NukeCSharp] props apply error: " + e.Message); }
    }

    // JSON object of the class's editable members with their DECLARED DEFAULTS (a plain
    // template instance — field initializers run, Start does NOT). Size-query like
    // ListClasses. The editor merges its serialized overrides on top.
    [UnmanagedCallersOnly]
    public static int GetClassProps(nint classNameUtf8, nint buf, int cap)
    {
        try
        {
            var t = FindClass(Marshal.PtrToStringUTF8(classNameUtf8) ?? "");
            if (t == null) return 0;
            var tmpl = (Electron)Activator.CreateInstance(t)!;   // defaults only — no Start
            var dict = new Dictionary<string, object?>();
            foreach (var (name, _, get, _) in Members(t)) dict[name] = get(tmpl);
            var bytes = System.Text.Json.JsonSerializer.SerializeToUtf8Bytes(dict);
            if (buf != 0 && cap > 0) Marshal.Copy(bytes, 0, buf, System.Math.Min(cap, bytes.Length));
            return bytes.Length;
        }
        catch { return 0; }
    }

    // Live edit while playing: one member of one instance (value = a JSON literal).
    [UnmanagedCallersOnly]
    public static void SetProp(nint handle, nint nameUtf8, nint valueJsonUtf8)
    {
        if (handle == 0) return;
        try
        {
            if (GCHandle.FromIntPtr(handle).Target is not Electron obj) return;
            var name = Marshal.PtrToStringUTF8(nameUtf8) ?? "";
            var json = Marshal.PtrToStringUTF8(valueJsonUtf8) ?? "";
            ApplyProps(obj, "{\"" + name + "\":" + json + "}");
        }
        catch {}
    }

    // Instantiate a behaviour by class name; serialized PROPS land in the fields BEFORE
    // Start (Unity-like: Start sees the configured values). Returns a GCHandle (0 = fail).
    [UnmanagedCallersOnly]
    public static nint CreateScript(nint classNameUtf8, long atomId, nint propsJsonUtf8)
    {
        try
        {
            var name = Marshal.PtrToStringUTF8(classNameUtf8) ?? "";
            var t = FindClass(name);
            if (t == null)
            {
                Native.Log($"[NukeCSharp] class not found (or not an Electron): {name} — if the class exists, check the Console for 'C# build FAILED' (the loaded assembly may be stale)");
                return 0;
            }
            var obj = (Electron)Activator.CreateInstance(t)!;
            obj.AtomId = atomId;
            ApplyProps(obj, Marshal.PtrToStringUTF8(propsJsonUtf8) ?? "");
            try { obj.Start(); }
            catch (Exception e) { Native.Log($"[NukeCSharp] {t.Name}.Start error: {e.Message}"); }
            return GCHandle.ToIntPtr(GCHandle.Alloc(obj));
        }
        catch (Exception e)
        {
            Native.Log("[NukeCSharp] CreateScript FAILED: " + e.Message);
            return 0;
        }
    }

    // Newline-joined FULL names of every loadable Electron class (feeds the editor's class
    // picker — nobody types names by hand). Writes up to `cap` utf8 bytes into `buf`;
    // returns the byte count REQUIRED (call with cap 0 to size). 0 = no classes loaded.
    [UnmanagedCallersOnly]
    public static int ListClasses(nint buf, int cap)
    {
        try
        {
            if (_games.Count == 0) return 0;
            var sb = new System.Text.StringBuilder();
            foreach (var asm in _games)
                foreach (var t in asm.GetTypes())
                    if (typeof(Electron).IsAssignableFrom(t) && !t.IsAbstract)
                        sb.Append(t.FullName).Append('\n');
            var bytes = System.Text.Encoding.UTF8.GetBytes(sb.ToString());
            if (buf != 0 && cap > 0)
                Marshal.Copy(bytes, 0, buf, System.Math.Min(cap, bytes.Length));
            return bytes.Length;
        }
        catch { return 0; }
    }

    [UnmanagedCallersOnly]
    public static void DestroyScript(nint handle)
    {
        if (handle == 0) return;
        try { GCHandle.FromIntPtr(handle).Free(); } catch {}
    }

    [UnmanagedCallersOnly]
    public static void CallUpdate(nint handle, double dt)
    {
        if (handle == 0) return;
        try
        {
            if (GCHandle.FromIntPtr(handle).Target is Electron b) b.Update(dt);
        }
        catch (Exception e) { Native.Log("[NukeCSharp] Update error: " + e.Message); }
    }
}
