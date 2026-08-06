// NukeCSharp — the C# scripting backend, hosting .NET/CoreCLR through hostfxr. A second
// "scripting" provider: the service is SHARED, so this loads beside NukeScript/Lua.
//
// The managed bridge (modules/managed/NukeEngine.Managed.dll) exposes UnmanagedCallersOnly
// entry points. Game scripts are a separate assembly loaded FROM BYTES into a collectible
// AssemblyLoadContext: a raw project compiles <content>/**/*.cs via `dotnet build`; a packed
// game reads managed/GameScripts.dll out of the pak.
// Hot reload: the Run loop polls .cs mtimes, recompiles, reloads the ALC and bumps a
// generation counter — live CSharpScript instances recreate on their next Update.
//
// Script convention: derive from NukeEngine.Electron, override Start()/Update(dt); the
// CSharpScript component's `className` names the class (full or simple name).

#include <interface/NUKEEInteface.h>   // NUKEModule + AppInstance
#include <interface/AssetCreators.h>   // "New C# Script" browser entry
#include <interface/IconsFileTypes.h>
#include <service/iScript.h>           // the SHARED scripting service contract
#include <interface/Modular.h>         // PluginOwnedTypes: which classes came from which plugin
#include <reflect/Reflect.h>
#include <reflect/ReflectBind.h>       // generic component/prop/method access for the C# API
#include <API/Model/Atom.h>
#include <API/Model/MeshRenderer.h>    // material sub-object access from C#
#include <API/Model/Transform.h>
#include <API/Model/Time.h>
#include <API/Model/World.h>
#include <API/Model/Package.h>         // packed game: the compiled assembly from the pak
#include <API/Model/resdb.h>           // object model: assets by name, texture content
#include <render/irender.h>            // invalidateTexture after script-set pixels
#include <API/Model/Texture.h>
#include <API/Model/Shader.h>
#include <API/Model/AnimClip.h>
#include <API/Model/Audio.h>           // sound content: PlayData blob channel
#include <API/Model/Mesh.h>
#include <API/Model/Jobs.h>            // script jobs (6.10): managed delegates on the engine pool
#include <mutex>
#include <unordered_map>

#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/thread.hpp>
#include <nlohmann/json.hpp>           // reflected props: defaults + serialized overrides
#include <glm/glm.hpp>                 // transform euler <-> quat for the C# API
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <iostream>
#include <sstream>   // dotnet build output -> Console error lines
#include <map>
#include <set>
#include <string>
#include <vector>

// Windows-only for now (hostfxr paths are wchar_t; the module list is per-platform anyway).
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bfs = boost::filesystem;
using namespace nuke;
using std::cout;
using std::endl;

// ---- hostfxr (Windows: char_t = wchar_t) -------------------------------------------------------

typedef wchar_t hchar;
#define UNMANAGEDCALLERSONLY_METHOD ((const hchar*)-1)

typedef int32_t (*hostfxr_initialize_fn)(const hchar* runtimeConfig, const void* params, void** hostHandle);
typedef int32_t (*hostfxr_get_delegate_fn)(void* hostHandle, int32_t type, void** delegateOut);
typedef int32_t (*hostfxr_close_fn)(void* hostHandle);
typedef int32_t (*load_assembly_and_get_function_pointer_fn)(
	const hchar* assemblyPath, const hchar* typeName, const hchar* methodName,
	const hchar* delegateTypeName, void* reserved, void** delegateOut);
static const int32_t hdt_load_assembly_and_get_function_pointer = 5;

// Bridge entry points (must mirror Bridge.cs signatures; x64 has a single calling convention).
typedef int   (*bridge_init_fn)(void* apiTable);
typedef int   (*bridge_load_fn)(void* bytes, int size);
typedef void  (*bridge_unload_fn)();
typedef void* (*bridge_create_fn)(const char* classNameUtf8, long long atomId, const char* propsJsonUtf8);
typedef void  (*bridge_destroy_fn)(void* handle);
typedef void  (*bridge_update_fn)(void* handle, double dt);
typedef int   (*bridge_list_fn)(void* buf, int cap);
typedef int   (*bridge_getprops_fn)(const char* classNameUtf8, void* buf, int cap);
typedef void  (*bridge_setprop_fn)(void* handle, const char* nameUtf8, const char* valueJsonUtf8);
typedef void  (*bridge_event_fn)(void* handle, const char* nameUtf8, const char* payloadUtf8);
typedef int   (*bridge_liveprops_fn)(void* handle, void* buf, int cap);   // -1 = SaveMode.None

// Managed -> native function table. LAYOUT MUST MATCH Bridge.cs NativeApi exactly, and may
// be extended ONLY by appending (both sides in the same change).
struct NativeApi
{
	void (__cdecl* log)(const char* utf8);
	void (__cdecl* getPosition)(long long atomId, double* xyz);
	void (__cdecl* setPosition)(long long atomId, double* xyz);
	long long (__cdecl* findAtom)(const char* name);                                  // 0 = none
	long long (__cdecl* getComponent)(long long atomId, const char* type);            // component id, 0 = none
	long long (__cdecl* addComponent)(long long atomId, const char* type);            // create + attach
	int  (__cdecl* getProp)(long long atomId, long long compId, const char* name,
	                        char* outJson, int cap);                                  // needed len, 0 = fail
	int  (__cdecl* setProp)(long long atomId, long long compId, const char* name,
	                        const char* valueJson);                                   // 1 = ok
	int  (__cdecl* invoke)(long long atomId, long long compId, const char* method,
	                       const char* argsJson, char* outJson, int cap);             // ret len (0 = void), -1 = fail
	void (__cdecl* getTransform)(long long atomId, double* posEulerScale9);
	void (__cdecl* setTransform)(long long atomId, const double* posEulerScale9);
	void (__cdecl* timeInfo)(double* delta, double* elapsed);
	// Reflected sub-objects a component owns (e.g. a MeshRenderer's material instance).
	int  (__cdecl* getObjProp)(long long atomId, long long compId, const char* objPath,
	                           const char* name, char* outJson, int cap);
	int  (__cdecl* setObjProp)(long long atomId, long long compId, const char* objPath,
	                           const char* name, const char* valueJson);
	// Object model: handles are engine-owned ids from the ReflectBind object table.
	long long (__cdecl* createObject)(const char* type);                     // -> objId (0 = fail)
	long long (__cdecl* objectFromGuid)(const char* guid);                   // asset guid -> objId
	long long (__cdecl* findAsset)(const char* type, const char* name);      // by asset/file NAME
	int  (__cdecl* objectGuid)(long long objId, char* buf, int cap);         // -> needed len
	int  (__cdecl* objectType)(long long objId, char* buf, int cap);
	int  (__cdecl* objGet)(long long objId, const char* name, char* outJson, int cap);
	int  (__cdecl* objSet)(long long objId, const char* name, const char* valueJson);
	int  (__cdecl* objInvoke)(long long objId, const char* method, const char* argsJson,
	                          char* outJson, int cap);
	long long (__cdecl* componentObject)(long long atomId, long long compId, const char* path); // owned instance -> objId
	int  (__cdecl* setTexturePixels)(long long objId, int w, int h, const unsigned char* rgba, int len);
	int  (__cdecl* readContent)(const char* rel, char* buf, int cap);        // content bytes (raw or pak)
	// Reflected static methods: the invoke channel addressed by type name, not an instance.
	int  (__cdecl* staticInvoke)(const char* type, const char* method, const char* argsJson,
	                             char* outJson, int cap);                    // ret len (0 = void), -1 = fail
	// Mesh CONTENT: unindexed triangle list (verts required, normals/uvs optional -> computed/zeroed).
	int  (__cdecl* setMeshGeometry)(long long objId, int numVerts, const float* verts,
	                                const float* normals, const float* uvs);
	// Sound CONTENT: play encoded audio bytes (ogg/wav/mp3/flac) a script composed or read.
	double (__cdecl* playAudioData)(const unsigned char* bytes, long long len,
	                                double volume, int loop, int bus);       // voice handle, 0 = fail
	// The atom's live Transform as a reflected object handle (0 = dead atom).
	long long (__cdecl* atomTransformObject)(long long atomId);
	// Reflected methods on the Atom itself; atoms travel as stable ids, never object handles.
	int  (__cdecl* atomInvoke)(long long atomId, const char* method, const char* argsJson,
	                           char* outJson, int cap);                      // ret len (0 = void), -1 = fail
	// Script jobs: managed delegates on the engine worker pool, called back through the
	// bridge exports. Workers compute over plain data — engine/world mutation only via RunOnMain.
	long long (__cdecl* jobsRun)(long long jobId);                    // -> wait handle (0 = fail)
	int  (__cdecl* jobsDone)(long long handle);                       // 1 = finished (handle released)
	void (__cdecl* jobsWait)(long long handle);                       // block until finished + release
	void (__cdecl* jobsParallelFor)(long long jobId, int count, int grain);   // blocks; range calls
	void (__cdecl* jobsRunOnMain)(long long jobId);                   // queued to the game thread
};

static void __cdecl NativeLog(const char* utf8)
{
	cout << (utf8 ? utf8 : "") << endl;
}
static Atom* FindAtom(long long id)
{
	AppInstance* app = AppInstance::GetSingleton();
	return (app && app->currentWorld) ? app->currentWorld->GetById((long)id) : nullptr;
}
static void __cdecl NativeGetPosition(long long atomId, double* xyz)
{
	if (!xyz) return;
	xyz[0] = xyz[1] = xyz[2] = 0;
	if (Atom* a = FindAtom(atomId))
	{
		xyz[0] = a->GetTransform().position.x;
		xyz[1] = a->GetTransform().position.y;
		xyz[2] = a->GetTransform().position.z;
	}
}
static void __cdecl NativeSetPosition(long long atomId, double* xyz)
{
	if (!xyz) return;
	if (Atom* a = FindAtom(atomId))
	{
		a->GetTransform().position.x = xyz[0];
		a->GetTransform().position.y = xyz[1];
		a->GetTransform().position.z = xyz[2];
	}
}
// ---- generic reflection surface (ReflectBind — the same layer Lua binds) ----------------------

