# Banana Internal

Windows x64 internal DLL and loader for the pinned Banana Shooter IL2CPP build.
The interface and ESP render through the game's Direct3D 11 swapchain using Dear ImGui.

## Features

| Tab | Controls |
| --- | --- |
| Visuals and aim | Player boxes, names, health bars, outlines, team filter, aim FOV and smoothing |
| Movement | Fly/noclip, fly speed, movement and jump multipliers, camera FOV |
| Weapons | Rapid-fire multiplier, infinite ammo, current ammo and local-host diagnostics |

Silent aim targets the verified hitscan-gun path without rotating the camera.
Normal aim and silent aim are mutually exclusive. Weapon spread is preserved.
Fly, movement overrides and weapon overrides start disabled.

## Requirements

- Windows x64
- Visual Studio 2022 or newer with **Desktop development with C++** and a Windows SDK
- CMake 3.20 or newer
- A Direct3D 11 device for the renderer integration test

Dependencies are included in `vendor/`; the build does not download packages.
Python is only needed when regenerating offsets.

## Build

From PowerShell in the repository directory:

```powershell
.\build.cmd
```

Or run the steps separately:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Outputs:

- `build/Release/BananaInternalUI.dll`
- `build/Release/BananaLoader.exe`

For a build without the test executables, configure with `-DBUILD_TESTING=OFF`.
The renderer test uses its own hidden D3D11 host and does not access the game.

## Run

1. Start the matching Banana Shooter build.
2. Run `launch.cmd`.
3. Press **Insert** to open the controls. The menu starts closed.

The loader selects a single running `Banana Shooter.exe`. To select a process explicitly:

```powershell
.\build\Release\BananaLoader.exe .\build\Release\BananaInternalUI.dll 12345
```

### Controls

| Key | Action |
| --- | --- |
| Insert | Open or close ImGui |
| Escape | Close ImGui when open; otherwise pass through to the game |
| F1 | Toggle ESP |
| F2 | Toggle normal aim |
| F3 | Toggle team filtering |
| F4 | Toggle fly/noclip |
| Right mouse, held | Engage normal aim with ImGui closed |
| End, with ImGui closed | Deactivate |

Fly uses the game's existing noclip movement: WASD moves, jump ascends, crouch
 descends and Shift boosts. The reset button disables movement and camera overrides.

Run `stop.cmd` to deactivate through the loader. The DLL remains loaded until
 game exit; fully restart the game before loading a different build.

## Compatibility and limitations

The bundled offsets target this `GameAssembly.dll` SHA-256:

```text
7fe0a424d7eaedfaab6b48073f9bfae7ba3c04e95c3f427cdb73a648fd94e86f
```

The DLL checks the file hash, PE identity and expected function entry bytes before
installing the game hooks. A game update requires new offsets and call-site checks.
See [Updating offsets](docs/OFFSETS.md).

- Aim has no visibility test or projectile prediction. Its range is 500 world units.
- Silent aim supports the identified hitscan gun calls; projectile, laser and knife paths are excluded.
- Rapid fire is capped at 60 client attempts per second. Remote servers control accepted fire rate and ammo.
- Local-host weapon overrides apply only to the matching local player's server weapon.
- Original movement, camera and outline settings restore through game callbacks. Restoration can be deferred while callbacks are paused.
- No Lua executor is included. A game-owned Lua execution entry point was not found in this client build.

New weapon, silent-aim and movement options require in-game verification for the
matching build. Passing the automated checks does not establish hit registration
or server acceptance.

## Diagnostics

The DLL writes `BananaInternalUI.log` and `BananaDiagnostics-<PID>.json` beside the
loaded DLL. The menu shows player/camera counts, target eligibility, projection,
aim state and faults. Runtime files and build outputs are ignored by Git.

If a feature is missing, check the selected tab, restart the game to release the
previous DLL, and launch the binary from `build/Release`.

## Dependencies

- Dear ImGui v1.92.5
- MinHook v1.3.4

Versions, source revisions and license locations are listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Their original license notices
are retained in the vendored source.
