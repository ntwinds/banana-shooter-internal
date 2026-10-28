# Updating offsets

`offsets.hpp` is required by the build. It contains fields, method RVAs, IL2CPP
type-information slots and the supported binary identity. `offsets.json` is an
optional generated report and is not needed to compile.

## Generate

Use matching `dump.cs`, `script.json` and `GameAssembly.dll` inputs:

```powershell
python tools/dump_offsets.py --artifacts "C:\path\to\Il2CppDumper-output" --binary "C:\path\to\GameAssembly.dll"
```

The tool uses Python's standard library. It preserves namespace and overload
identity, checks selected methods against `script.json`, and writes both outputs
into the repository root. Use `--output` to choose another directory.

Field offsets, static-storage offsets and module-relative addresses are separate
values. A static field offset must be applied to the class's static storage;
adding it directly to the module base is incorrect.

## Validate a new game build

Regenerating the header does not update every native hook assumption:

1. Check each selected method's native calling convention and signature.
2. Update the expected `PlayerMovement.Look` entry bytes in `src/main.cpp`.
3. Check the two forward-vector call sites in `src/silent_aim.cpp` and update
   their return RVAs. Each must still call `Transform.get_forward` from the
   intended local shot path.
4. Confirm player, camera, weapon and collection layouts against the new build.
5. Rebuild, run the checks, and verify behavior in the matching game version.

The generator identifies existing metadata; it does not recover stripped fields,
validate live objects, or discover replacement shot paths automatically.

Do not commit game binaries, metadata dumps, analysis databases or local reports.
The repository ignore rules exclude the common generated filenames.
