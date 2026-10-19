import argparse
import hashlib
import json
import shutil
import tempfile
import zipfile
from pathlib import Path


def digest(path):
    result = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def cache_paths(title):
    root = "switch/cemu/cache/shaderCache/"
    return {root + f"transferable/{title}_shaders.bin",
            root + f"driver/deko3d/{title}/v1/shaders.bin"}


def assemble(source, metadata, destination):
    with destination.open("wb") as output:
        for part in metadata["files"]:
            path = (source / part["path"]).resolve()
            if not path.is_relative_to(source.resolve()):
                raise ValueError("Invalid shader-pack path")
            if path.stat().st_size != part["size"] or digest(path) != part["sha256"]:
                raise ValueError(f"Damaged shader pack: {path}")
            with path.open("rb") as stream:
                shutil.copyfileobj(stream, output, 1024 * 1024)
    if destination.stat().st_size != metadata["size"] or digest(destination) != metadata["sha256"]:
        raise ValueError("Incomplete shader archive")


def package(source, output, manifest, titles, name):
    destination = output / name
    games = manifest["games"]
    with tempfile.TemporaryDirectory(prefix="cemu-shaders-") as temporary:
        archive = Path(temporary) / "game.zip"
        with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED,
                             allowZip64=False) as combined:
            for game in titles:
                assemble(source, games[game]["deko"], archive)
                with zipfile.ZipFile(archive) as individual:
                    expected = cache_paths(game)
                    if set(individual.namelist()) != expected or len(individual.infolist()) != len(expected):
                        raise ValueError(f"Unexpected cache files: {game}")
                    for entry in sorted(individual.namelist()):
                        info = zipfile.ZipInfo(entry, date_time=(1980, 1, 1, 0, 0, 0))
                        info.compress_type = zipfile.ZIP_DEFLATED
                        info.file_size = individual.getinfo(entry).file_size
                        info.external_attr = 0o100644 << 16
                        with individual.open(entry) as reader, combined.open(info, "w") as writer:
                            shutil.copyfileobj(reader, writer, 1024 * 1024)
    return {"file": name, "size": destination.stat().st_size, "sha256": digest(destination)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, default=Path(__file__).resolve().parents[2] / "shader_packs")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--game")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    manifest = json.loads((args.source / "manifest.json").read_text(encoding="utf-8"))
    titles = sorted(manifest["games"])
    if args.game:
        if args.game not in manifest["games"]:
            raise ValueError(f"Unknown game: {args.game}")
        titles = [args.game]
    name = f"deko-{args.game}.zip" if args.game else "UAM-Deko-Shaders-Pack.zip"
    result = package(args.source, args.output, manifest, titles, name)
    (args.output / "SHA256SUMS").write_text(
        f'{result["sha256"]}  {result["file"]}\n', encoding="ascii")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
