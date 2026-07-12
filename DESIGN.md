# NukeCSharp — C# scripting module (work in progress, task #66)

The SECOND scripting backend (scripting is a SHARED service since 2026-07-10 — loads
beside NukeScript/Lua). Hosts the modern .NET runtime, runs `Electron` classes as
components, hot-reloads on .cs changes, survives PIE.

## Pieces

- `managed/NukeEngine.Managed/` — DONE: the bridge assembly (this dir). Built with
  `dotnet build -c Release`; ships to `modules/managed/` (DLL + its
  `NukeEngine.Managed.runtimeconfig.json`). `Bridge` = UnmanagedCallersOnly entry points
  (Init/LoadGameAssembly/UnloadGameAssembly/CreateScript/DestroyScript/CallUpdate),
  `Electron` = the script base class, collectible ALC = hot reload.
- `src/NukeCSharp.cpp` — TODO, the native module (copy the NukeAudio/NukeScript module
  skeleton: NUKEModule subclass + `extern "C" __declspec(dllexport) NUKEModule* plugin`):
  * provides() = "scripting", sharedService() = true, queryService() = a MonoScriptService
    (iScript: Language() = "cs"; Run() = v1 stub returning false with a log — snippet eval
    needs Roslyn, later).
  * HOSTING (OnLoad): find `%ProgramFiles%\dotnet\host\fxr\<highest>\hostfxr.dll`
    (LoadLibraryW + GetProcAddress: hostfxr_initialize_for_runtime_config,
    hostfxr_get_runtime_delegate, hostfxr_close; char_t = wchar_t on Windows). Init with
    `modules/managed/NukeEngine.Managed.runtimeconfig.json`, get
    hdt_load_assembly_and_get_function_pointer, resolve each Bridge method with
    UNMANAGEDCALLERSONLY_METHOD ("NukeEngine.Bridge, NukeEngine.Managed").
  * Pass a NativeApi struct (cdecl fn pointers, layout MUST match Bridge.cs NativeApi):
    log(utf8) -> std::cout "[NukeCSharp]..."; getPosition/setPosition(atomId, float[3]) ->
    AppInstance currentScene GetById(atomId)->transform (position get/set).
  * COMPONENT `CSharpScript` (register like NukeScript's RegisterScriptComponent —
    TypeOf<CSharpScript>() + MakeField("className", ...) + t.create; see
    NukeScript.cpp:795-830): reflected prop `className`. Update(): only when
    playState == 1; lazily CreateScript(className, atom id); CallUpdate(dt).
    Destroy()/Reset(): DestroyScript. On hot reload: a generation counter bumps —
    instances with an old generation are dropped + recreated next Update (PIE-safe).
  * GAME SCRIPTS: raw project — collect `<content>/**/*.cs`; generate
    `<project>/managed/GameScripts.csproj` (net8.0, Reference NukeEngine.Managed via
    HintPath to modules/managed, `<EnableDefaultCompileItems>false` + explicit Compile
    Include of the found .cs), run `dotnet build -c Release -o <project>/managed/bin`
    (CreateProcess, wait, log stdout to the console), read GameScripts.dll bytes ->
    Bridge.LoadGameAssembly(bytes). Packed game — ReadContent("managed/GameScripts.dll")
    bytes from the pak (no dotnet SDK needed at runtime).
  * HOT RELOAD: in the module's Run loop poll .cs mtimes (~1 s); changed -> recompile ->
    LoadGameAssembly again (the bridge unloads the old ALC) -> ++generation.
  * cookContent: claim `.cs` (they DON'T ship raw); report "managed/GameScripts.dll" as a
    used path so the compiled assembly ships in the pak. (Ensure the editor pak build can
    include it: it lives under <project>/managed — packInclude or cooker-reported path.)
- `CMakeLists.txt` — TODO: copy NukeAudio's (module skeleton, D:/vcpkg classic includes,
  link NukeEngine.lib from ../NukeEngine/x64/<cfg>); add_custom_command post-build:
  `dotnet build managed/NukeEngine.Managed -c Release` and copy the DLL +
  runtimeconfig.json to `<rundir>/modules/managed/`.

## Gotchas / decisions
- dotnet on this machine: SDK 9.0.312, runtimes 8.0.22 + 9.0.14 (hostfxr rolls forward).
- hostfxr wchar_t paths; UNMANAGEDCALLERSONLY_METHOD = (void*)-1.
- The native NativeApi struct layout must match Bridge.cs exactly (3 cdecl pointers now).
- Managed DLL loads from a PATH (default ALC); GAME assembly loads from BYTES into the
  collectible ALC (works for pak-shipped games; nothing extracts).
- Atom ids are `long` (Atom::id.id) — the managed side keys everything by them.

## Object model v2 (task #67 — DONE 2026-07-11, verified in PIE in BOTH languages)

USER RULE: every reflected Model class (Texture, Mesh, Material, AnimClip, Shader, ...)
must be a FIRST-CLASS citizen in C++ AND C# AND Lua: create, edit props, set content,
assign to components. Through reflection — never per-class hand wiring.

Shipped (engine layer first, backends mounted on it):
1. ReflectBind object-HANDLE table (ENGINE-owned, language-neutral): Reflect_CreateObject
   (asset types auto-register into ResDB under a fresh guid), Reflect_WrapObject (non-owning,
   deduped), Reflect_ObjectFromGuid, Reflect_FindAsset(type, name) — case-insensitive by
   internal asset name then file stem (user code NEVER sees guids), Reflect_ObjectGet/Set/
   Invoke (Set fires Material::Resolve after asset-field writes), Reflect_SubObject /
   Reflect_ComponentObject ("material" on MeshRenderer — ONE table for every language),
   Reflect_ComponentFieldChanged (mesh/material re-resolution after component asset writes).
2. Assignment paths: asset-guid STRING fields carry the object semantics — C# gets typed
   object properties (field name minus "Guid"), Lua aliases (mr.mesh, mat.shader) and
   accepts an object handle anywhere a guid string is expected. (No FT::ObjectRef needed.)
3. CONTENT channels (blobs, hand-bound per language — they don't fit the JSON value path):
   Reflect_SetTexturePixels (RGBA8 + invalidateTexture), Reflect_SetMeshGeometry (unindexed
   triangle list; flat normals computed when omitted; version bump re-uploads GPU buffers),
   Audio::PlayData (encoded bytes from memory — script-composed WAV/OGG plays, no file).
4. NukeCSharp: NativeApi += createObject/objectFromGuid/findAsset/objectGuid/objectType/
   objGet/objSet/objInvoke/componentObject/setTexturePixels/readContent/staticInvoke/
   setMeshGeometry/playAudioData (append-only, both sides). Generator emits EVERY registry
   type: components + standalone classes (Create/Find/FromGuid), typed asset-object props,
   real enums, STATIC facades (Audio/Physics/DebugDraw/... via staticInvoke), and the
   content special-cases (Texture.SetPixels / Mesh.SetGeometry / Audio.PlayData) through
   protected NukeObject helpers (Native is internal to the bridge assembly).
5. NukeScript: the SAME handle layer in Lua — nuke.Object userdata (__index/__newindex over
   the registry, valid/guid/type builtins, tex:setPixels, mesh:setGeometry),
   nuke.<Type>.Create/Find/FromGuid for every creatable non-component,
   nuke.Assets.find(name[, type]), nuke.Packages.read(rel), nuke.Audio.PlayData(bytes).
