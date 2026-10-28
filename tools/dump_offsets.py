"""Reproduce source-bound IL2CPP offsets from Il2CppDumper output.

Member/static field offsets and native method RVAs are separate record kinds.
The output is static metadata evidence, not live-game validation.
"""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path

TARGETS = {
    "Safe.SafeInt": "SafeInt",
    "Weapon.ActiveWeapon": "ActiveWeapon",
    "Weapon.WeaponStats.WeaponStat": "WeaponStat",
    "Multiplayer.Entity.Server.ServerPlayer": "ServerPlayer",
    "Multiplayer.NetworkManager": "NetworkManager",
    "Movement.PlayerMovement": "PlayerMovement",
    "Multiplayer.Entity.Client.PlayerState": "PlayerState",
    "UnityEngine.Camera": "Camera",
    "UnityEngine.Transform": "Transform",
    "Outline": "Outline",
    "UnityEngine.Behaviour": "Behaviour",
    "UnityEngine.Component": "Component",
    "UnityEngine.Cursor": "Cursor",
    "PlayerCameraController.MoveCamera": "MoveCamera",
    "Weapon.Firearms": "Firearms",
    "Weapon.WeaponManager": "WeaponManager",
    "Movement.NoClipMovement": "NoClipMovement",
}
ALIASES = {
    "SafeInt": {"GetValue": "GetValue", "SetValue": "SetValue"},
    "ActiveWeapon": {"DoAttack": "DoAttack"},
    "NetworkManager": {"get_Instance": "get_Instance", "CantPlay": "CantPlay"},
    "PlayerMovement": {"Look": "Look", "ChangeNoClip": "ChangeNoClip"},
    "PlayerState": {"get_PlayerStates": "get_PlayerStates"},
    "Transform": {"get_position": "get_position", "get_forward": "get_forward", "set_eulerAngles": "set_eulerAngles", "set_localEulerAngles": "set_localEulerAngles"},
    "Behaviour": {"get_enabled": "get_enabled", "set_enabled": "set_enabled"},
    "Camera": {"get_main": "get_main", "WorldToScreenPoint": "WorldToScreenPoint", "get_pixelWidth": "get_pixelWidth", "get_pixelHeight": "get_pixelHeight", "get_fieldOfView": "get_fieldOfView", "set_fieldOfView": "set_fieldOfView"},
    "Component": {"get_transform": "get_transform"},
    "Cursor": {"get_lockState": "get_lockState", "set_lockState": "set_lockState", "get_visible": "get_visible", "set_visible": "set_visible"},
    "MoveCamera": {"get_Instance": "get_Instance", "LateUpdate": "LateUpdate"},
    "Firearms": {"DoAttack": "DoAttack", "GunShoot": "GunShoot", "get_camera": "get_camera"},
}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def ident(name):
    name = re.sub(r"<([^>]+)>k__BackingField", r"\1", name)
    name = re.sub(r"\W", "_", name)
    return ("_" + name) if name[:1].isdigit() else name


def parse_dump(text, script):
    by_rva = {}
    for item in script.get("ScriptMethod", []):
        by_rva.setdefault(item["Address"], []).append(item)
    namespace = ""
    current = None
    section = None
    pending_rva = None
    records = {}
    for number, line in enumerate(text.splitlines(), 1):
        if line.startswith("// Namespace:"):
            namespace = line.partition(":")[2].strip()
            current = None
            continue
        match = re.match(r"(?:public|private|internal|protected).*?\b(?:class|struct|enum|interface)\s+([^\s:]+).*// TypeDefIndex: (\d+)", line)
        if match:
            full_name = (namespace + "." if namespace else "") + match[1]
            current = None
            if full_name in TARGETS:
                current = {"name": match[1], "namespace": namespace, "qualified_name": full_name,
                           "type_definition_index": int(match[2]), "fields": [], "methods": []}
                records[full_name] = current
            section = None
            pending_rva = None
            continue
        if current is None:
            continue
        if line.strip() == "// Fields":
            section = "fields"
        elif line.strip() == "// Methods":
            section = "methods"
        elif line.strip() == "// Properties":
            section = "properties"
        elif section == "fields":
            field = re.match(r"\s*(.+?)\s+([^\s;]+); // (0x[0-9A-Fa-f]+)", line)
            if field:
                declaration, name, value = field.groups()
                kind = "static_field_offset" if re.search(r"\bstatic\b", declaration) else "member_offset"
                if re.search(r"\bconst\b", declaration):
                    kind = "constant"
                current["fields"].append({"name": name, "normalized_name": ident(name),
                                           "declaration": declaration, "kind": kind,
                                           "value": int(value, 16), "source_line": number})
        elif section == "methods":
            rva = re.search(r"// RVA: (0x[0-9A-Fa-f]+)", line)
            if rva:
                pending_rva = int(rva[1], 16)
                continue
            if pending_rva is not None and "(" in line and not line.lstrip().startswith(("//", "[")):
                signature = line.strip().removesuffix(" { }")
                method_name = re.search(r"([^\s(]+)\(", signature)[1]
                matches = [item for item in by_rva.get(pending_rva, [])
                           if item["Name"] == current["qualified_name"] + "$$" + method_name]
                current["methods"].append({"name": method_name, "signature": signature,
                                            "kind": "method_rva", "value": pending_rva,
                                            "source_line": number,
                                            "script_json_matches": matches,
                                            "identity": current["qualified_name"] + "::" + signature})
                pending_rva = None
    if set(records) != set(TARGETS):
        raise ValueError("Missing target classes: " + str(set(TARGETS) - set(records)))
    return records


