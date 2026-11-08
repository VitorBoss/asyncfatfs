"""Independently read VFAT names, aliases and data emitted by AFATFS_LFN_IMAGE_DIR."""
import argparse
import json
from importlib.metadata import version
from pathlib import Path

from exfat_interop import Chain, close_volume, volume


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=Path, required=True)
    args = parser.parse_args()
    names = ("First long filename.txt", "First long filenames.txt", "First long file notes.txt", "MixedCase.txt",
             "1234567890123", "12345678901234567890123456", ".hidden file", "multiple.parts.of.a.name.bin",
             "valid+comma,equals=brackets[].txt")
    report = {"status": "incomplete", "fattools_version": version("FATtools"), "results": []}
    try:
        images = sorted(args.images.glob("lfn-fat*-*.img"))
        assert len(images) == 6, "Expected FAT16 spc=1/4 and FAT32 spc=1, each with names and boundary images"
        for image in images:
            root = volume(image)
            try:
                if image.stem.endswith("-names"):
                    for index, name in enumerate(names):
                        entry = root.find(name)
                        assert entry is not None and entry.Name() == name, (image, name)
                        assert entry.dwFileSize == 4, (image, name)
                        assert bytes(Chain(root.boot, root.fat, entry.Start(), 4).read()) == bytes((0x18, 0xFA, 0x32, 0x90))
                        if index < 3:
                            assert entry.ShortName() == f"FIRSTL~{index + 1}.TXT"
                else:
                    directory = root.opendir("BOUNDARY")
                    entry = directory.find("Z" * 255)
                    assert entry is not None and entry.Name() == "Z" * 255, image
                    assert entry.dwFileSize == 512, image
                    assert bytes(Chain(root.boot, root.fat, entry.Start(), 512).read()) == b"\x39" * 512
                report["results"].append({"image": image.name, "passed": True})
                print(f"PASS FATtools: {image.name}")
            finally:
                close_volume(root)
        report["status"] = "passed"
    finally:
        (args.images / "verification.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