// ReflectValue <-> JSON literal (numbers, bools, strings; vectors/quats/colors as arrays;
// atom refs as their stable id number).
static std::string RvToJson(const ReflectValue& v)
{
	nlohmann::json j;
	switch (v.type)
	{
		case FT::Bool:   j = v.b; break;
		case FT::Int:    j = (long long)v.num; break;
		case FT::Float:
		case FT::Double: j = v.num; break;
		case FT::String: j = v.str; break;
		case FT::Vec2:   j = { v.v[0], v.v[1] }; break;
		case FT::Vec3:   j = { v.v[0], v.v[1], v.v[2] }; break;
		case FT::Vec4:
		case FT::Quat:
		case FT::Color:  j = { v.v[0], v.v[1], v.v[2], v.v[3] }; break;
		case FT::AtomRef: j = (unsigned long long)v.atom; break;
		case FT::ObjectRef: j = (unsigned long long)v.obj; break;   // engine object-handle id
		default: return std::string();   // void / unsupported
	}
	return j.dump();
}
static bool JsonToRv(const nlohmann::json& j, FT want, ReflectValue& out)
{
	out = ReflectValue{};
	out.type = want;
	switch (want)
	{
		case FT::Bool:   if (!j.is_boolean() && !j.is_number()) return false;
		                 out.b = j.is_boolean() ? j.get<bool>() : j.get<double>() != 0.0; return true;
		case FT::Int:
		case FT::Float:
		case FT::Double: if (!j.is_number()) return false; out.num = j.get<double>(); return true;
		case FT::String: if (!j.is_string()) return false; out.str = j.get<std::string>(); return true;
		case FT::Vec2:
		case FT::Vec3:
		case FT::Vec4:
		case FT::Quat:
		case FT::Color:
		{
			if (!j.is_array()) return false;
			for (int i = 0; i < 4 && i < (int)j.size(); ++i)
				if (j[i].is_number()) out.v[i] = j[i].get<double>();
			return true;
		}
		case FT::AtomRef: if (!j.is_number()) return false; out.atom = (unsigned long)j.get<unsigned long long>(); return true;
		case FT::ObjectRef: if (!j.is_number()) return false; out.obj = (unsigned long)j.get<unsigned long long>(); return true;
		default: return false;
	}
}

static int PutStr(const std::string& s, char* buf, int cap)
{
	if (buf && cap > 0) memcpy(buf, s.data(), (size_t)((int)s.size() < cap ? (int)s.size() : cap));
	return (int)s.size();
}
// The invoke channels use a two-call sized-string protocol (probe with a null buffer, then
// fetch). The method must EXECUTE EXACTLY ONCE: the probe runs it and caches the result JSON
// per thread, the fetch only copies that cache. Pure getters keep the plain PutStr path.
static thread_local std::string tlsInvokeCache;
static thread_local bool        tlsInvokeCached = false;
template<class Exec>   // Exec: bool(std::string& outJson) — runs the method, fills the result ("" = void)
static int InvokeOnce(Exec exec, char* outJson, int cap)
{
	if (!outJson)   // probe: execute once + cache
	{
		tlsInvokeCache.clear();
		tlsInvokeCached = false;
		if (!exec(tlsInvokeCache)) return -1;
		tlsInvokeCached = true;
		return (int)tlsInvokeCache.size();   // 0 = void (no fetch follows)
	}
	// fetch: serve the probe's cached result; execute only on a direct big-buffer call
	std::string js;
	if (tlsInvokeCached) { js.swap(tlsInvokeCache); tlsInvokeCached = false; }
	else if (!exec(js)) return -1;
	return PutStr(js, outJson, cap);
}

static long long __cdecl NativeFindAtom(const char* name)
{
	AppInstance* app = AppInstance::GetSingleton();
	if (!app || !app->currentWorld || !name) return 0;
	Atom* a = app->currentWorld->Get(name);
	return a ? (long long)a->id.id : 0;
}
static long long __cdecl NativeGetComponent(long long atomId, const char* type)
{
	Atom* a = FindAtom(atomId);
	if (!a || !type) return 0;
	Component* c = Reflect_FindComponent(a, type);
	return c ? (long long)c->id.id : 0;
}
static long long __cdecl NativeAddComponent(long long atomId, const char* type)
{
	Atom* a = FindAtom(atomId);
	if (!a || !type) return 0;
	Component* c = Reflect_AddComponent(a, type);
	return c ? (long long)c->id.id : 0;
}
static int __cdecl NativeGetProp(long long atomId, long long compId, const char* name, char* outJson, int cap)
{
	Component* c = Reflect_ResolveComponent((unsigned long)atomId, (unsigned long)compId);
	if (!c || !name || !c->GetType()) return 0;
	const Field* f = Reflect_FindField(c->GetType(), name);
	if (!f) return 0;
	// LIST props cannot ride in a ReflectValue: they cross as the serializer's JSON arrays.
	const bool isList = f->type == FT::IntList || f->type == FT::FloatList
	                 || f->type == FT::DoubleList || f->type == FT::StringList;
	std::string js = isList ? Reflect_GetFieldJson(c, *f) : RvToJson(Reflect_GetField(c, *f));
	if (js.empty()) return 0;
	if (outJson && cap > 0) memcpy(outJson, js.data(), (size_t)((int)js.size() < cap ? (int)js.size() : cap));
	return (int)js.size();
}
static int __cdecl NativeSetProp(long long atomId, long long compId, const char* name, const char* valueJson)
{
	Component* c = Reflect_ResolveComponent((unsigned long)atomId, (unsigned long)compId);
	if (!c || !name || !valueJson || !c->GetType()) return 0;
	const Field* f = Reflect_FindField(c->GetType(), name);
	if (!f) return 0;
	nlohmann::json j = nlohmann::json::parse(valueJson, nullptr, false);
	if (j.is_discarded()) return 0;
	if (f->type == FT::IntList || f->type == FT::FloatList
	 || f->type == FT::DoubleList || f->type == FT::StringList)
	{
		// LIST prop: the JSON array replaces the whole vector (ReflectValue can't hold it).
		if (!j.is_array() || !Reflect_SetFieldJson(c, *f, j.dump())) return 0;
		Reflect_ComponentFieldChanged(c, *f);
		return 1;
	}
	ReflectValue v;
	if (!JsonToRv(j, f->type, v)) return 0;
	if (!Reflect_SetField(c, *f, v)) return 0;
	Reflect_ComponentFieldChanged(c, *f);   // asset-ref writes take effect this frame
	return 1;
}
static int __cdecl NativeInvoke(long long atomId, long long compId, const char* method,
                                const char* argsJson, char* outJson, int cap)
{
	return InvokeOnce([&](std::string& out) -> bool {
		Component* c = Reflect_ResolveComponent((unsigned long)atomId, (unsigned long)compId);
		if (!c || !method || !c->GetType()) return false;
		const Method* m = Reflect_FindMethod(c->GetType(), method);
		if (!m) return false;
		nlohmann::json args = argsJson && *argsJson ? nlohmann::json::parse(argsJson, nullptr, false)
		                                            : nlohmann::json::array();
		if (args.is_discarded() || !args.is_array() || args.size() != m->params.size()) return false;
		std::vector<ReflectValue> rv(m->params.size());
		for (size_t i = 0; i < m->params.size(); ++i)
			if (!JsonToRv(args[i], m->params[i], rv[i])) return false;
		ReflectValue ret;
		if (!Reflect_Invoke(c, *m, rv.data(), rv.size(), ret)) return false;
		out = RvToJson(ret);   // "" = void
		return true;
	}, outJson, cap);
}
static void __cdecl NativeGetTransform(long long atomId, double* t9)
{
	if (!t9) return;
	for (int i = 0; i < 9; ++i) t9[i] = (i >= 6) ? 1.0 : 0.0;   // identity: pos 0, euler 0, scale 1
	if (Atom* a = FindAtom(atomId))
	{
		Transform& t = a->GetTransform();
		t9[0] = t.position.x; t9[1] = t.position.y; t9[2] = t.position.z;
		glm::dvec3 e = glm::degrees(glm::eulerAngles(glm::dquat(t.rotation.w, t.rotation.x, t.rotation.y, t.rotation.z)));
		t9[3] = e.x; t9[4] = e.y; t9[5] = e.z;
		t9[6] = t.scale.x; t9[7] = t.scale.y; t9[8] = t.scale.z;
	}
}
static void __cdecl NativeSetTransform(long long atomId, const double* t9)
{
	if (!t9) return;
	if (Atom* a = FindAtom(atomId))
	{
		Transform& t = a->GetTransform();
		t.position.x = t9[0]; t.position.y = t9[1]; t.position.z = t9[2];
		glm::dquat q = glm::dquat(glm::radians(glm::dvec3(t9[3], t9[4], t9[5])));
		t.rotation.x = q.x; t.rotation.y = q.y; t.rotation.z = q.z; t.rotation.w = q.w;
		t.eulerHint.x = t9[3]; t.eulerHint.y = t9[4]; t.eulerHint.z = t9[5];
		t.scale.x = t9[6]; t.scale.y = t9[7]; t.scale.z = t9[8];
	}
}
// Thin alias over the engine's Reflect_SubObject (one table shared by every language).
static void* ResolveSubObject(Component* c, const char* path, TypeInfo** ti)
{
	return Reflect_SubObject(c, path ? path : "", ti);
}
static int __cdecl NativeGetObjProp(long long atomId, long long compId, const char* objPath,
                                    const char* name, char* outJson, int cap)
{
	Component* c = Reflect_ResolveComponent((unsigned long)atomId, (unsigned long)compId);
	TypeInfo* ti = nullptr;
	void* obj = ResolveSubObject(c, objPath, &ti);
	if (!obj || !ti || !name) return 0;
	const Field* f = Reflect_FindField(ti, name);
	if (!f) return 0;
	std::string js = RvToJson(Reflect_GetField(obj, *f));
	if (js.empty()) return 0;
	if (outJson && cap > 0) memcpy(outJson, js.data(), (size_t)((int)js.size() < cap ? (int)js.size() : cap));
	return (int)js.size();
}
static int __cdecl NativeSetObjProp(long long atomId, long long compId, const char* objPath,
                                    const char* name, const char* valueJson)
{
	Component* c = Reflect_ResolveComponent((unsigned long)atomId, (unsigned long)compId);
	TypeInfo* ti = nullptr;
	void* obj = ResolveSubObject(c, objPath, &ti);
	if (!obj || !ti || !name || !valueJson) return 0;
	const Field* f = Reflect_FindField(ti, name);
	if (!f) return 0;
	nlohmann::json j = nlohmann::json::parse(valueJson, nullptr, false);
	if (j.is_discarded()) return 0;
	ReflectValue v;
	if (!JsonToRv(j, f->type, v)) return 0;
	return Reflect_SetField(obj, *f, v) ? 1 : 0;
}
static void __cdecl NativeTimeInfo(double* delta, double* elapsed)
{
	Time* t = Time::getSingleton();
	if (delta)   *delta   = t->delta;
	if (elapsed) *elapsed = t->elapsed;
}

// ---- the OBJECT channel (ReflectBind handle table) ---------------------------------------------

