#!/usr/bin/env python3
"""Precompiled drako3d (Switch 2) shader packs for Cemu-nx, from the transferable caches the deko3d packs use.

    python make_packs.py build                 configure and build host/ (drako_shader_tool.exe)
    python make_packs.py build-uam2            link uam2_batch.exe from an ouncebrew/uam2 build directory
    python make_packs.py verify --game <id>    decompile only; compare the cache keys with shader_packs/deko
    python make_packs.py run --game <id>       make shader_packs/drako/<id>.zip and its manifest entry
    python make_packs.py run --all             every game of shader_packs/manifest.json
    python make_packs.py check                 re-validate every drako3d pack the manifest lists
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
REPO = TOOL_DIR.parents[1]
BUILD = TOOL_DIR / "build"
PACKS = REPO / "shader_packs"
MANIFEST = PACKS / "manifest.json"
DEFAULT_UAM2_BUILD = REPO.parent / "ouncebrew" / "uam2" / "build"
DEFAULT_MSYS2 = Path(r"C:/msys64")
SOURCE_ASSET = "shaders.zip"  # the asset of manifest.json's source release

CACHE_ROOT = "switch/cemu/cache/shaderCache/"
UAM2_STAGES = {0: "vert", 3: "geom", 4: "frag"}  # UamStage (UamBridge.h) -> uam2 -s
FILECACHE_MAGIC_V3 = 0x8371B696
FILETABLE_NAME = (0xEFEFEFEFEFEFEFEF, 0xFEFEFEFEFEFEFEFE)

# (SPIRV-Cross for the renderer: cmake/switch/Deko3DDependencies.cmake, also pinned in host/CMakeLists.txt)
SPIRV_CROSS_COMMIT = "ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b"
GLSLANG_COMMIT = "1062752a891c95b2bfeed9e356562d88f9df84ac"
SPIRV_CROSS_PATCH = REPO / "cmake" / "switch" / "patches" / "spirv-cross-shadow1d.patch"


def transferable_entry(title):
    return CACHE_ROOT + f"transferable/{title}_shaders.bin"


def driver_entry(title, api="drako3d"):
    return CACHE_ROOT + f"driver/{api}/{title}/v1/shaders.bin"


def log(message):
    print(message, flush=True)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def tool_exe():
    path = BUILD / "host" / "Release" / "drako_shader_tool.exe"
    if not path.exists():
        sys.exit(f"{path} is missing: run 'python make_packs.py build' first")
    return path


def load_manifest():
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


# ---- FileCache (src/Cemu/FileCache/FileCache.cpp) reading, for checks ------------------------------------------

def filecache_entries(data):
    """(extra version, [(name1, name2, flags, stored bytes)]) of a FileCache v3 file. Flag 1: zlib-compressed
    (some transferable caches have such entries; the renderer's native cache never does)."""
    magic, extra, data_offset, table_offset, table_size = struct.unpack_from("<IIQQI", data, 0)
    if magic != FILECACHE_MAGIC_V3:
        raise ValueError("not a FileCache v3 file")
    entries = []
    for i in range(table_size // 32):
        name1, name2, offset, size, flags = struct.unpack_from("<QQQIB", data, data_offset + table_offset + i * 32)
        if (name1, name2) != (0, 0) and (name1, name2) != FILETABLE_NAME:
            entries.append((name1, name2, flags, data[data_offset + offset:data_offset + offset + size]))
    return extra, entries


def filecache_names(data):
    return [(name1, name2) for name1, name2, _, _ in filecache_entries(data)[1]]


def check_cdkc_record(name1, name2, record):
    """DKShader.cpp's LoadCachedBinary checks, written again independently (Switch 2 limits)."""
    if len(record) < 416:
        return "short record"
    magic, version, stage, size, source_hash = struct.unpack_from("<IIIIQ", record, 0)
    uniforms, textures, storage = record[24:152], record[152:280], record[280:408]
    push = struct.unpack_from("<b", record, 408)[0]
    binary = record[416:]
    if magic != 0x434B4443 or version != 1 or stage != name2 or source_hash != name1:
        return "header"
    if not size or size > 16 * 1024 * 1024 or size != len(binary):
        return "size"
    if any(not (-1 <= b - 256 * (b > 127) < limit) for table, limit in ((uniforms, 15), (textures, 32), (storage, 16))
           for b in table) or not -1 <= push < 15:
        return "bindings"
    dksh_magic, header_size, control_size, code_size, programs_offset, programs = struct.unpack_from("<6I", binary, 0)
    if (dksh_magic != 0x48534B44 or header_size < 24 or control_size > len(binary) or
            code_size > len(binary) - control_size or programs_offset > control_size or not programs or
            programs > (control_size - programs_offset) // 64):
        return "dksh header"
    expected_type = {0: 0, 3: 2, 4: 1}.get(stage)
    for i in range(programs):
        program_type, _, _, _, _, scratch = struct.unpack_from("<6I", binary, programs_offset + i * 64)
        if program_type != expected_type or scratch > 128 * 1024:
            return "dksh program"
    return None


def cmd_check(args):
    """Re-reads every drako3d pack of the manifest: file size and sha256, the two zip entries, both caches and
    every native record."""
    manifest = load_manifest()
    problems = 0
    totals = {"games": 0, "native": 0, "transferable": 0, "bytes": 0}
    for title, game in sorted(manifest["games"].items()):
        entry = game.get("drako")
        if not entry:
            log(f"{title}: no drako entry")
            problems += 1
            continue
        pack = PACKS / entry["files"][0]["path"]
        issues = []
        if not pack.exists() or pack.stat().st_size != entry["size"] or sha256_file(pack) != entry["sha256"]:
            issues.append("size/sha256")
        else:
            with zipfile.ZipFile(pack) as archive:
                names = {info.filename: info for info in archive.infolist()}
                if set(names) != {transferable_entry(title), driver_entry(title)}:
                    issues.append("zip entries")
                else:
                    for info in names.values():
                        if (info.compress_type != zipfile.ZIP_DEFLATED or info.date_time != (1980, 1, 1, 0, 0, 0)
                                or info.external_attr >> 16 != 0o100644):
                            issues.append(f"zip metadata of {info.filename}")
                    extra, native = filecache_entries(archive.read(driver_entry(title)))
                    transferable = filecache_names(archive.read(transferable_entry(title)))
                    if extra != 1:
                        issues.append("driver cache extra version")
                    for name1, name2, flags, record in native:
                        problem = "compressed" if flags & 1 else check_cdkc_record(name1, name2, record)
                        if problem:
                            issues.append(f"record {name1:016x}:{name2}: {problem}")
                    if len(native) != entry["native_shaders"] or len(transferable) != entry["transferable_shaders"]:
                        issues.append("counts")
                    totals["native"] += len(native)
                    totals["transferable"] += len(transferable)
        totals["games"] += 1
        totals["bytes"] += entry["size"]
        if issues:
            problems += 1
            log(f"{title}: " + "; ".join(issues[:5]))
    log(f"checked {totals['games']} packs ({totals['bytes']} bytes, {totals['native']} native records, "
        f"{totals['transferable']} transferable entries): {problems} with problems")
    return 1 if problems else 0


# ---- build ----------------------------------------------------------------------------------------------------

def find_cmake(explicit):
    candidates = [explicit] if explicit else [os.environ.get("CMAKE"), shutil.which("cmake")]
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-find",
                                r"Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"],
                               capture_output=True, text=True).stdout.split()
        candidates += found
    for candidate in candidates:
        if not candidate:
            continue
        try:
            version = subprocess.run([candidate, "--version"], capture_output=True, text=True).stdout
        except OSError:
            continue
        match = re.search(r"cmake version (\d+)\.(\d+)", version)
        if match and (int(match.group(1)), int(match.group(2))) >= (3, 24):
            return candidate
    sys.exit("CMake 3.24 or newer not found (pass --cmake)")


def cmd_build(args):
    cmake = find_cmake(args.cmake)
    BUILD.mkdir(parents=True, exist_ok=True)
    (BUILD / ".gitignore").write_text("*\n", encoding="ascii")
    # (x64 on every host: Cemu's precompiled.h takes MSVC for x64, e.g. _tzcnt_u32; Windows on Arm runs it emulated)
    subprocess.run([cmake, "-S", str(TOOL_DIR / "host"), "-B", str(BUILD / "host"), "-G", "Visual Studio 17 2022",
                    "-A", "x64", f"-DFETCHCONTENT_BASE_DIR={BUILD / '_deps'}"], check=True)
    subprocess.run([cmake, "--build", str(BUILD / "host"), "--config", "Release", "--parallel", str(args.jobs)],
                   check=True)
    log(f"built {tool_exe()}")


# ---- source snapshot ------------------------------------------------------------------------------------------

def fetch_source(manifest):
    source = manifest["source"]
    destination = BUILD / "downloads" / SOURCE_ASSET
    if destination.exists() and sha256_file(destination) == source["sha256"]:
        return destination
    destination.parent.mkdir(parents=True, exist_ok=True)
    url = f"{source['repository']}/releases/download/{source['release']}/{SOURCE_ASSET}"
    log(f"downloading {url}")
    partial = destination.with_suffix(".part")
    with urllib.request.urlopen(url) as response, open(partial, "wb") as output:
        shutil.copyfileobj(response, output, 1024 * 1024)
    digest = sha256_file(partial)
    if digest != source["sha256"]:
        partial.unlink()
        sys.exit(f"{url}: sha256 {digest}, the manifest expects {source['sha256']}")
    partial.replace(destination)
    return destination


def extract_transferable(snapshot, title, destination):
    with zipfile.ZipFile(snapshot) as archive:
        data = archive.read(f"shaderCache/transferable/{title}_shaders.bin")
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(data)
    return destination


# ---- decompile / compare --------------------------------------------------------------------------------------

def read_tsv(path):
    rows = []
    header = None
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if header is None:
            header = fields
            continue
        rows.append(dict(zip(header, fields)))
    return rows


def run_tool(arguments):
    result = subprocess.run([str(tool_exe()), *arguments], capture_output=True, text=True)
    if result.returncode != 0:
        sys.exit(f"drako_shader_tool {arguments[0]} failed:\n{result.stderr}{result.stdout}")
    return json.loads(result.stdout.strip().splitlines()[-1])


def decompile(title, snapshot, jobs, dump_source=False):
    work = BUILD / "work" / title
    if work.exists():
        shutil.rmtree(work)
    source = extract_transferable(snapshot, title, work / "input" / f"{title}_shaders.bin")
    arguments = ["decompile", "--title", title, "--cache", str(source), "--work", str(work), "--jobs", str(jobs)]
    if dump_source:
        arguments.append("--dump-source")
    summary = run_tool(arguments)
    return work, source, summary, read_tsv(work / "records.tsv")


def compare_with_deko(title, records):
    """The cache key depends only on the untranslated GLSL, so the deko3d pack of the same transferable cache has
    the same keys: every entry of its transferable cache must give exactly the keys of its native cache.

    One known difference is allowed: the deko3d packs of Breath of the Wild also hold code for the four vertex
    shaders that LatteShader_CreateRendererShader filters out ("avoid AMD crash", on every renderer), so the console
    never compiles or looks them up. Such keys ("unused" here: the GLSL of a no_shader entry) are reported, not
    counted as mismatches."""
    pack = PACKS / "deko" / f"{title}.zip"
    if not pack.exists():
        return None
    with zipfile.ZipFile(pack) as archive:
        deko_native = set(filecache_names(archive.read(driver_entry(title, "deko3d"))))
        deko_entries = set(filecache_names(archive.read(transferable_entry(title))))
    ours = set()
    unused = set()
    for record in records:
        if (int(record["name1"], 16), int(record["name2"], 16)) not in deko_entries:
            continue
        if record["key"] != "-":
            ours.add((int(record["key"], 16), int(record["stage"])))
        match = re.search(r"unused key ([0-9a-f]{16}):(\d)", record["detail"])
        if record["status"] == "no_shader" and match:
            unused.add((int(match.group(1), 16), int(match.group(2))))
    matched = deko_native & ours
    return {
        "deko_native": len(deko_native),
        "ours": len(ours),
        "matched": len(matched),
        "unused": sorted(deko_native & unused),
        "missing": sorted(deko_native - ours - unused),
        "extra": sorted(ours - deko_native),
        "rate": len(matched) / max(1, len(deko_native - unused)),
    }


def report_comparison(title, comparison):
    if comparison is None:
        log(f"{title}: no deko3d pack to compare with")
        return True
    ok = not comparison["missing"] and not comparison["extra"]
    unused = comparison["unused"]
    log(f"{title}: keys {comparison['matched']}/{comparison['deko_native'] - len(unused)} of the deko3d pack match "
        f"({comparison['rate'] * 100:.2f}%), {len(comparison['missing'])} missing, {len(comparison['extra'])} extra"
        + (f" ({len(unused)} deko3d entries for shaders the console never creates)" if unused else ""))
    for key, stage in comparison["missing"][:10]:
        log(f"    missing {key:016x} stage {stage}")
    for key, stage in comparison["extra"][:10]:
        log(f"    extra   {key:016x} stage {stage}")
    return ok


def cmd_verify(args):
    manifest = load_manifest()
    snapshot = fetch_source(manifest)
    titles = sorted(manifest["games"]) if args.all else args.game
    totals = {"matched": 0, "deko_native": 0, "games": 0, "exact": 0}
    for title in titles:
        _, _, summary, records = decompile(title, snapshot, args.jobs, args.dump_source)
        comparison = compare_with_deko(title, records)
        exact = report_comparison(title, comparison)
        if comparison:
            totals["games"] += 1
            totals["exact"] += exact
            totals["matched"] += comparison["matched"]
            totals["deko_native"] += comparison["deko_native"] - len(comparison["unused"])
            totals["unused"] = totals.get("unused", 0) + len(comparison["unused"])
    log(f"verified {totals['games']} games: {totals['exact']} with identical key sets, "
        f"{totals['matched']}/{totals['deko_native']} keys matched"
        + (f" ({totals['unused']} deko3d entries for shaders the console never creates)" if totals.get("unused") else ""))
    return 0 if totals["exact"] == totals["games"] else 1


# ---- uam2 -----------------------------------------------------------------------------------------------------

# The console compiles with uam2rt: UAM2 as a library in the program (mesa-nx/src/nouveau/uam2rt), on Mesa
# 26.2.7. On the PC, uam2_batch.exe is UAM2 (ouncebrew/uam2, Mesa 26.3.0) linked from the objects of an existing
# uam2 build, with uam2rt's changes to its GLSL front end (below) and a main that compiles many shaders per
# process, as uam2rt does. Without the xfb change, every vertex shader feeding a geometry shader (an output
# block) fails to compile on the PC; without the attribute one, every vertex shader with more than 16 inputs.
UAM2_BATCH = BUILD / "uam2" / "uam2_batch.exe"
UAM2_SOURCES = ("compile.c", "dksh_writer.cpp", "glsl_frontend.cpp", "lower_abi.c", "t239.c")
UAM2RT_FRONTEND_REWRITES = [
    ("\t\tpc->MaxAttribs = i == MESA_SHADER_VERTEX ? 16 :",
     "\t\t// (uam2rt: drako3d's 32 vertex attributes, not GL's 16)\n"
     "\t\tpc->MaxAttribs = i == MESA_SHADER_VERTEX ? 32 :"),
    ("\tctx->Const.MaxVertexStreams = 4;\n",
     "\tctx->Const.MaxVertexStreams = 4;\n"
     "\t// (uam2rt: GLSL 4.40+ checks every output block's default xfb_buffer against these; Mesa leaves them 0)\n"
     "\tctx->Const.MaxTransformFeedbackBuffers = 4;\n"
     "\tctx->Const.MaxTransformFeedbackSeparateComponents = 128;\n"
     "\tctx->Const.MaxTransformFeedbackInterleavedComponents = 128;\n"),
    ("\tfor (unsigned i = 0; i < map.num_ubos; i ++)\n"
     "\t\tmap.ubo_binding[i] = prog->sh.UniformBlocks[i]->Binding;\n",
     "\tfor (unsigned i = 0; i < map.num_ubos; i ++)\n"
     "\t{\n"
     "\t\tmap.ubo_binding[i] = prog->sh.UniformBlocks[i]->Binding;\n"
     "\t\t// (uam2rt: binding N is constant buffer N + 3 of the 18 a graphics stage has)\n"
     "\t\tif (nir->info.stage != MESA_SHADER_COMPUTE && map.ubo_binding[i] > 18 - UAM2_FIRST_USER_CBUF - 1)\n"
     "\t\t{\n"
     "\t\t\tfprintf(stderr, \"error: uniform block binding %d: graphics stages on drako3d have bindings 0 to %d\\n\",\n"
     "\t\t\t\tmap.ubo_binding[i], 18 - UAM2_FIRST_USER_CBUF - 1);\n"
     "\t\t\tmap.ok = false;\n"
     "\t\t}\n"
     "\t}\n"),
]


def ninja_edges(build_ninja):
    """{output: {"inputs": [...], "vars": {...}}} for the uam2 targets of a meson-generated build.ninja."""
    text = build_ninja.read_text(encoding="utf-8").replace("$\n", "")
    edges = {}
    current = None
    for line in text.splitlines():
        if line.startswith("build "):
            head = line[len("build "):].split(" || ")[0].split(" | ")[0]
            output, _, rest = head.partition(": ")
            output = output.replace("$:", ":").replace("$ ", " ")
            current = edges.setdefault(output, {"inputs": rest.split()[1:], "vars": {}})
        elif current is not None and line.startswith(" ") and " = " in line:
            name, _, value = line.strip().partition(" = ")
            current["vars"][name] = value
        else:
            current = None
    return edges


def ninja_args(value):
    import shlex
    return shlex.split(value.replace("$$", "\0").replace("$ ", " ").replace("$:", ":").replace("\0", "$"))


def cmd_build_uam2(args):
    uam2_build = Path(args.uam2_build).resolve()
    edges = ninja_edges(uam2_build / "build.ninja")
    link = edges.get("source/uam2.exe")
    if not link:
        sys.exit(f"{uam2_build}: not a uam2 build directory (no source/uam2.exe in build.ninja)")
    compiler_dir = Path(args.msys2) / "clangarm64" / "bin"
    clang = {"c": str(compiler_dir / "clang.exe"), "cpp": str(compiler_dir / "clang++.exe")}
    out = BUILD / "uam2"
    sources = out / "src"
    objects = out / "obj"
    for directory in (sources, objects):
        directory.mkdir(parents=True, exist_ok=True)
    uam2_source = uam2_build.parent / "source"
    for header in uam2_source.glob("*.h"):
        shutil.copyfile(header, sources / header.name)
    for name in UAM2_SOURCES:
        text = (uam2_source / name).read_text(encoding="utf-8")
        if name == "glsl_frontend.cpp":
            text = text.replace("\r\n", "\n")
            for old, new in UAM2RT_FRONTEND_REWRITES:
                if text.count(old) != 1:
                    sys.exit(f"{uam2_source / name}: cannot apply uam2rt's change at:\n{old}")
                text = text.replace(old, new)
        (sources / name).write_text(text, encoding="utf-8", newline="\n")
    shutil.copyfile(TOOL_DIR / "uam2_batch" / "uam2_batch.cpp", sources / "uam2_batch.cpp")
    env = dict(os.environ, PATH=str(compiler_dir) + os.pathsep + os.environ.get("PATH", ""))

    def compile_one(name, edge_name):
        edge = edges[f"source/uam2.exe.p/{edge_name}.obj"]
        language = "cpp" if edge_name.endswith(".cpp") else "c"
        flags = [flag for flag in ninja_args(edge["vars"]["ARGS"]) if flag != "-fdiagnostics-color=always"]
        output = objects / f"{name}.obj"
        subprocess.run([clang[language], *flags, f"-I{sources}", "-o", str(output), "-c", str(sources / name)],
                       cwd=uam2_build, env=env, check=True)
        return str(output)

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = [pool.submit(compile_one, name, name) for name in UAM2_SOURCES]
        futures.append(pool.submit(compile_one, "uam2_batch.cpp", "main.cpp"))
        built = [future.result() for future in futures]
    subprocess.run([clang["cpp"], "-o", str(UAM2_BATCH), *built, *ninja_args(link["vars"]["LINK_ARGS"])],
                   cwd=uam2_build, env=env, check=True)
    identity = uam2_identity(uam2_build)
    (out / "identity.json").write_text(json.dumps(identity, indent=2) + "\n", encoding="utf-8")
    log(f"built {UAM2_BATCH} from {uam2_build} ({identity.get('uam2_version')}, "
        f"commit {identity.get('uam2_commit', '?')})")


def uam2_identity(uam2_build):
    source = uam2_build.parent
    identity = {"uam2_build": str(uam2_build)}
    meson = (source / "meson.build").read_text(encoding="utf-8")
    version = re.search(r"project\('uam2'.*?version:\s*'([^']+)'", meson, re.S)
    mesa = re.search(r"mesa_version\s*=\s*'([^']+)'", meson)
    identity["uam2_version"] = f"uam2 {version.group(1)}" if version else "uam2"
    identity["mesa_version"] = mesa.group(1) if mesa else None
    for key, path in (("uam2_commit", source), ("mesa_commit", source / "subprojects" / "mesa")):
        commit = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True)
        if commit.returncode == 0:
            identity[key] = commit.stdout.strip()
    dirty = subprocess.run(["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"],
                           capture_output=True, text=True)
    identity["uam2_modified"] = bool(dirty.stdout.strip())
    return identity


