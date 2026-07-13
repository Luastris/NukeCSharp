# NukeCSharp

C# scripting backend for [NukeEngine](https://github.com/Luastris/NukeEngine-Eco)
(scripting is a SHARED service — C# loads beside Lua). Hosts the **modern .NET runtime**
(CoreCLR through hostfxr — not legacy Mono): `net8.0` floor with `RollForward`, so any
installed runtime ≥ 8 works. Dev machines need the .NET SDK (`dotnet build`); players
need only the runtime.

- Game scripts = every `.cs` under the project's `content/`, compiled into
  `GameScripts.dll` and loaded from bytes into a collectible AssemblyLoadContext.
- **Hot reload**: change a `.cs` and the assembly recompiles + reloads live — even
  mid-PIE (instances are recreated from the new code).
- A packaged game ships `GameScripts.dll` inside its pak; a MOD's scripts compile into
  their own assembly against the game's and load ADDITIVELY (game classes stay).
- `EngineTypes.g.cs` is generated from the reflection registry on every compile — the
  full typed engine API with IDE IntelliSense (the generated csproj opens in VS/Rider
  with the project context).

## The component

Add a `CSharpScript` to an atom and pick the class in the inspector (classes are
enumerated from the loaded assembly — no typing names). Public fields/properties are
inspector props: serialized, applied BEFORE `Start()`, editable live in PIE.

```csharp
using NukeEngine;

public class Rotator : Electron
{
    public double speed = 90.0;            // deg/sec — an inspector prop

    public override void Start()
    {
        Log("Rotator on " + Atom.Name);
    }

    public override void Update(double dt) // NOTE: double, not float
    {
        Rotation = Quaternion.FromAxisAngle(Vector3.Up, speed * dt) * Rotation;
    }
}
```

## API

### Electron (the script base)

`Atom` (the owner), `Position` (`Vector3`), `Rotation` (**`Quaternion`**, like the
engine), `EulerAngles` (degrees), `Scale`, `Log(text)`,
`GetComponent<T>()` / `AddComponent<T>()` / `GetComponent("TypeName")`.

### Typed components & enums (generated)

```csharp
var light = GetComponent<Light>() ?? AddComponent<Light>();
light.Intensity = 7.5f;
light.Type      = LightType.Point;         // real enums, both C++ and C# sides

var rb = GetComponent<Rigidbody>();
rb?.AddForce(new Vector3(0, 10, 0));       // [[nuke::func]] methods, typed
```

### Game & World — load worlds, spawn and destroy atoms

```csharp
var w = Game.GetWorld();                       // the CURRENT world as an object
Log(w.Name);

var enemy = w.CreateAtom("Enemy");             // new atom at the world root
enemy.Tag = "hostile";
enemy.AddComponent<MeshRenderer>();
enemy.GetTransform()!.Position = new Vector3(0, 1, 0);

var player = w.Get("Player");                  // whole-tree lookup by name
enemy.SetParent(player);                       // parenting (null = back to root)
enemy.Destroy();                               // DEFERRED: removed at end of frame (safe on self)

var boss = Prefabs.Instantiate("Prefabs/Boss.nuprefab");   // spawn a saved subtree

Game.LoadWorld("Worlds/level2.nuworld");       // switch worlds (applied at the frame boundary)
if (Game.IsPaused()) Game.SetPaused(false);
Game.Quit();                                   // Player: close; editor: ignored
```

Atoms also expose `Name`, `Tag`, `Parent`, `AddChild`; World adds `GetById`,
`Pick(origin, dir)`, `Reparent`, `QueueDestroy`, `SaveToString`/`LoadFromString`, `Clear`.

### Window / video settings

Each setter updates the window config, **persists it** (so the next launch uses it), and
applies live (in the Player; the editor skips the live change but still saves for the game):

```csharp
Game.SetResolution(1600, 900);
Game.SetWindowMode(0);       // 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen
Game.SetBorderless(true);    // windowed decoration off
Game.SetOpacity(0.9);        // whole-window 0..1 (live)
Game.SetTransparent(true);   // per-pixel alpha (takes effect on next launch)
int w = Game.WindowWidth(); int m = Game.WindowMode();   // + WindowHeight/IsBorderless/Opacity
```

### Utilities

```csharp
NukeEngine.Log.Info("mygame", "checkpoint");   // editor Console (also .Warn/.Error);
                                               // qualify it inside an Electron — the base
                                               // class has a Log(text) METHOD of its own
var c = Clock.Create();                        // pausable monotonic stopwatch
c.Pause(); c.Resume(); double s = c.Elapsed();
```

### The object model — every Model class is first-class

```csharp
var mr  = GetComponent<MeshRenderer>();
var mat = mr?.Material;                    // the live material instance
mat.Shader   = Shader.Find("world");       // assets by NAME — never guids
mat.Metallic = 0.4f;

var tex = Texture.Create();                // registers into ResDB
tex.SetPixels(64, 64, rgbaBytes);          // raw RGBA8, length == w*h*4
mat.Diffuse = tex;

var mesh = Mesh.Create();                  // procedural geometry (triangle list)
mesh.SetGeometry(vertices);                // Vector3[]; normals/uvs optional
mr.Mesh = mesh;

var t = Atom.GetTransform();               // the full reflected Transform object

byte[] data = Packages.Read("Worlds/Main.nuworld");   // content through pak layers
Audio.PlayData(wavBytes, 0.5);             // play encoded audio from memory
var any = Assets.Find<Texture>("bricks");  // typed lookup by name
```

### Static facades (generated from `[[nuke::func]]` statics)

`Audio.Play/PlayAt/Stop/...`, `Physics.Raycast/...`, `DebugDraw.Line/WireBox/...`,
`Gui.*`, `Time.*` — plus `NukeTime.Delta` / `NukeTime.Elapsed`.

### Math (mirrors the C++ originals 1:1, all double)

`Vector2/3/4` (component-wise and scalar operators, `Dot`, `Cross`, `Lerp`,
`Normalized`, direction constants), `Quaternion` (`FromEulerDeg`, `FromAxisAngle`,
`LookRotation`, `Slerp`, `Rotate`, `q * q`, `q * v`), `Color`, `Math`
(`Clamp/Clamp01/Lerp/LerpUnclamped`, `Deg2Rad/Rad2Deg`).

## Troubleshooting

- **"class not found"** → open the Console: a failed `dotnet build` prints its errors
  with file(line). The classic one after the API move to doubles: `Update(float)` no
  longer overrides — the signature is `Update(double dt)`.
- Renaming a `.cs` in the browser renames the class declaration with it (they must
  match — components bind classes by name).

## Building

Part of the [NukeEngine-Eco](https://github.com/Luastris/NukeEngine-Eco) superbuild, or
standalone: `cmake -S . -B build -G "Visual Studio 17 2022" -A x64` +
`cmake --build build --config Debug` (needs `VCPKG_ROOT`, the .NET SDK, and the engine
built first). The post-build deploys `NukeCSharp.dll` to `modules/` and builds the
managed bridge into `modules/managed/`.
