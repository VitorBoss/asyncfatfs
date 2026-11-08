"""Bounded allocation-protocol model; does not execute the library or model SD timing.

Enumerates entry-aligned prefix tears to isolate allocation ordering from torn
individual FAT entries. This is deliberately not a full power-loss proof.
"""

import argparse
import json
import struct
from pathlib import Path


SECTOR_SIZE = 512
ENTRY_COUNT = 128
END_OF_CHAIN = 0x0FFFFFFF
OLD_TAIL = 32
FIRST_NEW = OLD_TAIL + 1


def sector_image(entries):
    return struct.pack("<128I", *entries)


def reachable_clusters(entries):
    reachable = set()
    current = OLD_TAIL
    while current != END_OF_CHAIN:
        assert OLD_TAIL <= current < ENTRY_COUNT, "out-of-range link"
        assert current not in reachable, "cycle"
        reachable.add(current)
        current = entries[current]
        assert current != 0, "link to a free cluster"
    return reachable


def orphan_count(entries):
    reachable = reachable_clusters(entries)
    return sum(value != 0 and index not in reachable for index, value in enumerate(entries))


def investigate(batch_size, staged):
    entries = [0] * ENTRY_COUNT
    entries[OLD_TAIL] = END_OF_CHAIN
    copies = [entries.copy(), entries.copy()]
    prepared = entries.copy()
    linked = entries.copy()
    for cluster in range(FIRST_NEW, FIRST_NEW + batch_size):
        prepared[cluster] = END_OF_CHAIN
        linked[cluster] = cluster + 1 if cluster + 1 < FIRST_NEW + batch_size else END_OF_CHAIN
    linked[OLD_TAIL] = FIRST_NEW
    images = [prepared, linked] if staged else [linked]
    checked_prefixes = 0
    invalid_prefixes = 0
    preimage_violations = 0
    maximum_orphans = 0
    for image in images:
        for copy_index in range(2):
            previous = copies[copy_index]
            # Every proposed link must target a previously allocated entry in
            # BOTH durable copies, rather than another change in this image.
            for cluster, target in enumerate(image):
                if target not in (0, END_OF_CHAIN) and target != previous[cluster]:
                    if any(durable[target] == 0 for durable in copies):
                        preimage_violations += 1
            old_bytes = sector_image(previous)
            new_bytes = sector_image(image)
            for prefix in range(0, SECTOR_SIZE + 1, 4):
                torn = list(struct.unpack("<128I", new_bytes[:prefix] + old_bytes[prefix:]))
                checked_prefixes += 1
                try:
                    maximum_orphans = max(maximum_orphans, orphan_count(torn))
                except AssertionError:
                    invalid_prefixes += 1
            copies[copy_index] = image.copy()
    assert copies[0] == copies[1]
    assert len(reachable_clusters(copies[0])) == batch_size + 1
    return {
        "batch_clusters": batch_size,
        "protocol": "eoc_ack_both_then_links_ack_both" if staged else "combined_eoc_and_links",
        "fat_sector_writes": len(images) * 2,
        "prefix_states_checked": checked_prefixes,
        "invalid_prefix_states": invalid_prefixes,
        "links_without_durable_target_at_submission": preimage_violations,
        "maximum_unreachable_allocated_clusters_per_copy": maximum_orphans,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    arguments = parser.parse_args()
    results = []
    for batch_size in (1, 2, 4, 8, 16, 32, 64):
        staged = investigate(batch_size, True)
        combined = investigate(batch_size, False)
        assert staged["invalid_prefix_states"] == 0
        assert staged["links_without_durable_target_at_submission"] == 0
        assert staged["maximum_unreachable_allocated_clusters_per_copy"] == batch_size
        assert combined["invalid_prefix_states"] > 0
        assert combined["links_without_durable_target_at_submission"] > 0
        results.extend((staged, combined))
    payload = {
        "scope": "one FAT sector per copy, contiguous batch, existing one-cluster file",
        "tear_model": "all 129 entry-aligned byte-prefix lengths per write",
        "excludes": ["individual-entry byte tears", "backend failures", "library state machine",
                     "cache scheduling", "data and directory IO", "remount recovery", "timing"],
        "results": results,
    }
    arguments.output.parent.mkdir(parents=True, exist_ok=True)
    arguments.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Passed: {sum(result['prefix_states_checked'] for result in results)} prefix states; "
          f"{len(results)} protocols/batch sizes. Evidence: {arguments.output}")


if __name__ == "__main__":
    main()