def compile_shaders(work, records, jobs, timeout):
    """uam2_batch over the translated shaders, in chunks spread over `jobs` processes. A crash or a hang fails the
    shader it happened on (the batch reports each job's start); the rest of its chunk goes to a new process.

    Each process gets its own TMP/TEMP directory, removed when it exits: Mesa's memstream on Windows backs every
    NIR printer with a GetTempFileName file that it never deletes (u_memstream_close removes the file before
    closing it), and once the 65535 names of a directory are taken NAK panics on every shader."""
    if not UAM2_BATCH.exists():
        sys.exit(f"{UAM2_BATCH} is missing: run 'python make_packs.py build-uam2' first")
    dksh = work / "dksh"
    dksh.mkdir(exist_ok=True)
    names = [(f"{r['key']}_{r['stage']}", UAM2_STAGES[int(r["stage"])]) for r in records if r["status"] == "ok"]
    size = max(1, min(32, len(names) // (jobs * 4) or 1))
    chunks = [names[i:i + size] for i in range(0, len(names), size)]

    def run_chunk(number, chunk):
        results = {}
        attempt = 0
        while chunk:
            jobs_file = dksh / f"jobs_{number}_{attempt}.txt"
            jobs_file.write_text("".join(
                f"{stage}\t{work / 'glsl' / (name + '.glsl')}\t{dksh / (name + '.dksh')}\t{dksh / (name + '.log')}\n"
                for name, stage in chunk), encoding="utf-8")
            temp = work / "tmp" / f"{number}_{attempt}"
            temp.mkdir(parents=True, exist_ok=True)
            env = dict(os.environ, TMP=str(temp), TEMP=str(temp))
            try:
                result = subprocess.run([str(UAM2_BATCH), str(jobs_file)], capture_output=True, text=True,
                                        timeout=timeout * len(chunk), env=env)
                output, failure = result.stdout, f"uam2 crashed (exit code {result.returncode:#x})"
            except subprocess.TimeoutExpired as error:
                output = error.stdout.decode() if isinstance(error.stdout, bytes) else (error.stdout or "")
                failure = f"uam2 did not finish within {timeout * len(chunk)} s"
            jobs_file.unlink(missing_ok=True)
            shutil.rmtree(temp, ignore_errors=True)
            begun = -1
            done = {}
            for line in output.splitlines():
                fields = line.split("\t")
                if fields[0] == "begin":
                    begun = int(fields[1])
                elif fields[0] == "end":
                    done[int(fields[1])] = fields[2] == "ok"
            for index, ok in done.items():
                results[chunk[index][0]] = ok
            if len(done) == len(chunk):
                break
            if begun < 0:
                # (the process did not get to any shader)
                for name, _ in chunk:
                    results[name] = False
                    (dksh / f"{name}.log").write_text(failure, encoding="utf-8")
                break
            next_job = min(index for index in range(len(chunk)) if index not in done)
            if begun == next_job:
                # (it stopped in this shader)
                name = chunk[begun][0]
                results[name] = False
                (dksh / f"{name}.dksh").unlink(missing_ok=True)
                (dksh / f"{name}.log").write_text(failure, encoding="utf-8")
                next_job += 1
            chunk = chunk[next_job:]
            attempt += 1
        return results

    started = time.time()
    results = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
        for chunk_results in pool.map(lambda item: run_chunk(*item), enumerate(chunks)):
            results.update(chunk_results)
    shutil.rmtree(work / "tmp", ignore_errors=True)
    compiled = sum(results.values())
    return compiled, len(names) - compiled, time.time() - started


# ---- packs ----------------------------------------------------------------------------------------------------

def write_pack(path, entries):
    """Deterministic like dist/switch/package_shader_packs.py: deflate, no zip64, 1980-01-01, 0644."""
    path.parent.mkdir(parents=True, exist_ok=True)
    partial = path.with_suffix(".part")
    with zipfile.ZipFile(partial, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=False) as archive:
        for name, source in sorted(entries.items()):
            info = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            archive.writestr(info, Path(source).read_bytes())
    partial.replace(path)


def make_pack(title, snapshot, args):
    work, source, summary, records = decompile(title, snapshot, args.jobs)
    comparison = compare_with_deko(title, records)
    if not report_comparison(title, comparison) and not args.allow_key_mismatch:
        sys.exit(f"{title}: the cache keys differ from the deko3d pack's; not making a pack (--allow-key-mismatch)")
    compiled, failed, seconds = compile_shaders(work, records, args.jobs, args.timeout)
    log(f"{title}: uam2 compiled {compiled} shaders, {failed} failed ({seconds:.0f} s)")
    out = work / "out"
    transferable = out / f"{title}_shaders.bin"
    driver = out / "shaders.bin"
    assembled = run_tool(["assemble", "--title", title, "--cache", str(source), "--work", str(work),
                          "--driver-out", str(driver), "--transferable-out", str(transferable)])
    with open(transferable, "rb") as stream:
        kept = len(filecache_names(stream.read()))
    with open(driver, "rb") as stream:
        native = len(filecache_names(stream.read()))
    if kept != assembled["transferable_out"] or native != assembled["native"]:
        sys.exit(f"{title}: assembled caches do not match the tool's counts")
    pack = PACKS / "drako" / f"{title}.zip"
    write_pack(pack, {transferable_entry(title): transferable, driver_entry(title): driver})
    size = pack.stat().st_size
    digest = sha256_file(pack)
    entry = {
        "size": size,
        "sha256": digest,
        "files": [{"path": f"drako/{title}.zip", "size": size, "sha256": digest}],
        "transferable_shaders": kept,
        "native_shaders": native,
        "excluded_shaders": assembled["transferable_in"] - kept,
    }
    log(f"{title}: {pack.relative_to(REPO)} {size} bytes, {kept} transferable, {native} native, "
        f"{entry['excluded_shaders']} excluded")
    if not args.keep_work:
        for directory in ("glsl", "dksh", "source", "tmp"):
            shutil.rmtree(work / directory, ignore_errors=True)
    return entry, comparison


def drako_block(identity):
    block = {
        "name": "UAM2 Drako Shaders Pack",
        "cache_version": 1,
        "uam2_commit": identity.get("uam2_commit"),
        "uam2_version": identity.get("uam2_version"),
        "uam2rt_changes": ["xfb buffer limits", "graphics uniform block bindings 0-14", "32 vertex attributes"],
        "mesa_version": identity.get("mesa_version"),
        "mesa_commit": identity.get("mesa_commit"),
        "glslang_commit": GLSLANG_COMMIT,
        "spirv_cross_commit": SPIRV_CROSS_COMMIT,
        "compiler_patches": {SPIRV_CROSS_PATCH.name: sha256_file(SPIRV_CROSS_PATCH)},
        "indexed_cache": True,
    }
    return {key: value for key, value in block.items() if value is not None}


def update_manifest(identity, entries):
    """Adds the drako data only (top-level "drako" after "deko", games[id].drako after games[id].deko), keeping the
    file's formatting and line endings."""
    raw = MANIFEST.read_bytes()
    newline = "\r\n" if b"\r\n" in raw else "\n"
    manifest = json.loads(raw)
    if (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").replace("\n", newline).encode("utf-8") != raw:
        sys.exit("manifest.json is not in the formatting this script writes; not touching it")

    def insert_after(mapping, anchor, key, value):
        result = {}
        for existing_key, existing_value in mapping.items():
            if existing_key == key:
                continue
            result[existing_key] = existing_value
            if existing_key == anchor:
                result[key] = value
        if key not in result:
            result[key] = value
        return result

    manifest = insert_after(manifest, "deko", "drako", drako_block(identity))
    for title, entry in entries.items():
        manifest["games"][title] = insert_after(manifest["games"][title], "deko", "drako", entry)
    text = (json.dumps(manifest, indent=2, ensure_ascii=False) + "\n").replace("\n", newline)
    partial = MANIFEST.with_suffix(".json.part")
    partial.write_bytes(text.encode("utf-8"))
    partial.replace(MANIFEST)


def cmd_run(args):
    manifest = load_manifest()
    titles = sorted(manifest["games"]) if args.all else args.game
    unknown = [title for title in titles if title not in manifest["games"]]
    if unknown:
        sys.exit(f"not in manifest.json: {', '.join(unknown)}")
    identity_file = UAM2_BATCH.parent / "identity.json"
    if not UAM2_BATCH.exists() or not identity_file.exists():
        sys.exit(f"{UAM2_BATCH} is missing: run 'python make_packs.py build-uam2' first")
    identity = json.loads(identity_file.read_text(encoding="utf-8"))
    log(f"compiler: {identity.get('uam2_version')} (Mesa {identity.get('mesa_version')}) commit "
        f"{identity.get('uam2_commit', '?')}{' (modified)' if identity.get('uam2_modified') else ''}")
    snapshot = fetch_source(manifest)
    totals = {"games": 0, "native": 0, "excluded": 0, "transferable": 0, "bytes": 0}
    for index, title in enumerate(titles, 1):
        log(f"[{index}/{len(titles)}] {title} {manifest['games'][title].get('name', '')}")
        entry, _ = make_pack(title, snapshot, args)
        if not args.no_manifest:
            update_manifest(identity, {title: entry})
        totals["games"] += 1
        totals["native"] += entry["native_shaders"]
        totals["excluded"] += entry["excluded_shaders"]
        totals["transferable"] += entry["transferable_shaders"]
        totals["bytes"] += entry["size"]
    log(f"done: {totals['games']} packs, {totals['native']} native shaders, {totals['excluded']} excluded, "
        f"{totals['transferable']} transferable, {totals['bytes']} bytes")
    return 0


def main():
    # (game names are not ASCII; Windows' console or a redirected stdout would otherwise use the ANSI code page)
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = parser.add_subparsers(dest="command", required=True)
    jobs = os.cpu_count() or 4

    build = commands.add_parser("build", help="configure and build the host tool")
    build.add_argument("--cmake", help="cmake executable (3.24+; default: PATH, then Visual Studio's)")
    build.add_argument("--jobs", type=int, default=jobs)
    build.set_defaults(func=cmd_build)

    build_uam2 = commands.add_parser("build-uam2", help="link uam2_batch.exe from a uam2 build directory")
    build_uam2.add_argument("--uam2-build", default=os.environ.get("UAM2_BUILD", str(DEFAULT_UAM2_BUILD)),
                            help=f"ouncebrew/uam2 build directory (default: $UAM2_BUILD or {DEFAULT_UAM2_BUILD})")
    build_uam2.add_argument("--msys2", default=os.environ.get("MSYS2_ROOT", str(DEFAULT_MSYS2)),
                            help="MSYS2 root whose clangarm64 compilers built uam2 (default: C:/msys64)")
    build_uam2.add_argument("--jobs", type=int, default=jobs)
    build_uam2.set_defaults(func=cmd_build_uam2)

    check = commands.add_parser("check", help="re-validate every drako3d pack listed in manifest.json")
    check.set_defaults(func=cmd_check)

    for name, func, text in (("verify", cmd_verify, "compare cache keys with the deko3d packs"),
                             ("run", cmd_run, "make drako3d packs")):
        command = commands.add_parser(name, help=text)
        which = command.add_mutually_exclusive_group(required=True)
        which.add_argument("--game", action="append", help="title id (16 hex digits); repeatable")
        which.add_argument("--all", action="store_true", help="every game of shader_packs/manifest.json")
        command.add_argument("--jobs", type=int, default=jobs)
        command.set_defaults(func=func)
        if name == "verify":
            command.add_argument("--dump-source", action="store_true",
                                 help="keep the untranslated GLSL in build/work/<id>/source")
        else:
            command.add_argument("--timeout", type=int, default=600, help="seconds per shader for uam2")
            command.add_argument("--keep-work", action="store_true",
                                 help="keep the translated GLSL and DKSH files in build/work/<id>")
            command.add_argument("--no-manifest", action="store_true", help="do not update manifest.json")
            command.add_argument("--allow-key-mismatch", action="store_true",
                                 help="make the pack even if the keys differ from the deko3d pack's")
    args = parser.parse_args()
    if getattr(args, "game", None):
        args.game = [title.lower() for title in args.game]
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