def pe_identity(data):
    if data[:2] != b"MZ":
        raise ValueError("Expected a PE file")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    machine, _, timestamp = struct.unpack_from("<HHI", data, pe + 4)
    if machine != 0x8664:
        raise ValueError("Expected AMD64 GameAssembly")
    image_size = struct.unpack_from("<I", data, pe + 24 + 56)[0]
    return {"machine": machine, "time_date_stamp": timestamp, "size_of_image": image_size}


def generate(artifacts, binary, output):
    dump_bytes = (artifacts / "dump.cs").read_bytes()
    script_bytes = (artifacts / "script.json").read_bytes()
    binary_bytes = binary.read_bytes()
    script = json.loads(script_bytes)
    records = parse_dump(dump_bytes.decode("utf-8-sig"), script)
    for qualified, record in records.items():
        matches = [item for item in script.get("ScriptMetadata", []) if item["Name"] == qualified + "_TypeInfo"]
        if len(matches) == 1:
            record["type_info"] = matches[0]
    pe = pe_identity(binary_bytes)
    report = {"schema_version": 1, "binary": binary.name, "binary_sha256": digest(binary_bytes),
              "pe": pe, "dump_cs_sha256": digest(dump_bytes), "script_json_sha256": digest(script_bytes),
              "evidence_kind": "Il2CppDumper static metadata",
              "limitations": ["Not live-game validation", "Static field offsets are relative to the class static storage, not module base",
                              "Method RVAs require adding the loaded GameAssembly base", "No unknown or zero-valued fields are guessed"],
              "types": list(records.values())}
    lines = ["#pragma once", "#include <cstdint>", "", "// Generated by tools/dump_offsets.py; do not edit.",
             "namespace offsets {",
             'inline constexpr char GameAssemblySha256[] = "' + report["binary_sha256"] + '";',
             f'inline constexpr std::uint32_t GameAssemblyTimestamp = 0x{pe["time_date_stamp"]:X};',
             f'inline constexpr std::uint32_t GameAssemblySizeOfImage = 0x{pe["size_of_image"]:X};']
    for qualified, short in TARGETS.items():
        record = records[qualified]
        lines += [f"namespace {short} {{", "namespace fields {"]
        aliases = []
        for field in record["fields"]:
            name = field["normalized_name"]
            # All fields retain explicit storage category below.
            lines.append(f'inline constexpr std::uintptr_t {name} = 0x{field["value"]:X}; // {field["kind"]}, dump.cs:{field["source_line"]}')
            if field["kind"] == "member_offset":
                aliases.append(f"inline constexpr auto {name} = fields::{name};")
        lines += ["}", "namespace methods {"]
        for method in record["methods"]:
            symbol = ident(method["name"]) + "_" + digest(method["signature"].encode())[:8]
            method["cpp_symbol"] = f"offsets::{short}::methods::{symbol}"
            lines.append(f'inline constexpr std::uintptr_t {symbol} = 0x{method["value"]:X}; // dump.cs:{method["source_line"]}')
        lines.append("}")
        for alias, method_name in ALIASES.get(short, {}).items():
            matches = [method for method in record["methods"] if method["name"] == method_name]
            if short == "Camera" and method_name == "WorldToScreenPoint":
                matches = [method for method in matches if "(Vector3 position)" in method["signature"]]
            if len(matches) != 1 or not matches[0]["script_json_matches"]:
                raise ValueError("Missing or ambiguous method alias " + short + "::" + alias)
            aliases.append("inline constexpr auto " + alias + " = " + matches[0]["cpp_symbol"] + ";")
        lines += aliases + ["}"]
        if "type_info" in record:
            lines.insert(len(lines) - 1, f'inline constexpr std::uintptr_t TypeInfoRva = 0x{record["type_info"]["Address"]:X}; // module RVA of IL2CPP class pointer slot')
    lines.append("}\n")
    output.mkdir(parents=True, exist_ok=True)
    (output / "offsets.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (output / "offsets.hpp").write_text("\n".join(lines), encoding="utf-8")
    print("Generated offsets.hpp and offsets.json; SHA256=" + report["binary_sha256"])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--artifacts", type=Path, required=True, help="Il2CppDumper output directory")
    parser.add_argument("--binary", type=Path, required=True, help="Matching AMD64 GameAssembly.dll")
    parser.add_argument("--output", type=Path, default=Path(__file__).resolve().parents[1])
    arguments = parser.parse_args()
    generate(arguments.artifacts, arguments.binary, arguments.output)