static long long __cdecl NativeCreateObject(const char* type)
{
	return type ? (long long)Reflect_CreateObject(type) : 0;
}
static long long __cdecl NativeObjectFromGuid(const char* guid)
{
	return guid ? (long long)Reflect_ObjectFromGuid(guid) : 0;
}
// Asset lookup by name (case-insensitive: internal asset names first, then file stems).
static long long __cdecl NativeFindAsset(const char* type, const char* name)
{
	return name ? (long long)Reflect_FindAsset(type ? type : "", name) : 0;
}
static int __cdecl NativeObjectGuid(long long objId, char* buf, int cap)
{
	return PutStr(Reflect_ObjectGuid((unsigned long)objId), buf, cap);
}
static int __cdecl NativeObjectType(long long objId, char* buf, int cap)
{
	return PutStr(Reflect_ObjectType((unsigned long)objId), buf, cap);
}
static int __cdecl NativeObjGet(long long objId, const char* name, char* outJson, int cap)
{
	if (!name) return 0;
	std::string js = RvToJson(Reflect_ObjectGet((unsigned long)objId, name));
	return js.empty() ? 0 : PutStr(js, outJson, cap);
}
static int __cdecl NativeObjSet(long long objId, const char* name, const char* valueJson)
{
	if (!name || !valueJson) return 0;
	TypeInfo* ti = Registry_Find(Reflect_ObjectType((unsigned long)objId));
	if (!ti) return 0;
	const Field* f = Reflect_FindField(ti, name);
	if (!f) return 0;
	nlohmann::json j = nlohmann::json::parse(valueJson, nullptr, false);
	ReflectValue v;
	if (j.is_discarded() || !JsonToRv(j, f->type, v)) return 0;
	return Reflect_ObjectSet((unsigned long)objId, name, v) ? 1 : 0;
}
static int __cdecl NativeObjInvoke(long long objId, const char* method, const char* argsJson,
                                   char* outJson, int cap)
{
	return InvokeOnce([&](std::string& out) -> bool {
		if (!method) return false;
		TypeInfo* ti = Registry_Find(Reflect_ObjectType((unsigned long)objId));
		if (!ti) return false;
		const Method* m = Reflect_FindMethod(ti, method);
		if (!m) return false;
		nlohmann::json args = argsJson && *argsJson ? nlohmann::json::parse(argsJson, nullptr, false)
		                                            : nlohmann::json::array();
		if (args.is_discarded() || !args.is_array() || args.size() != m->params.size()) return false;
		std::vector<ReflectValue> rv(m->params.size());
		for (size_t i = 0; i < m->params.size(); ++i)
			if (!JsonToRv(args[i], m->params[i], rv[i])) return false;
		ReflectValue ret;
		if (!Reflect_ObjectInvoke((unsigned long)objId, method, rv.data(), rv.size(), ret)) return false;
		out = RvToJson(ret);
		return true;
	}, outJson, cap);
}
// A component's OWNED instance as a full object handle (MeshRenderer's material).
static long long __cdecl NativeComponentObject(long long atomId, long long compId, const char* path)
{
	Component* c = Reflect_ResolveComponent((unsigned long)atomId, (unsigned long)compId);
	TypeInfo* ti = nullptr;
	void* obj = ResolveSubObject(c, path, &ti);
	return (obj && ti) ? (long long)Reflect_WrapObject(obj, ti->name) : 0;
}
// Texture CONTENT: the blob channel (pixel data doesn't fit the JSON value path).
static int __cdecl NativeSetTexturePixels(long long objId, int w, int h, const unsigned char* rgba, int len)
{
	return Reflect_SetTexturePixels((unsigned long)objId, w, h, rgba, len < 0 ? 0 : (std::size_t)len) ? 1 : 0;
}
// Content bytes (raw project or pak — the SAME resolution the engine uses everywhere).
static int __cdecl NativeReadContent(const char* rel, char* buf, int cap)
{
	if (!rel) return 0;
	std::string data;
	if (!AppInstance::GetSingleton()->ReadContent(rel, data)) return 0;
	if (buf && cap > 0) memcpy(buf, data.data(), (size_t)((int)data.size() < cap ? (int)data.size() : cap));
	return (int)data.size();
}
// Reflected STATIC methods by type name (facades: Audio.Play, Physics.Raycast, ...).
static int __cdecl NativeStaticInvoke(const char* type, const char* method, const char* argsJson,
                                      char* outJson, int cap)
{
	return InvokeOnce([&](std::string& out) -> bool {
		if (!type || !method) return false;
		TypeInfo* ti = Registry_Find(type);
		const Method* m = ti ? Reflect_FindMethod(ti, method) : nullptr;
		if (!m || !m->isStatic) return false;
		nlohmann::json args = argsJson && *argsJson ? nlohmann::json::parse(argsJson, nullptr, false)
		                                            : nlohmann::json::array();
		if (args.is_discarded() || !args.is_array() || args.size() != m->params.size()) return false;
		std::vector<ReflectValue> rv(m->params.size());
		for (size_t i = 0; i < m->params.size(); ++i)
			if (!JsonToRv(args[i], m->params[i], rv[i])) return false;
		ReflectValue ret;
		if (!Reflect_Invoke(nullptr, *m, rv.data(), rv.size(), ret)) return false;
		out = RvToJson(ret);
		return true;
	}, outJson, cap);
}
// Mesh CONTENT: the geometry blob channel (engine-side validation + normal generation).
static int __cdecl NativeSetMeshGeometry(long long objId, int numVerts, const float* verts,
                                         const float* normals, const float* uvs)
{
	return Reflect_SetMeshGeometry((unsigned long)objId, numVerts, verts, normals, uvs) ? 1 : 0;
}
// Sound CONTENT: encoded audio bytes from script (backend decodes from its own copy).
static double __cdecl NativePlayAudioData(const unsigned char* bytes, long long len,
                                          double volume, int loop, int bus)
{
	if (!bytes || len <= 0) return 0.0;
	return Audio::PlayData(bytes, (uint64_t)len, volume, loop != 0, (double)bus);
}
// The atom's live Transform wrapped as a reflected object (stale-safe: dead atom -> 0).
static long long __cdecl NativeAtomTransformObject(long long atomId)
{
	Atom* a = FindAtom(atomId);
	return a ? (long long)Reflect_WrapObject(&a->GetTransform(), "Transform") : 0;
}
// Reflected [[nuke::func]] methods on the ATOM (TypeOf<Atom>): same shape as NativeInvoke.
static int __cdecl NativeAtomInvoke(long long atomId, const char* method,
                                    const char* argsJson, char* outJson, int cap)
{
	return InvokeOnce([&](std::string& out) -> bool {
		Atom* a = FindAtom(atomId);
		if (!a || !method) return false;
		TypeInfo* ti = Registry_Find("Atom");
		const Method* m = ti ? Reflect_FindMethod(ti, method) : nullptr;
		if (!m) return false;
		nlohmann::json args = argsJson && *argsJson ? nlohmann::json::parse(argsJson, nullptr, false)
		                                            : nlohmann::json::array();
		if (args.is_discarded() || !args.is_array() || args.size() != m->params.size()) return false;
		std::vector<ReflectValue> rv(m->params.size());
		for (size_t i = 0; i < m->params.size(); ++i)
			if (!JsonToRv(args[i], m->params[i], rv[i])) return false;
		ReflectValue ret;
		if (!Reflect_Invoke(a, *m, rv.data(), rv.size(), ret)) return false;
		out = RvToJson(ret);
		return true;
	}, outJson, cap);
}

// Callbacks INTO the bridge: run the delegate stored under jobId. RunJobRange covers
// [begin, end) in ONE managed transition — per-index interop would dominate cheap bodies.
typedef int (*bridge_runjob_fn)(long long jobId);
typedef int (*bridge_runjobrange_fn)(long long jobId, int begin, int end);
static bridge_runjob_fn      gRunJob      = nullptr;
static bridge_runjobrange_fn gRunJobRange = nullptr;

// JobHandle is a C++ object, so C# holds an id into this table. Released on Wait() or the
// first Done()==true poll — a lookup miss therefore reads as done.
static std::mutex gJobsMx;
static std::unordered_map<long long, nuke::JobHandle> gJobHandles;
static long long gJobsNext = 1;

static long long __cdecl NativeJobsRun(long long jobId)
{
	if (!gRunJob) return 0;
	nuke::JobHandle h = nuke::Jobs::Schedule([jobId] { if (gRunJob) gRunJob(jobId); });
	std::lock_guard<std::mutex> lk(gJobsMx);
	const long long id = gJobsNext++;
	gJobHandles[id] = h;
	return id;
}
static int __cdecl NativeJobsDone(long long handle)
{
	std::lock_guard<std::mutex> lk(gJobsMx);
	auto it = gJobHandles.find(handle);
	if (it == gJobHandles.end()) return 1;   // released earlier = finished
	if (!it->second.Done()) return 0;
	gJobHandles.erase(it);
	return 1;
}
static void __cdecl NativeJobsWait(long long handle)
{
	nuke::JobHandle h;
	{
		std::lock_guard<std::mutex> lk(gJobsMx);
		auto it = gJobHandles.find(handle);
		if (it == gJobHandles.end()) return;
		h = it->second;               // copy out — never block the table lock on a job
		gJobHandles.erase(it);
	}
	h.Wait();
}
static void __cdecl NativeJobsParallelFor(long long jobId, int count, int grain)
{
	if (!gRunJobRange || count <= 0) return;
	if (grain <= 0)
	{
		// ~4 chunks per worker: balances load without paying a managed transition per item.
		const int lanes = nuke::Jobs::WorkerCount() + 1;
		grain = (count + lanes * 4 - 1) / (lanes * 4);
		if (grain < 1) grain = 1;
	}
	// Fan out chunk indices through Jobs::ParallelFor; it also crunches on the calling thread.
	const int g = grain, n = count;
	const int chunks = (n + g - 1) / g;
	nuke::Jobs::ParallelFor(0, chunks, 1, [jobId, g, n](int c)
	{
		const int b = c * g;
		const int e = (b + g < n) ? b + g : n;
		if (gRunJobRange) gRunJobRange(jobId, b, e);
	});
}
static void __cdecl NativeJobsRunOnMain(long long jobId)
{
	nuke::Jobs::RunOnMain([jobId] { if (gRunJob) gRunJob(jobId); });
}

static NativeApi gApi = { &NativeLog, &NativeGetPosition, &NativeSetPosition,
                          &NativeFindAtom, &NativeGetComponent, &NativeAddComponent,
                          &NativeGetProp, &NativeSetProp, &NativeInvoke,
                          &NativeGetTransform, &NativeSetTransform, &NativeTimeInfo,
                          &NativeGetObjProp, &NativeSetObjProp,
                          &NativeCreateObject, &NativeObjectFromGuid, &NativeFindAsset,
                          &NativeObjectGuid, &NativeObjectType,
                          &NativeObjGet, &NativeObjSet, &NativeObjInvoke,
                          &NativeComponentObject, &NativeSetTexturePixels, &NativeReadContent,
                          &NativeStaticInvoke, &NativeSetMeshGeometry, &NativePlayAudioData,
                          &NativeAtomTransformObject, &NativeAtomInvoke,
                          &NativeJobsRun, &NativeJobsDone, &NativeJobsWait,
                          &NativeJobsParallelFor, &NativeJobsRunOnMain };

// The hosted runtime + resolved bridge entry points.
static bridge_init_fn    gInit    = nullptr;
static bridge_load_fn    gLoad    = nullptr;
static bridge_load_fn    gAdd     = nullptr;   // AddGameAssembly: extra assembly, same ALC
static bridge_unload_fn  gUnload  = nullptr;
static bridge_create_fn  gCreate  = nullptr;
static bridge_destroy_fn gDestroy = nullptr;
static bridge_update_fn  gUpdate  = nullptr;
static bridge_list_fn    gList    = nullptr;
static bridge_getprops_fn gGetProps = nullptr;
static bridge_setprop_fn  gSetProp  = nullptr;
static bridge_event_fn    gEvent    = nullptr;   // nuke::Events delivery (6.3)
static bridge_liveprops_fn gLive    = nullptr;   // live-state capture on save (6.6)
static int               gGeneration = 0;      // bumps on every game-assembly (re)load
static bool              gHostUp = false;

