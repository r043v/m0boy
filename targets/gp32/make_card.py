"""Build a 32 MiB SmartMedia snapshot from a directory."""
import argparse
from functools import lru_cache
from pathlib import Path
import struct
import subprocess
import tempfile

SECTOR_SIZE = 512
PAGES_PER_BLOCK = 32
PHYSICAL_BLOCKS = 2048
ZONE_BLOCKS = 1024
ZONE_LOGICAL_BLOCKS = 1000
LOGICAL_BLOCKS = 2 * ZONE_LOGICAL_BLOCKS
PARTITION_START = 35


@lru_cache(maxsize=1024)
def ecc(data):
    """SmartMedia Hamming code for a 256-byte half-page."""
    row_parity = 0
    column_parity = 0
    for address, value in enumerate(data):
        column_parity ^= value
        if bin(value).count("1") % 2:
            for bit in range(8):
                row_parity ^= 1 << (2 * bit + ((address >> bit) & 1))
    column_code = 3
    for bit, mask in enumerate((0x55, 0xaa, 0x33, 0xcc, 0x0f, 0xf0), 2):
        column_code |= (1 ^ (bin(column_parity & mask).count("1") % 2)) << bit
    return (row_parity ^ 0xffff).to_bytes(2, "little") + bytes([column_code])


def page(data, block_address):
    return data + b"\xff" * 6 + block_address + ecc(data[256:]) + block_address + ecc(data[:256])


def make_card(folder, destination, mkfs, mcopy, identity=None, *, prepare_directory=True):
    header = b""
    if identity is not None:
        with identity.open("rb") as source:
            header = source.read(1024)
        if len(header) != 1024 or header[:3] != b"\xec\x75\xa5":
            raise ValueError("Expected a 32 MiB Samsung SMC image with a GeePee32 UID header")
        if identity.stat().st_size != len(header) + PHYSICAL_BLOCKS * PAGES_PER_BLOCK * 528:
            raise ValueError("Invalid 32 MiB SMC image size")
    if prepare_directory:
        folder.mkdir(parents=True, exist_ok=True)
    for entry in folder.rglob("*"):
        if entry.is_symlink() or not (entry.is_dir() or entry.is_file()):
            raise ValueError(f"Unsupported SmartMedia entry: {entry}")
    if prepare_directory:
        for name in ("GAME", "GPMM"):
            if not any(entry.name.upper() == name and entry.is_dir() for entry in folder.iterdir()):
                (folder / name).mkdir(exist_ok=True)

    volume_sectors = LOGICAL_BLOCKS * PAGES_PER_BLOCK - PARTITION_START - 1
    with tempfile.TemporaryDirectory(dir=destination.parent) as workspace:
        fat = Path(workspace) / "card.fat"
        subprocess.run([mkfs, "-C", "-F", "16", "-s", "4", "-R", "1", "-r", "256", "-g", "8/16", "-a", "-h", str(PARTITION_START),
                        str(fat), str(volume_sectors // 2)], check=True)
        for entry in sorted(folder.iterdir()):
            subprocess.run([mcopy, "-s", "-i", str(fat), str(entry.absolute()), "::/"],
                           check=True)
        mbr = bytearray(PARTITION_START * SECTOR_SIZE)
        # One FAT16 partition. The guest addresses it by LBA.
        struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\x01\x01\x00", 0x06,
                         b"\xfe\xff\xff", PARTITION_START, volume_sectors)
        mbr[510:512] = b"\x55\xaa"
        disk = (bytes(mbr) + fat.read_bytes()).ljust(LOGICAL_BLOCKS * PAGES_PER_BLOCK * SECTOR_SIZE, b"\x00")
        card = Path(workspace) / "card.smc"
        with card.open("wb") as output:
            output.write(header)
            # CIS in the first physical block, with a copy in each half-page.
            cis = bytes.fromhex("01 03 d9 01 ff 18 02 df 01 20 04 00 00 00 00 ff")
            cis = cis.ljust(256, b"\xff")
            erased = b"\xff" * 528
            output.write(page(cis * 2, b"\x00\x00"))
            output.write(erased * (PAGES_PER_BLOCK - 1))
            # Logical addresses are relative to a zone, and every zone keeps
            # erased blocks of its own: the guest needs one for each rewrite.
            for zone in range(PHYSICAL_BLOCKS // ZONE_BLOCKS):
                for index in range(ZONE_LOGICAL_BLOCKS):
                    block = zone * ZONE_LOGICAL_BLOCKS + index
                    address = 0x1000 | (index << 1)
                    address |= bin(address).count("1") % 2
                    spare_address = address.to_bytes(2, "big")
                    for sector in range(block * PAGES_PER_BLOCK, (block + 1) * PAGES_PER_BLOCK):
                        start = sector * SECTOR_SIZE
                        output.write(page(disk[start:start + SECTOR_SIZE], spare_address))
                used = ZONE_LOGICAL_BLOCKS + (zone == 0)
                output.write(erased * ((ZONE_BLOCKS - used) * PAGES_PER_BLOCK))
        card.replace(destination)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("mkfs")
    parser.add_argument("mcopy")
    parser.add_argument("--identity", type=Path)
    args = parser.parse_args()
    make_card(args.folder, args.destination, args.mkfs, args.mcopy, args.identity)
