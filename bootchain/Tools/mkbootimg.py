#!/usr/bin/env python3
"""yunluo Android boot image packer.

This follows the known-good yunluo GKI v4 layout used by
kernel/yunluo/yunluo-mainline/tools/pack-yunluo.py:
  boot header: 1584 bytes, padded to 4096
  boot payload: kernel only (UEFI shimmed+gzip image)
  ramdisk: supplied by the companion vendor_boot.img
  output: fixed 64 MiB boot partition image
"""
import argparse
import os
import struct
import sys

PAGE = 4096
BOOT_PART_SIZE = 64 * 1024 * 1024
BOOT_MAGIC = b"ANDROID!"
# Stock yunluo boot image: Android 12.0.0, patch 2023-11.
OS_VERSION_FIELD = 0x1800017B
HEADER_SIZE = 1584
HEADER_VERSION = 4


def pad(data: bytes, size: int = PAGE) -> bytes:
    remainder = len(data) % size
    return data if remainder == 0 else data + b"\0" * (size - remainder)


def pack_boot(kernel_path: str, output_path: str,
              part_size: int = BOOT_PART_SIZE,
              os_version: int = OS_VERSION_FIELD) -> None:
    kernel = open(kernel_path, "rb").read()
    header = bytearray(HEADER_SIZE)
    header[0:8] = BOOT_MAGIC

    # Android boot header v4 fields used by yunluo LK:
    #   +8  kernel_size
    #   +12 ramdisk_size (zero; ramdisk is in vendor_boot)
    #   +16 os_version
    #   +20 header_size (1584)
    struct.pack_into("<IIII", header, 8,
                     len(kernel), 0, os_version, HEADER_SIZE)
    struct.pack_into("<I", header, 40, HEADER_VERSION)

    image = bytearray()
    image += pad(bytes(header))
    image += kernel
    image = bytearray(pad(bytes(image)))

    if len(image) > part_size:
        raise ValueError(
            f"boot payload ({len(image)} bytes) exceeds partition "
            f"size ({part_size} bytes)"
        )
    image += b"\0" * (part_size - len(image))
    with open(output_path, "wb") as out:
        out.write(image)

    print(
        f"boot.img: kernel={len(kernel)} ramdisk=0 "
        f"header=v{HEADER_VERSION}/{HEADER_SIZE} "
        f"-> {output_path} ({part_size} bytes)"
    )


def inspect(path: str) -> None:
    data = open(path, "rb").read(4096)
    file_size = os.path.getsize(path)
    if data[:8] == BOOT_MAGIC:
        kernel_size, ramdisk_size, os_version, header_size = struct.unpack_from(
            "<IIII", data, 8
        )
        version, = struct.unpack_from("<I", data, 40)
        print(
            f"{path}: boot v{version} page={PAGE} "
            f"kernel={kernel_size} ramdisk={ramdisk_size} "
            f"os_version=0x{os_version:08x} header_size={header_size} "
            f"file_size={file_size}"
        )
    elif data[:8] == b"VNDRBOOT":
        version, page_size, kernel_addr, ramdisk_addr, ramdisk_size = \
            struct.unpack_from("<IIIII", data, 8)
        print(
            f"{path}: vendor_boot v{version} page={page_size} "
            f"kernel_addr=0x{kernel_addr:x} "
            f"ramdisk_addr=0x{ramdisk_addr:x} ramdisk={ramdisk_size} "
            f"file_size={file_size}"
        )
    else:
        print(f"{path}: unknown format")


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)

    boot = sub.add_parser("boot")
    boot.add_argument("kernel")
    boot.add_argument("output")
    boot.add_argument("--os-version", type=lambda value: int(value, 0),
                      default=OS_VERSION_FIELD)

    inspect_cmd = sub.add_parser("inspect")
    inspect_cmd.add_argument("paths", nargs="+")

    args = parser.parse_args()
    if args.command == "boot":
        pack_boot(args.kernel, args.output, os_version=args.os_version)
    else:
        for path in args.paths:
            inspect(path)


if __name__ == "__main__":
    main()