// Locate the newest hostfxr.dll under %ProgramFiles%\dotnet\host\fxr\<version>\.
static std::wstring FindHostFxr()
{
	const char* pf = getenv("ProgramFiles");
	bfs::path fxr = bfs::path(pf ? pf : "C:\\Program Files") / "dotnet" / "host" / "fxr";
	boost::system::error_code ec;
	if (!bfs::exists(fxr, ec)) return L"";
	bfs::path best;
	for (bfs::directory_iterator it(fxr, ec), end; it != end && !ec; it.increment(ec))
		if (bfs::is_directory(it->path()) && (best.empty() || it->path().filename().string() > best.filename().string()))
			best = it->path();
	if (best.empty()) return L"";
	return (best / "hostfxr.dll").wstring();
}

// Start the runtime + resolve every Bridge entry point. Idempotent; false on any failure
// (the module then stays inert — CSharpScript components become no-ops with a log).
static bool EnsureHost()
{
	if (gHostUp) return true;

	std::wstring fxrPath = FindHostFxr();
	if (fxrPath.empty()) { cout << "[NukeCSharp]\t.NET runtime not found (no %ProgramFiles%\\dotnet\\host\\fxr)" << endl; return false; }
	HMODULE fxr = LoadLibraryW(fxrPath.c_str());
	if (!fxr) { cout << "[NukeCSharp]\tcan't load hostfxr.dll" << endl; return false; }
	auto init  = (hostfxr_initialize_fn)  GetProcAddress(fxr, "hostfxr_initialize_for_runtime_config");
	auto getd  = (hostfxr_get_delegate_fn)GetProcAddress(fxr, "hostfxr_get_runtime_delegate");
	auto close = (hostfxr_close_fn)       GetProcAddress(fxr, "hostfxr_close");
	if (!init || !getd || !close) { cout << "[NukeCSharp]\thostfxr exports missing" << endl; return false; }

	// The bridge ships next to the module, with the runtimeconfig.json naming its framework.
	bfs::path managedDir = bfs::path("modules") / "managed";
	bfs::path cfg = managedDir / "NukeEngine.Managed.runtimeconfig.json";
	bfs::path dll = managedDir / "NukeEngine.Managed.dll";
	boost::system::error_code ec;
	if (!bfs::exists(cfg, ec) || !bfs::exists(dll, ec))
	{ cout << "[NukeCSharp]\tmanaged bridge missing (modules/managed/) — module inert" << endl; return false; }

	void* host = nullptr;
	int rc = init(bfs::absolute(cfg).wstring().c_str(), nullptr, &host);
	if (rc != 0 && rc != 1 && rc != 2)   // 0 ok, 1/2 = already initialized (fine)
	{ cout << "[NukeCSharp]\thostfxr init failed: 0x" << std::hex << rc << std::dec << endl; return false; }
	load_assembly_and_get_function_pointer_fn loadAsm = nullptr;
	rc = getd(host, hdt_load_assembly_and_get_function_pointer, (void**)&loadAsm);
	close(host);
	if (rc != 0 || !loadAsm) { cout << "[NukeCSharp]\thostfxr delegate failed: 0x" << std::hex << rc << std::dec << endl; return false; }

	const std::wstring asmPath = bfs::absolute(dll).wstring();
	const hchar* type = L"NukeEngine.Bridge, NukeEngine.Managed";
	auto resolve = [&](const hchar* method, void** out) {
		int r = loadAsm(asmPath.c_str(), type, method, UNMANAGEDCALLERSONLY_METHOD, nullptr, out);
		if (r != 0) { cout << "[NukeCSharp]\tbridge method missing: " << (r) << endl; *out = nullptr; }
		return r == 0;
	};
	bool ok = true;
	ok &= resolve(L"Init",               (void**)&gInit);
	ok &= resolve(L"LoadGameAssembly",   (void**)&gLoad);
	ok &= resolve(L"AddGameAssembly",    (void**)&gAdd);
	ok &= resolve(L"UnloadGameAssembly", (void**)&gUnload);
	ok &= resolve(L"CreateScript",       (void**)&gCreate);
	ok &= resolve(L"DestroyScript",      (void**)&gDestroy);
	ok &= resolve(L"CallUpdate",         (void**)&gUpdate);
	ok &= resolve(L"ListClasses",        (void**)&gList);
	ok &= resolve(L"GetClassProps",      (void**)&gGetProps);
	ok &= resolve(L"SetProp",            (void**)&gSetProp);
	ok &= resolve(L"CallEvent",          (void**)&gEvent);
	ok &= resolve(L"GetLiveProps",       (void**)&gLive);
	ok &= resolve(L"RunJob",             (void**)&gRunJob);       // script jobs (6.10)
	ok &= resolve(L"RunJobRange",        (void**)&gRunJobRange);
	if (!ok) return false;
	if (gInit(&gApi) != 1) { cout << "[NukeCSharp]\tbridge Init failed" << endl; return false; }
	gHostUp = true;
	cout << "[NukeCSharp]\t.NET runtime hosted (" << bfs::path(fxrPath).parent_path().filename().string() << ")" << endl;
	return true;
}

// ---- game-scripts compilation (raw project) + loading -----------------------------------------

// Every .cs under the project content (raw sessions; the packed path reads the pak instead).
static std::vector<bfs::path> CollectCs(const bfs::path& contentRoot)
{
	std::vector<bfs::path> out;
	boost::system::error_code ec;
	if (contentRoot.empty() || !bfs::exists(contentRoot, ec)) return out;
	for (bfs::recursive_directory_iterator it(contentRoot, ec), end; it != end && !ec; it.increment(ec))
		if (!bfs::is_directory(it->path()) && it->path().extension() == ".cs")
			out.push_back(it->path());
	return out;
}

// ---- typed wrapper generation (EngineTypes.g.cs) -----------------------------------------------
// The reflection registry -> typed C# classes (GetComponent<Light>().Intensity), covering
// whatever component types are loaded when the scripts compile.

static std::string CsPascal(std::string s)
{
	if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
	return s;
}
// Emit a C# enum declaration for a reflected enum-tagged int field, one per (class, prop).
// Labels are sanitized to identifiers; digit-leading ones get a '_' prefix.
static std::string CsEnumDecl(const std::string& enumName, const std::vector<std::string>& labels)
{
	std::string decl = "public enum " + enumName + "\n{\n";
	for (size_t i = 0; i < labels.size(); ++i)
	{
		std::string lbl;
		for (char ch : labels[i])
			if (isalnum((unsigned char)ch) || ch == '_') lbl += ch;
		if (lbl.empty() || isdigit((unsigned char)lbl[0])) lbl = "_" + lbl;
		decl += "    " + lbl + " = " + std::to_string(i) + ",\n";
	}
	decl += "}\n\n";
	return decl;
}

