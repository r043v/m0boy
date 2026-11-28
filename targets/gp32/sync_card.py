"""Present a local directory as a GP32 SmartMedia card."""
import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

from make_card import make_card


def entries(folder):
    result = {}
    for path in sorted(folder.rglob("*")):
        relative = path.relative_to(folder).as_posix()
        key = relative.casefold()
        if key in result or path.is_symlink():
            raise ValueError(f"Ambiguous or unsupported SmartMedia entry: {path}")
        if path.is_dir():
            value = None
        elif path.is_file():
            with path.open("rb") as stream:
                value = hashlib.file_digest(stream, "sha256").hexdigest()
        else:
            raise ValueError(f"Unsupported SmartMedia entry: {path}")
        result[key] = [relative, value]
    return result


def build(folder, image):
    baseline = entries(folder)
    make_card(folder, image, "mkfs.fat", "mcopy", prepare_directory=False)
    with extracted_card(image) as exported:
        saved = entries(exported)
        if {key: item[1] for key, item in saved.items()} != {
            key: item[1] for key, item in baseline.items()
        }:
            raise ValueError("Some host files cannot be represented on the FAT volume")
    image.with_suffix(".json").write_text(json.dumps(baseline))


def logical_disk(image):
    raw = image.read_bytes()
    if len(raw) != 2048 * 32 * 528:
        raise ValueError("Invalid directory-backed SmartMedia geometry")
    disk = bytearray(2000 * 32 * 512)
    mapped = {}
    for block in range(2048):
        first = block * 32 * 528
        spare = raw[first + 512:first + 528]
        address = int.from_bytes(spare[6:8], "big")
        if spare[5].bit_count() < 7 or address >> 12 != 1 or address.bit_count() % 2:
            continue
        index = (address & 0xfff) >> 1
        if index >= 1000:
            continue
        logical = block // 1024 * 1000 + index
        pages = sum(raw[first + page * 528 + 518:first + page * 528 + 520] == spare[6:8]
                    for page in range(32))
        mapped.setdefault(logical, []).append((pages, block))
    for logical, copies in mapped.items():
        # A rewrite stopped half way leaves a partial copy beside the complete
        # one; the guest driver drops it the same way at its next mount.
        best = max(pages for pages, _ in copies)
        blocks = [block for pages, block in copies if pages == best]
        if len(blocks) != 1:
            raise ValueError(f"Ambiguous SmartMedia mapping for block {logical}")
        for page in range(32):
            dest = (logical * 32 + page) * 512
            source = (blocks[0] * 32 + page) * 528
            disk[dest:dest + 512] = raw[source:source + 512]
    start = int.from_bytes(disk[454:458], "little")
    length = int.from_bytes(disk[458:462], "little")
    if (disk[510:512] != b"\x55\xaa" or disk[450] not in (1, 4, 6, 0x0e) or
            not start or not length or start + length > len(disk) // 512):
        raise ValueError("The guest card lacks a supported FAT partition")
    return disk[start * 512:(start + length) * 512]


@contextmanager
def extracted_card(image):
    with tempfile.TemporaryDirectory(dir=image.parent) as workspace:
        workspace = Path(workspace)
        fat = workspace / "volume.fat"
        fat.write_bytes(logical_disk(image))
        exported = workspace / "files"
        exported.mkdir()
        listing = subprocess.check_output(["mdir", "-b", "-i", str(fat), "::/"])
        if listing:
            subprocess.run(["mcopy", "-s", "-m", "-i", str(fat), "::*", str(exported)], check=True)
        yield exported


def update(folder, image):
    baseline = json.loads(image.with_suffix(".json").read_text())
    with extracted_card(image) as exported:
        guest = entries(exported)
        current = entries(folder)
        changes = {key for key in baseline.keys() | guest.keys()
                   if (baseline.get(key) or ["", False])[1] != (guest.get(key) or ["", False])[1]}
        # Resolve existing FAT names back to their original host spelling.
        destinations = {}
        for key, (name, _) in sorted(guest.items()):
            parent = key.rpartition("/")[0]
            existing = current.get(key) or baseline.get(key)
            destinations[key] = (Path(destinations.get(parent, "")) /
                                 Path(existing[0] if existing else name).name)
        for key in changes:
            if current.get(key) != baseline.get(key):
                raise ValueError(f"SmartMedia file also changed on the host: {key}")
            # Never remove a directory that acquired a new host child.
            if key in baseline and baseline[key][1] is None:
                prefix = key + "/"
                if any(child.startswith(prefix) and child not in changes for child in current):
                    raise ValueError(f"SmartMedia directory gained host files: {key}")
            parent = key.rpartition("/")[0]
            while parent:
                if parent in current and current[parent][1] is not None and parent not in changes:
                    raise ValueError(f"SmartMedia directory conflicts with host file: {parent}")
                parent = parent.rpartition("/")[0]
        for key in sorted(changes, key=lambda name: (-name.count("/"), name)):
            if key not in current:
                continue
            name, digest = current[key]
            target = folder / name
            if digest is None:
                target.rmdir()
            elif key not in guest or guest[key][1] is None:
                target.unlink()
        for key in sorted(changes):
            if key not in guest:
                continue
            name, digest = guest[key]
            target = folder / destinations[key]
            if digest is None:
                target.mkdir()
                continue
            with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as stream:
                temp = Path(stream.name)
            try:
                shutil.copy2(exported / name, temp)
                if target.exists():
                    os.chmod(temp, target.stat().st_mode & 0o777)
                temp.replace(target)
            finally:
                temp.unlink(missing_ok=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("build", "update"))
    parser.add_argument("folder", type=Path)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        (build if args.operation == "build" else update)(args.folder, args.image)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SmartMedia: {error}\n")