static std::string GenerateCsWrappers()
{
	std::string enumDecls;   // namespace-level enum declarations, emitted after the classes
	std::set<std::string> emittedEnums;   // enum type names already declared (dedup field vs method enums)

	// Asset kind -> generated class: those guid string fields also get a typed object property.
	auto assetClass = [](const std::string& kind) -> const char* {
		if (kind == "mesh")       return "Mesh";
		if (kind == "material")   return "Material";
		if (kind == "texture")    return "Texture";
		if (kind == "shader")     return "Shader";
		if (kind == "postshader") return "Shader";
		if (kind == "anim")       return "AnimClip";
		return nullptr;
	};

	static const char* kReserved[] = { "Atom", "Component", "Electron", "NukeTime", "Assets", "Packages",
	                                   "Vector2", "Vector3", "Vector4", "Quaternion", "Color",
	                                   "Bridge", "Native", "NativeApi", "GameAlc", "NukeObject", "Object" };
	auto reserved = [&](const std::string& tn) {
		for (const char* r : kReserved) if (tn == r) return true;
		return tn.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos;
	};
	auto isComponent = [&](TypeInfo* ti) {
		for (TypeInfo* t = ti; t; t = Registry_Find(t->base))
			if (t->base == "Component") return true;
		return false;
	};

	// Public members of the handwritten bases (Component/NukeObject in Bridge.cs). A reflected
	// prop/method with one of these names HIDES the base member, so the generated accessors must
	// call the base explicitly (base.GetBool(...)) and the hiding member gets `new` — otherwise
	// e.g. Animator's [[nuke::func]] GetBool (SM param) shadows Component.GetBool and every
	// bool prop in that class stops compiling (CS0019 on `?? default`).
	static const char* kBaseMembers[] = { "GetNumber", "GetBool", "GetString", "GetVector",
		"GetStrings", "GetVector2", "GetVector3", "GetVector4", "GetQuat", "GetColor", "GetAtom",
		"Set", "SetStrings", "Call", "StaticCall", "OwnedHandle", "Type", "TypeName", "Guid",
		"ObjectId", "Enabled" };
	auto hidesBase = [&](const std::string& P) {
		for (const char* b : kBaseMembers) if (P == b) return true;
		return false;
	};
	auto newKw = [&](const std::string& P) { return hidesBase(P) ? std::string("new ") : std::string(); };

	// ONE field emission for both worlds (components and objects share the helper names).
	auto emitFields = [&](TypeInfo* root, const std::string& tn, std::string& o, std::set<std::string>& used) {
		for (TypeInfo* ti = root; ti; ti = Registry_Find(ti->base))
			for (const Field& f : ti->fields)
			{
				if (f.hidden) continue;
				std::string P = CsPascal(f.name);
				if (!used.insert(P).second) continue;
				const std::string n = "\"" + f.name + "\"";
				const std::string pub = "    public " + newKw(P);
				switch (f.type)
				{
					case FT::Bool:
						o += pub + "bool " + P + " { get => base.GetBool(" + n + ") ?? default; set => base.Set(" + n + ", value); }\n"; break;
					case FT::Int:
						if (!f.enumLabels.empty())
						{
							const std::string en = tn + P;
							if (emittedEnums.insert(en).second) enumDecls += CsEnumDecl(en, f.enumLabels);
							o += pub + en + " " + P + " { get => (" + en + ")(int)(base.GetNumber(" + n + ") ?? 0); set => base.Set(" + n + ", (double)(int)value); }\n";
						}
						else
							o += pub + "int " + P + " { get => (int)(base.GetNumber(" + n + ") ?? 0); set => base.Set(" + n + ", (double)value); }\n";
						break;
					case FT::Float:
						o += pub + "float " + P + " { get => (float)(base.GetNumber(" + n + ") ?? 0); set => base.Set(" + n + ", (double)value); }\n"; break;
					case FT::Double:
						o += pub + "double " + P + " { get => base.GetNumber(" + n + ") ?? 0; set => base.Set(" + n + ", value); }\n"; break;
					case FT::String:
					{
						// Asset guid field -> a TYPED object property (name = field minus "Guid").
						if (const char* cls = assetClass(f.asset))
						{
							std::string tp = P;
							if (tp.size() > 4 && tp.compare(tp.size() - 4, 4, "Guid") == 0) tp = tp.substr(0, tp.size() - 4);
							if (!used.insert(tp).second) { tp += "Asset"; used.insert(tp); }
							o += "    public " + newKw(tp) + std::string(cls) + "? " + tp + " { get { var g = base.GetString(" + n + ") ?? \"\"; return g.Length == 0 ? null : " + cls + ".FromGuid(g); } set => base.Set(" + n + ", value?.Guid ?? \"\"); }\n";
						}
						o += pub + "string " + P + " { get => base.GetString(" + n + ") ?? \"\"; set => base.Set(" + n + ", value); }\n";
						break;
					}
					case FT::Vec3:
						o += pub + "Vector3 " + P + " { get => base.GetVector3(" + n + "); set => base.Set(" + n + ", value); }\n"; break;
					case FT::Vec2:
						o += pub + "Vector2 " + P + " { get => base.GetVector2(" + n + "); set => base.Set(" + n + ", value); }\n"; break;
					case FT::Vec4:
						o += pub + "Vector4 " + P + " { get => base.GetVector4(" + n + "); set => base.Set(" + n + ", value); }\n"; break;
					case FT::Quat:
						o += pub + "Quaternion " + P + " { get => base.GetQuat(" + n + "); set => base.Set(" + n + ", value); }\n"; break;
					case FT::Color:
						o += pub + "Color " + P + " { get => base.GetColor(" + n + "); set => base.Set(" + n + ", value); }\n"; break;
					case FT::AtomRef:
						if (isComponent(root))
							o += pub + "Atom? " + P + " { get => base.GetAtom(" + n + "); set => base.Set(" + n + ", (double)(value?.Id ?? 0)); }\n";
						else { used.erase(P); }
						break;
					// LIST props cross the bridge as JSON arrays: numeric -> double[], string -> string[].
					case FT::IntList:
					case FT::FloatList:
					case FT::DoubleList:
						o += pub + "double[] " + P + " { get => base.GetVector(" + n + ") ?? System.Array.Empty<double>(); set => base.Set(" + n + ", value); }\n"; break;
					case FT::StringList:
						o += pub + "string[] " + P + " { get => base.GetStrings(" + n + ") ?? System.Array.Empty<string>(); set => base.SetStrings(" + n + ", value); }\n"; break;
					default: used.erase(P); break;   // unsupported member type: not exposed
				}
			}
	};

	// Which ObjectRef classes a wrapper can be constructed for: emitted NukeObject-channel
	// classes only (components other than Transform ride the (atom, comp) channel instead).
	auto objectClassOk = [&](const std::string& cls) -> bool {
		if (cls.empty() || reserved(cls)) return false;
		TypeInfo* t = Registry_Find(cls);
		if (!t) return false;
		return !isComponent(t) || cls == "Transform";
	};
	// FT (+ class name for ref slots) -> the C# parameter/return type ("" = unsupported).
	auto csType = [&](FT t, const std::string& cls) -> std::string {
		switch (t)
		{
			case FT::Bool: return "bool"; case FT::Int: return "int";
			case FT::Float: return "float"; case FT::Double: return "double";
			case FT::String: return "string"; case FT::Vec3: return "Vector3";
			case FT::Vec2: return "Vector2"; case FT::Vec4: return "Vector4";
			case FT::Quat: return "Quaternion"; case FT::Color: return "Color";
			case FT::AtomRef:   return "Atom";                                        // handwritten bridge class
			case FT::ObjectRef: return objectClassOk(cls) ? cls : std::string();      // generated wrapper class
			default: return std::string();
		}
	};
	auto pcls = [](const Method& m, size_t i) -> std::string {
		return i < m.paramClass.size() ? m.paramClass[i] : std::string();
	};
	// A reflected ENUM name behind an Int slot ("" = plain int) -> the generated enum type.
	auto penum = [](const Method& m, size_t i) -> std::string {
		return i < m.paramEnum.size() ? m.paramEnum[i] : std::string();
	};
	// One method body, shared by the instance and static emitters. `callPrefix` must already
	// open the call ("Call(" / "StaticCall(\"Type\", "); the body appends name, args and ')'.
	auto emitBody = [&](const Method& m, const std::string& P, bool isStatic,
	                    const std::string& callPrefix, std::string& o) -> bool {
		std::string rt = m.ret == FT::Unknown ? "void"
		               : (m.ret == FT::Int && !m.retEnum.empty()) ? m.retEnum   // enum return
		               : csType(m.ret, m.retClass);
		if (rt.empty()) return false;
		if (m.ret == FT::AtomRef || m.ret == FT::ObjectRef) rt += "?";                // null = miss
		std::string sig, pass;
		for (size_t i = 0; i < m.params.size(); ++i)
		{
			const std::string en = m.params[i] == FT::Int ? penum(m, i) : std::string();
			std::string pt = !en.empty() ? en : csType(m.params[i], pcls(m, i));
			if (pt.empty()) return false;
			std::string an = "a" + std::to_string(i);
			if (m.params[i] == FT::AtomRef || m.params[i] == FT::ObjectRef) pt += "?";   // null passes 0
			sig += std::string(i ? ", " : "") + pt + " " + an;
			std::string boxed = an;
			if (!en.empty())              boxed = "(double)(int)" + an;   // enum -> number (JSON expects an int)
			if (m.params[i] == FT::Vec3)  boxed = "new double[]{" + an + ".X, " + an + ".Y, " + an + ".Z}";
			if (m.params[i] == FT::Vec2)  boxed = "new double[]{" + an + ".X, " + an + ".Y}";
			if (m.params[i] == FT::Vec4 || m.params[i] == FT::Quat)
			                              boxed = "new double[]{" + an + ".X, " + an + ".Y, " + an + ".Z, " + an + ".W}";
			if (m.params[i] == FT::Color) boxed = "new double[]{" + an + ".R, " + an + ".G, " + an + ".B, " + an + ".A}";
			if (m.params[i] == FT::AtomRef)   boxed = "(double)(" + an + "?.Id ?? 0)";
			if (m.params[i] == FT::ObjectRef) boxed = "(double)(" + an + "?.ObjectId ?? 0)";
			pass += ", " + boxed;
		}
		o += "    public " + std::string(isStatic ? "static " : "") + newKw(P) + rt + " " + P + "(" + sig + ")\n    {\n";
		o += "        var r = " + callPrefix + "\"" + m.name + "\"" + pass + ");\n";
		if (m.ret == FT::Int && !m.retEnum.empty())   // enum return: cast the numeric result
		{
			o += "        return (" + m.retEnum + ")(r != null && double.TryParse(r, System.Globalization.CultureInfo.InvariantCulture, out var d) ? (int)d : 0);\n";
			o += "    }\n";
			return true;
		}
		switch (m.ret)
		{
			case FT::Unknown: o += "        _ = r;\n"; break;
			case FT::Bool:    o += "        return r == \"true\";\n"; break;
			case FT::Int:     o += "        return r != null && double.TryParse(r, System.Globalization.CultureInfo.InvariantCulture, out var d) ? (int)d : 0;\n"; break;
			case FT::Float:   o += "        return r != null && double.TryParse(r, System.Globalization.CultureInfo.InvariantCulture, out var d) ? (float)d : 0;\n"; break;
			case FT::Double:  o += "        return r != null && double.TryParse(r, System.Globalization.CultureInfo.InvariantCulture, out var d) ? d : 0;\n"; break;
			case FT::String:  o += "        try { return r != null ? System.Text.Json.JsonSerializer.Deserialize<string>(r) ?? \"\" : \"\"; } catch { return \"\"; }\n"; break;
			case FT::Vec3:    o += "        try { var v = r != null ? System.Text.Json.JsonSerializer.Deserialize<double[]>(r) : null; return v is { Length: >= 3 } ? new Vector3(v[0], v[1], v[2]) : default; } catch { return default; }\n"; break;
			case FT::Vec2:    o += "        try { var v = r != null ? System.Text.Json.JsonSerializer.Deserialize<double[]>(r) : null; return v is { Length: >= 2 } ? new Vector2(v[0], v[1]) : default; } catch { return default; }\n"; break;
			case FT::Quat:    o += "        try { var v = r != null ? System.Text.Json.JsonSerializer.Deserialize<double[]>(r) : null; return v is { Length: >= 4 } ? new Quaternion(v[0], v[1], v[2], v[3]) : Quaternion.Identity; } catch { return Quaternion.Identity; }\n"; break;
			case FT::Color:   o += "        try { var v = r != null ? System.Text.Json.JsonSerializer.Deserialize<double[]>(r) : null; return v is { Length: >= 4 } ? new Color(v[0], v[1], v[2], v[3]) : default; } catch { return default; }\n"; break;
			case FT::AtomRef: o += "        return r != null && double.TryParse(r, System.Globalization.CultureInfo.InvariantCulture, out var d) && d != 0 ? new Atom((long)d) : null;\n"; break;
			case FT::ObjectRef:
				o += "        return r != null && double.TryParse(r, System.Globalization.CultureInfo.InvariantCulture, out var d) && d != 0 ? new " + m.retClass + "((long)d) : null;\n"; break;
			default:          o += "        try { var v = r != null ? System.Text.Json.JsonSerializer.Deserialize<double[]>(r) : null; return v is { Length: >= 4 } ? new Vector4(v[0], v[1], v[2], v[3]) : default; } catch { return default; }\n"; break;
		}
		o += "    }\n";
		return true;
	};

	// ONE method emission for both worlds (Component.Call and NukeObject.Call match).
	auto emitMethods = [&](TypeInfo* root, std::string& o, std::set<std::string>& used) {
		for (TypeInfo* ti = root; ti; ti = Registry_Find(ti->base))
			for (const Method& m : ti->methods)
			{
				std::string P = CsPascal(m.name);
				if (m.isStatic || !used.insert(P).second) continue;
				if (!emitBody(m, P, false, "base.Call(", o)) used.erase(P);
			}
	};

	// Static reflected methods: same body, dispatched by type name.
	auto emitStatics = [&](TypeInfo* root, std::string& o, std::set<std::string>& used) {
		for (TypeInfo* ti = root; ti; ti = Registry_Find(ti->base))
			for (const Method& m : ti->methods)
			{
				std::string P = CsPascal(m.name);
				if (!m.isStatic || !used.insert(P).second) continue;
				if (!emitBody(m, P, true, "StaticCall(\"" + root->name + "\", ", o)) used.erase(P);
			}
	};

	std::string o =
		"// AUTO-GENERATED by NukeCSharp from the reflection registry - DO NOT EDIT.\n"
		"// EVERY reflected Model class is here: components (GetComponent<Light>()) AND\n"
		"// standalone objects (Material/Texture/Mesh/Shader/AnimClip: Create/Find/FromGuid,\n"
		"// typed props incl. asset refs as OBJECTS, [[nuke::func]] methods, real enums).\n"
		"#nullable enable\n"
		"namespace NukeEngine;\n\n";

	// 1) Components.
	for (const std::string& tn : Reflect_ComponentTypes())
	{
		if (reserved(tn)) continue;
		TypeInfo* root = Registry_Find(tn);
		if (!root) continue;
		o += "public sealed class " + tn + " : Component\n{\n";
		o += "    public " + tn + "(long atomId, long compId) : base(atomId, compId, \"" + tn + "\") {}\n";
		std::set<std::string> used = { tn };
		emitFields(root, tn, o, used);
		emitMethods(root, o, used);
		emitStatics(root, o, used);
		// Component-OWNED reflected instances (the sub-object table's generator side).
		if (tn == "MeshRenderer")
			o += "    public Material? Material { get { long h = OwnedHandle(\"material\"); return h != 0 ? new Material(h) : null; } }\n";
		o += "}\n\n";
	}

	// 2) Every other reflected class. Transform derives Component in the engine but is reached
	// via Atom.GetTransform(), so it rides the OBJECT channel here, not the component-id one.
	std::set<std::string> comps;
	for (const std::string& tn : Reflect_ComponentTypes()) comps.insert(tn);
	for (TypeInfo* ti : Registry_All())
	{
		const std::string on = ti->name;
		if (comps.count(on) || reserved(on) || (isComponent(ti) && on != "Transform")) continue;
		o += "public sealed class " + on + " : NukeObject\n{\n";
		o += "    public " + on + "(long objectId) : base(objectId) {}\n";
		if (ti->create)
			o += "    public static " + on + "? Create() { long h = CreateHandle(\"" + on + "\"); return h != 0 ? new " + on + "(h) : null; }\n";
		// Find/FromGuid only for ResDB assets: on a facade they would always return null.
		std::set<std::string> used = { on, "Create", "Guid", "TypeName", "ObjectId" };
		if (Reflect_IsAssetType(on))
		{
			o += "    public static " + on + "? FromGuid(string guid) { long h = HandleFromGuid(guid); return h != 0 ? new " + on + "(h) : null; }\n";
			o += "    public static " + on + "? Find(string name) { long h = HandleByName(\"" + on + "\", name); return h != 0 ? new " + on + "(h) : null; }\n";
			used.insert("FromGuid");
			used.insert("Find");
		}
		emitFields(ti, on, o, used);
		emitMethods(ti, o, used);
		emitStatics(ti, o, used);
		// CONTENT channels (blobs don't fit the reflected value path — bound by hand):
		if (on == "Texture")
			o += "    public bool SetPixels(int width, int height, byte[] rgba) => SetPixelsInternal(width, height, rgba);\n";
		if (on == "Mesh")
			o += "    public bool SetGeometry(Vector3[] vertices, Vector3[]? normals = null, Vector2[]? uvs = null) => SetGeometryInternal(vertices, normals, uvs);\n";
		if (on == "Audio")
			o += "    public static double PlayData(byte[] bytes, double volume = 1.0, bool loop = false, int bus = 1) => PlayAudioDataInternal(bytes, volume, loop, bus);\n";
		o += "}\n\n";
	}

	// An extension method, because Atom itself lives in the handwritten bridge.
	if (Registry_Find("Transform") && !reserved("Transform"))
		o += "public static class AtomTransformExtensions\n{\n"
		     "    public static Transform? GetTransform(this Atom a)\n"
		     "    { long h = a.TransformHandle; return h != 0 ? new Transform(h) : null; }\n"
		     "}\n\n";

	// Enum types used by method params/returns, deduped against the field enums above.
	for (const std::string& en : Reflect_AllEnumNames())
		if (const std::vector<std::string>* labels = Reflect_EnumLabels(en))
			if (emittedEnums.insert(en).second)
				enumDecls += CsEnumDecl(en, *labels);

	o += enumDecls;   // namespace-level enums referenced by the properties above
	return o;
}

// `dotnet build` a generated csproj into <project>/managed/bin/<asmName>.dll.
// `asmName` is "GameScripts" for a dev project; a session/mod uses its own unique name plus
// `refGameDll` to reference the game's assembly, so its scripts extend it rather than replace it.
static bool CompileGameScripts(const bfs::path& projectDir, const std::vector<bfs::path>& sources,
                               bfs::path& outDll, const std::string& asmName = "GameScripts",
                               const bfs::path& refGameDll = bfs::path())
{
	boost::system::error_code ec;
	bfs::path managed = projectDir / "managed";
	bfs::create_directories(managed, ec);
	bfs::path bridge = bfs::absolute(bfs::path("modules") / "managed" / "NukeEngine.Managed.dll");

	// The typed wrappers compile with the game scripts, but a session referencing the game's
	// assembly must NOT re-emit them: duplicates would clash with every engine type.
	std::string items;
	if (refGameDll.empty())
	{
		bfs::ofstream g(managed / "EngineTypes.g.cs", std::ios::trunc);
		if (g) g << GenerateCsWrappers();
		items += "    <Compile Include=\"" + bfs::absolute(managed / "EngineTypes.g.cs").generic_string() + "\" />\n";
	}
	for (const bfs::path& s : sources)
		items += "    <Compile Include=\"" + bfs::absolute(s).generic_string() + "\" />\n";
	std::string refs =
		"    <Reference Include=\"NukeEngine.Managed\"><HintPath>" + bridge.generic_string() + "</HintPath></Reference>\n";
	if (!refGameDll.empty())
		refs += "    <Reference Include=\"GameScripts\"><HintPath>" + bfs::absolute(refGameDll).generic_string() + "</HintPath></Reference>\n";
	std::string csproj =
		"<Project Sdk=\"Microsoft.NET.Sdk\">\n"
		"  <PropertyGroup>\n"
		"    <TargetFramework>net8.0</TargetFramework>\n"
		"    <AssemblyName>" + asmName + "</AssemblyName>\n"
		"    <Nullable>enable</Nullable>\n"
		"    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>\n"
		"    <AppendTargetFrameworkToOutputPath>false</AppendTargetFrameworkToOutputPath>\n"
		"  </PropertyGroup>\n"
		"  <ItemGroup>\n"
		+ refs + items +
		"  </ItemGroup>\n"
		"</Project>\n";
	{
		bfs::ofstream f(managed / "GameScripts.csproj", std::ios::trunc);
		if (!f) return false;
		f << csproj;
	}

	std::string cmd = "dotnet build \"" + (managed / "GameScripts.csproj").string()
	                + "\" -c Release -o \"" + (managed / "bin").string() + "\" --nologo -v q";
	// Capture the compiler's output so a failed build can name its errors in the Console.
	SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
	HANDLE rd = NULL, wr = NULL;
	CreatePipe(&rd, &wr, &sa, 0);
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOA si = {}; si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdOutput = wr; si.hStdError = wr;
	PROCESS_INFORMATION pi = {};
	std::string mcmd = cmd;
	if (!CreateProcessA(NULL, &mcmd[0], NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
	{
		CloseHandle(rd); CloseHandle(wr);
		cout << "[NukeCSharp]\tdotnet not found — install the .NET SDK to compile C# scripts" << endl;
		return false;
	}
	CloseHandle(wr);   // ours closed: the read loop ends when the child exits
	std::string output;
	char buf[4096];
	DWORD got = 0;
	while (ReadFile(rd, buf, sizeof(buf), &got, NULL) && got > 0)
		output.append(buf, got);
	CloseHandle(rd);
	WaitForSingleObject(pi.hProcess, 120000);
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
	if (code != 0)
	{
		cout << "[NukeCSharp]\tC# build FAILED (dotnet build exit " << code << "):" << endl;
		std::istringstream lines(output);
		std::string line;
		int shown = 0;
		while (std::getline(lines, line) && shown < 10)
			if (line.find("error") != std::string::npos)
			{ cout << "[NukeCSharp]\t  " << line << endl; ++shown; }
		if (!shown) cout << "[NukeCSharp]\t  (no error lines captured — run 'dotnet build managed/GameScripts.csproj' by hand)" << endl;
		return false;
	}
	outDll = managed / "bin" / (asmName + ".dll");
	return bfs::exists(outDll, ec);
}

// Push assembly BYTES into the bridge (collectible ALC — this IS the (re)load).
// Publish every loaded Electron class + its public fields into the SHARED reflection registry,
// so reflection-driven editor UI (animation/sequencer prop pickers) offers script props next to
// native ones. Re-runs on every (re)load: the registry is cleared for this host component first.
static void PublishScriptClasses()
{
	nuke::Reflect_ClearScriptClasses("CSharpScript");
	if (!gHostUp || !gList) return;
	int need = gList(nullptr, 0);
	if (need <= 0) return;
	std::string raw((size_t)need, ' ');
	gList(&raw[0], need);
	nlohmann::json names = nlohmann::json::parse(raw, nullptr, false);
	if (names.is_discarded() || !names.is_array()) return;
	for (const auto& n : names)
	{
		if (!n.is_string()) continue;
		nuke::ScriptClass sc;
		sc.name = sc.selector = n.get<std::string>();
		sc.component = "CSharpScript";
		if (gGetProps)   // defaults double as the prop schema (name -> value kind)
		{
			int pn = gGetProps(sc.selector.c_str(), nullptr, 0);
			if (pn > 0)
			{
				std::string praw((size_t)pn, ' ');
				gGetProps(sc.selector.c_str(), &praw[0], pn);
				nlohmann::json defs = nlohmann::json::parse(praw, nullptr, false);
				if (!defs.is_discarded() && defs.is_object())
					for (auto it = defs.begin(); it != defs.end(); ++it)
					{
						if (it.key().rfind("__", 0) == 0) continue;
						nuke::ScriptProp p;
						p.name = it.key();
						if      (it.value().is_number())  p.type = nuke::FT::Double;
						else if (it.value().is_boolean()) p.type = nuke::FT::Bool;
						else if (it.value().is_string())  p.type = nuke::FT::String;
						else continue;
						sc.props.push_back(p);
					}
			}
		}
		nuke::Reflect_RegisterScriptClass(sc);
	}
}

static bool LoadGameBytes(const std::string& bytes)
{
	if (!gHostUp || !gLoad || bytes.empty()) return false;
	int n = gLoad((void*)bytes.data(), (int)bytes.size());
	if (n < 0) return false;
	++gGeneration;   // live instances are stale now — components recreate next Update
	PublishScriptClasses();
	return true;
}

// Push assembly BYTES as an ADDITION into the live ALC (extends, never replaces).
static bool AddGameBytes(const std::string& bytes)
{
	if (!gHostUp || !gAdd || bytes.empty()) return false;
	if (gAdd((void*)bytes.data(), (int)bytes.size()) < 0) return false;
	PublishScriptClasses();   // the session's classes join the shared registry too
	return true;
}

// Identifier-safe assembly name for a SESSION's own scripts (unique per session/mod so
// several mods' assemblies coexist in one ALC).
static std::string SessionAsmName(const bfs::path& projectDir)
{
	std::string s;
	for (char c : projectDir.filename().string())
		if (isalnum((unsigned char)c) || c == '_') s += c;
	if (s.empty()) s = "Session";
	return "Scripts_" + s;
}

// Compile (raw) or read (packed) + load. Returns false when there are no scripts at all.
static bool BuildAndLoadGameScripts()
{
	if (!EnsureHost()) return false;
	AppInstance* app = AppInstance::GetSingleton();

	// The packed game assembly: present in a shipped game or a mounted session, absent in a dev project.
	std::string gameBytes;
	const bool packedBase = Package::Read("managed/bin/GameScripts.dll", gameBytes) && !gameBytes.empty();

	std::vector<bfs::path> cs;
	bfs::path content;
	if (!app->contentRoot.empty())
	{
		content = app->contentRoot;
		cs = CollectCs(content);
	}

	// Plain dev project: the sources ARE the game — compile into GameScripts.dll.
	if (!cs.empty() && !packedBase)
	{
		bfs::path dll;
		if (!CompileGameScripts(content.parent_path(), cs, dll)) return false;
		bfs::ifstream f(dll, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		if (!LoadGameBytes(bytes)) return false;
		cout << "[NukeCSharp]\tC# scripts compiled + loaded (" << cs.size() << " file(s))" << endl;
		return true;
	}

	if (!packedBase) return false;   // nothing anywhere

	// 1) The game's own classes FIRST — a mounted session must never lose them.
	if (!LoadGameBytes(gameBytes)) return false;
	cout << "[NukeCSharp]\tC# scripts loaded from the pak" << endl;
	std::set<std::string> have = { "gamescripts" };

	// 2) The session's own scripts: a separate assembly referencing the game's, added to it.
	if (!cs.empty())
	{
		bfs::path proj = content.parent_path();
		bfs::path refDir = proj / "managed" / "ref";
		boost::system::error_code ec;
		bfs::create_directories(refDir, ec);
		{
			bfs::ofstream rf(refDir / "GameScripts.dll", std::ios::binary | std::ios::trunc);
			if (rf) rf.write(gameBytes.data(), (std::streamsize)gameBytes.size());
		}
		const std::string asmName = SessionAsmName(proj);
		bfs::path dll;
		if (CompileGameScripts(proj, cs, dll, asmName, refDir / "GameScripts.dll"))
		{
			bfs::ifstream f(dll, std::ios::binary);
			std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
			if (AddGameBytes(bytes))
			{
				cout << "[NukeCSharp]\tsession scripts compiled + ADDED (" << cs.size() << " file(s), " << asmName << ")" << endl;
				std::string low = asmName;
				for (char& c : low) c = (char)tolower((unsigned char)c);
				have.insert(low);
			}
		}
		// A failed session compile keeps the game's classes — errors are in the Console.
	}

	// 3) Every MOD's shipped scripts assembly (unique names, any layer): additive too.
	for (const std::string& rel : Package::List("managed/bin/"))
	{
		std::string low = rel;
		for (char& c : low) c = (char)tolower((unsigned char)c);
		if (low.size() < 5 || low.compare(low.size() - 4, 4, ".dll") != 0) continue;
		std::string stem = bfs::path(rel).stem().string();
		for (char& c : stem) c = (char)tolower((unsigned char)c);
		if (have.count(stem)) continue;
		std::string bytes;
		if (Package::Read(rel, bytes) && !bytes.empty() && AddGameBytes(bytes))
		{
			cout << "[NukeCSharp]\tmod scripts ADDED: " << bfs::path(rel).filename().string() << endl;
			have.insert(stem);
		}
	}
	return true;
}

// ---- the component ------------------------------------------------------------------------------

class CSharpScript : public Component
{
	NUKE_CLASS(CSharpScript, Component, "Scripts")
public:
	[[nuke::prop(asset="csclass", label="Class")]] std::string className;   // full or simple Electron class name
	[[nuke::prop(hidden)]]                         std::string props;       // edited prop values as JSON (serialized)

	void* handle = nullptr;  // managed GCHandle (0 = not created)
	int   gen    = -1;       // generation the instance was created under (hot reload check)
	bool  failed = false;    // class missing: don't retry every frame

	CSharpScript() : Component("CSharpScript") {}

	void Init(Atom* parent) override
	{
		atom = parent;
		transform = &parent->GetTransform();
		parent->components.push_back(this);
	}

	void Update() override
	{
		if (!gHostUp || !gCreate || className.empty()) return;
		if (handle && gen != gGeneration) { gDestroy(handle); handle = nullptr; failed = false; }   // hot reload
		// A new assembly generation is the retry signal for a create that failed at boot.
		if (!handle && failed && gen != gGeneration) failed = false;
		if (!handle && !failed)
		{
			// The bridge applies the serialized prop overrides before Start runs.
			handle = gCreate(className.c_str(), (long long)(atom ? atom->id.id : 0), props.c_str());
			gen = gGeneration;
			if (!handle) failed = true;   // logged by the bridge
		}
		if (handle) gUpdate(handle, Time::getSingleton()->gameDelta);   // scaled game delta: 0 while frozen
	}

	// Forwards to the C# instance's OnEvent(string, string); game thread under the game lock.
	void OnEvent(const std::string& name, const std::string& payload) override
	{
		if (handle && gEvent) gEvent(handle, name.c_str(), payload.c_str());
	}

	// Pulls the live public fields back into the serialized props before the world saves.
	// The class picks the policy with [Save(SaveMode.All|Marked|None)] / [DontSave]; the
	// capture MERGES over the configured props, so a Marked subset keeps the rest intact.
	void OnBeforeSave() override
	{
		if (!handle || !gLive) return;   // not running — the save keeps the configured values
		const int need = gLive(handle, nullptr, 0);
		if (need <= 0) return;           // 0 = error/empty, -1 = SaveMode.None (opted out)
		std::string live((size_t)need, ' ');
		gLive(handle, &live[0], need);
		nlohmann::json cur = props.empty() ? nlohmann::json::object()
		                                   : nlohmann::json::parse(props, nullptr, false);
		if (!cur.is_object()) cur = nlohmann::json::object();
		nlohmann::json j = nlohmann::json::parse(live, nullptr, false);
		if (!j.is_object()) return;
		for (auto& kv : j.items()) cur[kv.key()] = kv.value();
		props = cur.dump();
	}

	void Destroy() override
	{
		if (handle && gDestroy) gDestroy(handle);
		handle = nullptr;
	}
	void Reset() override { Destroy(); failed = false; }   // PIE stop: a fresh instance next play
	void FixedUpdate() override {}
	void Pause() override {}

	// The class's public fields are the inspector props: defaults come from a template
	// instance on the managed side, the serialized `props` JSON overlays them.
	std::vector<DynProp> DynamicProps() override
	{
		std::vector<DynProp> out;
		if (!gHostUp || !gGetProps || className.empty()) return out;
		int need = gGetProps(className.c_str(), nullptr, 0);
		if (need <= 0) return out;
		std::string defsRaw((size_t)need, ' ');
		gGetProps(className.c_str(), &defsRaw[0], need);
		nlohmann::json defs = nlohmann::json::parse(defsRaw, nullptr, false);
		if (defs.is_discarded() || !defs.is_object()) return out;
		nlohmann::json edited = props.empty() ? nlohmann::json::object()
		                                      : nlohmann::json::parse(props, nullptr, false);
		if (edited.is_discarded()) edited = nlohmann::json::object();
		auto toVar = [](const nlohmann::json& v) {
			NukeVar r;
			if (v.is_object() && v.contains("__atomref"))      // atom REFERENCE by stable id
			{
				r.kind = NukeVar::Kind::AtomRef;
				r.refId = v["__atomref"].is_number() ? v["__atomref"].get<long long>() : 0;
			}
			else if (v.is_number())  { r.kind = NukeVar::Kind::Number; r.num = v.get<double>(); }
			else if (v.is_boolean()) { r.kind = NukeVar::Kind::Bool;   r.b   = v.get<bool>(); }
			else if (v.is_string())  { r.kind = NukeVar::Kind::String; r.str = v.get<std::string>(); }
			return r;
		};
		for (auto it = defs.begin(); it != defs.end(); ++it)
		{
			if (it.key().rfind("__", 0) == 0) continue;        // meta keys are not props
			DynProp d;
			d.name = it.key();
			d.def  = toVar(it.value());
			if (d.def.kind == NukeVar::Kind::None) continue;   // unsupported member type
			d.value = edited.contains(d.name) ? toVar(edited[d.name]) : d.def;
			if (d.value.kind == NukeVar::Kind::None) d.value = d.def;
			out.push_back(d);
		}
		return out;
	}

	void SetDynamicProp(const std::string& name, const NukeVar& v) override
	{
		nlohmann::json edited = props.empty() ? nlohmann::json::object()
		                                      : nlohmann::json::parse(props, nullptr, false);
		if (edited.is_discarded() || !edited.is_object()) edited = nlohmann::json::object();
		nlohmann::json jv;
		if      (v.kind == NukeVar::Kind::Number)  jv = v.num;
		else if (v.kind == NukeVar::Kind::Bool)    jv = v.b;
		else if (v.kind == NukeVar::Kind::String)  jv = v.str;
		else if (v.kind == NukeVar::Kind::AtomRef) jv = { { "__atomref", v.refId } };
		else return;
		edited[name] = jv;
		props = edited.dump();
		// Live instance (playing): push the edit straight into the field.
		if (handle && gSetProp) gSetProp(handle, name.c_str(), jv.dump().c_str());
	}
};

// Generated registration; must be included IN THIS TU, below the CSharpScript definition.
#include "NukeCSharp.gen.inc"   // defines NukeReflectInit_NukeCSharp()

// ---- the scripting service ----------------------------------------------------------------------

struct CSharpScriptService : public iScript
{
	const char* Language() override { return "cs"; }
	const char* HostComponent() override { return "CSharpScript"; }
	const char* Icon() override { return ICON_FT_CSHARP; }
	bool Run(const char* code, const char* chunkName) override
	{
		// Snippet eval needs Roslyn scripting — planned; class-based scripts are the v1 path.
		cout << "[NukeCSharp]\tRun('" << (chunkName ? chunkName : "snippet")
		     << "'): C# snippet eval not available yet (use Electron classes)" << endl;
		return false;
	}

	// The loaded game assembly's Electron classes — the editor's class picker reads this.
	int ListClasses(char* buf, int cap) override
	{
		return (gHostUp && gList) ? gList(buf, cap) : 0;
	}

	// The class's public fields, straight off the LOADED assembly (the same source the inspector
	// props use): "name<TAB>kind" per line, value kind taken from the managed default.
	int ListClassProps(const char* cls, char* buf, int cap) override
	{
		if (!gHostUp || !gGetProps || !cls || !*cls) return 0;
		int need = gGetProps(cls, nullptr, 0);
		if (need <= 0) return 0;
		std::string raw((size_t)need, ' ');
		gGetProps(cls, &raw[0], need);
		nlohmann::json defs = nlohmann::json::parse(raw, nullptr, false);
		if (defs.is_discarded() || !defs.is_object()) return 0;
		std::string out;
		for (auto it = defs.begin(); it != defs.end(); ++it)
		{
			if (it.key().rfind("__", 0) == 0) continue;   // meta keys are not props
			const char* kind = it.value().is_number()  ? "number"
			                 : it.value().is_boolean() ? "bool"
			                 : it.value().is_string()  ? "string" : nullptr;
			if (!kind) continue;                          // atom refs / tables are not keyable
			out += it.key() + '\t' + kind + '\n';
		}
		if (out.empty()) return 0;
		if (buf && cap >= (int)out.size()) memcpy(buf, out.data(), out.size());
		return (int)out.size();
	}

	// Which engine plugins a piece of C# needs. This backend owns both shapes its code ships in:
	// .cs source, where a plugin class is written out as an identifier, and the compiled assembly,
	// whose metadata keeps every type and member name NUL-delimited in the strings heap — so an
	// exact whole-name hit there is a real reference, not a word in a comment.
	int ModuleDeps(const char* path, char* buf, int cap) override
	{
		if (!path) return 0;
		std::string ext = bfs::path(path).extension().string();
		for (char& c : ext) c = (char)tolower((unsigned char)c);
		const bool isSrc = (ext == ".cs");
		const bool isAsm = (ext == ".dll");
		if (!isSrc && !isAsm) return 0;                // not ours: nothing was read

		bfs::ifstream f(bfs::path(path), std::ios::binary);
		if (!f) return 0;
		std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		f.close();
		if (data.empty()) return 0;
		if (isSrc)
			// Line comments out: a class named in prose is not a dependency.
			for (size_t i = 0; i + 1 < data.size(); ++i)
				if (data[i] == '/' && data[i + 1] == '/')
					while (i < data.size() && data[i] != '\n') data[i++] = ' ';
		else
		{
			bool dummy = false;
			if (HasPInvoke(data, dummy) <= 0) return 0;   // a native DLL is not this backend's
		}
		auto ident = [](char c) { return isalnum((unsigned char)c) != 0 || c == '_'; };
		std::set<std::string> mods;
		for (const auto& t : nuke::PluginOwnedTypes())
		{
			bool hit = false;
			if (isAsm)
				hit = data.find(std::string(1, '\0') + t.first + '\0') != std::string::npos;
			else
				for (size_t p = data.find(t.first); p != std::string::npos && !hit; p = data.find(t.first, p + 1))
				{
					const size_t e = p + t.first.size();
					hit = (p == 0 || !ident(data[p - 1])) && (e >= data.size() || !ident(data[e]));
				}
			if (hit) mods.insert(t.second);
		}
		std::string out;
		for (const std::string& m : mods) out += m + '\n';
		if (out.empty()) return 0;
		if (buf && cap >= (int)out.size()) memcpy(buf, out.data(), out.size());
		return (int)out.size();
	}

	// Is this piece of C# tied to one platform? IL is not — it runs on whatever .NET is under it.
	// The exception is code that leaves IL: a P/Invoke binds the assembly to native libraries and
	// so to an OS. That is readable from the metadata without running anything — the #~ stream
	// advertises which tables exist, and ImplMap exists only if something is declared extern.
	// Source is judged by the same idea one step earlier, at the attribute.
	int PlatformOf(const char* path, char* buf, int cap) override
	{
		if (!path) return 0;
		std::string ext = bfs::path(path).extension().string();
		for (char& c : ext) c = (char)tolower((unsigned char)c);
		if (ext != ".cs" && ext != ".dll") return 0;    // not ours: nothing was read
		bfs::ifstream f(bfs::path(path), std::ios::binary);
		if (!f) return 0;
		std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
		f.close();
		bool native = false;
		if (ext == ".cs")
		{
			// Comments out first: "DllImport" in prose must not bind the package to an OS.
			for (size_t i = 0; i + 1 < d.size(); ++i)
				if (d[i] == '/' && d[i + 1] == '/')
					while (i < d.size() && d[i] != '\n') d[i++] = ' ';
			native = d.find("DllImport") != std::string::npos;
		}
		else if (HasPInvoke(d, native) <= 0)
			return 0;                                   // not a managed assembly: not ours to judge

		const std::string tag = native ? nuke::Package::CurrentPlatform() : "any";
		if (buf && cap >= (int)tag.size()) memcpy(buf, tag.data(), tag.size());
		return (int)tag.size();
	}

private:
	// Walks a PE's CLI metadata far enough to read the #~ stream's `Valid` table mask. 1 = this
	// is a managed assembly (and `pinvoke` says whether ImplMap, bit 28, is present), 0 = it is
	// not one, -1 = malformed. All of this is .NET's file format, so it lives in .NET's module.
	static int HasPInvoke(const std::string& d, bool& pinvoke)
	{
		pinvoke = false;
		auto u16 = [&](size_t o) -> uint16_t
		{ return o + 2 <= d.size() ? (uint16_t)((uint8_t)d[o] | ((uint8_t)d[o + 1] << 8)) : 0; };
		auto u32 = [&](size_t o) -> uint32_t
		{
			if (o + 4 > d.size()) return 0;
			return (uint32_t)((uint8_t)d[o] | ((uint8_t)d[o + 1] << 8) | ((uint8_t)d[o + 2] << 16)
			                | ((uint32_t)(uint8_t)d[o + 3] << 24));
		};
		if (d.size() < 0x40 || u16(0) != 0x5A4D) return 0;                 // "MZ"
		const uint32_t peOff = u32(0x3C);
		if (peOff + 24 > d.size() || u32(peOff) != 0x00004550) return 0;   // "PE\0\0"
		const uint16_t nSec = u16(peOff + 6), optSize = u16(peOff + 20);
		const size_t opt = peOff + 24;
		const size_t dirs = opt + (u16(opt) == 0x20B ? 112 : 96);          // PE32+ / PE32
		const uint32_t corRva = u32(dirs + 14 * 8);                        // CLI header directory
		if (!corRva) return 0;                                             // unmanaged
		const size_t secs = opt + optSize;
		auto toOff = [&](uint32_t rva) -> size_t
		{
			for (uint16_t i = 0; i < nSec; ++i)
			{
				const size_t s = secs + (size_t)i * 40;
				const uint32_t va = u32(s + 12), rawSz = u32(s + 16), raw = u32(s + 20);
				if (rva >= va && rva < va + rawSz) return raw + (rva - va);
			}
			return 0;
		};
		const size_t cor = toOff(corRva);
		if (!cor) return -1;
		const size_t meta = toOff(u32(cor + 8));                           // MetaData RVA
		if (!meta || u32(meta) != 0x424A5342) return -1;                   // "BSJB"
		size_t p = meta + 16 + u32(meta + 12) + 4;                         // version, flags, count
		const uint16_t streams = u16(p - 2);
		for (uint16_t i = 0; i < streams && p + 8 < d.size(); ++i)
		{
			const uint32_t sOff = u32(p);
			const char* name = d.data() + p + 8;
			const size_t nameLen = strnlen(name, 32);
			if (nameLen == 2 && name[0] == '#' && name[1] == '~')
			{
				const size_t tab = meta + sOff;
				const uint64_t valid = (uint64_t)u32(tab + 8) | ((uint64_t)u32(tab + 12) << 32);
				pinvoke = (valid & (1ull << 28)) != 0;                     // ImplMap = P/Invoke
				return 1;
			}
			p += 8 + ((nameLen + 1 + 3) & ~(size_t)3);
		}
		return 1;   // managed, but no table stream found: treat as portable
	}
public:

};
static CSharpScriptService gMonoService;

// ---- the module ---------------------------------------------------------------------------------

class NukeCSharpModule : public NUKEModule
{
public:
	NukeCSharpModule()
	{
		strcpy(title, "NukeCSharp");
		strcpy(author, "Luastris");
		strcpy(description, "C# scripting: Electron classes on the hosted .NET runtime (hot reload).");
		strcpy(version, "1.0.0.0");
		strcpy(site, "https://luastris.com");
		tags = { "csharp", "dotnet", "scripting", "gameplay" };
	}

	const char* provides() override { return "scripting"; }
	void*       queryService() override { return static_cast<iScript*>(&gMonoService); }
	bool        sharedService() override { return true; }   // loads BESIDE the Lua backend

	// .cs sources are OURS: they don't ship raw — the compiled GameScripts.dll does.
	bool cookContent(const char* contentRel, const char* bytes, uint64_t size,
	                 std::vector<std::string>& outUses) override
	{
		std::string rel = contentRel ? contentRel : "";
		for (char& c : rel) c = (char)tolower((unsigned char)c);
		if (rel.size() < 3 || rel.compare(rel.size() - 3, 3, ".cs") != 0) return false;
		(void)bytes; (void)size; (void)outUses;
		return false;   // .cs SOURCES never ship — the compiled GameScripts.dll does (shipExtras)
	}

	// Ships the compiled game assembly into the pak and the managed bridge next to the module
	// (without the bridge this module is inert). The .NET runtime itself is not bundled.
	void shipExtras(const char* projectDir, std::vector<std::string>& pakFiles,
	                std::vector<std::pair<std::string, std::string>>& distFiles) override
	{
		boost::system::error_code ec;
		if (bfs::exists(bfs::path(projectDir ? projectDir : "") / "managed" / "bin" / "GameScripts.dll", ec))
			pakFiles.push_back("managed/bin/GameScripts.dll");
		distFiles.push_back({ "modules/managed", "modules/managed" });
	}

	void OnLoad() override
	{
#ifdef _WIN32
		// PIN this DLL: a hosted CLR never unloads, so an un-pinned FreeLibrary would block
		// forever in DllMain(PROCESS_DETACH). Pinned, the DLL just dies with the process.
		HMODULE self = nullptr;
		GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		                   reinterpret_cast<LPCWSTR>(&NukeReflectInit_NukeCSharp), &self);
#endif
		NukeReflectInit_NukeCSharp();   // register this module's reflected components (generated)
		cout << "[NukeCSharp]\tCSharpScript registered." << endl;
		nuke::AssetCreator csType;
		csType.label = "C# Script";
		csType.ext = ".cs";
		csType.icon = ICON_FT_CSHARP;
		csType.baseName = "NewElectron";
		csType.category = "Scripts";
		csType.textEditable = true;
		csType.syntaxLanguage = "cpp";   // closest built-in highlighting
		// %CLASSNAME% follows the file name; the editor substitutes it on create and on rename.
		csType.content =
			"using NukeEngine;\n\n"
			"public class %CLASSNAME% : Electron\n"
			"{\n"
			"    public override void Start()\n"
			"    {\n"
			"        Log(\"hello from C#\");\n"
			"    }\n\n"
			"    public override void Update(double dt)\n"
			"    {\n"
			"        // var p = Position; p.Y += dt; Position = p;\n"
			"    }\n"
			"}\n";
		nuke::RegisterAssetCreator(csType);
	}

	void Run(AppInstance* instance) override
	{
		// Host the runtime, load the scripts, then watch .cs for hot reload (raw sessions only).
		if (!BuildAndLoadGameScripts())
		{
			if (gHostUp) cout << "[NukeCSharp]\tno C# scripts yet — watching for .cs files" << endl;
		}
		std::map<std::string, std::time_t> stamps;
		auto snapshot = [&]() {
			std::map<std::string, std::time_t> s;
			boost::system::error_code ec;
			for (const bfs::path& p : CollectCs(instance->contentRoot))
				s[p.string()] = bfs::last_write_time(p, ec);
			return s;
		};
		stamps = snapshot();
		while (!stopped)
		{
			boost::this_thread::sleep_for(boost::chrono::milliseconds(1000));
			if (instance->contentRoot.empty()) continue;   // packed: nothing to watch
			auto now = snapshot();
			if (now != stamps)
			{
				stamps = now;
				cout << "[NukeCSharp]\t.cs change detected — recompiling..." << endl;
				BuildAndLoadGameScripts();
			}
		}
	}

	bool HasSettings() override { return false; }
	void Settings() override {}
	void Shutdown() override
	{
		if (gHostUp && gUnload) gUnload();
		stopped = true;
	}
};

extern "C" BOOST_SYMBOL_EXPORT NukeCSharpModule plugin;
NukeCSharpModule plugin;
