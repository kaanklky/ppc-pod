#!/usr/bin/env python3

import os
import sys
import string

MAX_SYMLINK_TARGET_LEN = 300
PRINTABLE = set(string.ascii_letters + string.digits + "._-/+")

def looks_like_path(text):
    if not text:
        return False
    if len(text) > MAX_SYMLINK_TARGET_LEN:
        return False
    return all(c in PRINTABLE for c in text)

def resolve_target(file_path, link_text):
    base_dir = os.path.dirname(file_path)
    if link_text.startswith("/"):
        return link_text, os.path.exists(link_text)
    candidate = os.path.normpath(os.path.join(base_dir, link_text))
    return candidate, os.path.exists(candidate)

def scan_pass(sdk_root):
    fixed = 0
    skipped_empty = 0
    ambiguous = []

    for dirpath, dirnames, filenames in os.walk(sdk_root):
        for name in filenames:
            path = os.path.join(dirpath, name)
            if os.path.islink(path):
                continue
            try:
                size = os.path.getsize(path)
            except OSError:
                continue
            if size == 0:
                skipped_empty += 1
                continue
            if size > MAX_SYMLINK_TARGET_LEN:
                continue
            try:
                with open(path, "rb") as f:
                    raw = f.read()
            except OSError:
                continue
            if b"\x00" in raw:
                continue
            try:
                text = raw.decode("ascii").strip()
            except UnicodeDecodeError:
                continue
            if not looks_like_path(text):
                continue

            if text.startswith("/"):
                os.remove(path)
                os.symlink(text, path)
                fixed += 1
                continue

            target, exists = resolve_target(path, text)
            if not exists:
                ambiguous.append((path, text))
                continue
            os.remove(path)
            os.symlink(text, path)
            fixed += 1

    return fixed, skipped_empty, ambiguous

def main():
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <sdk-root>", file=sys.stderr)
        sys.exit(1)

    sdk_root = sys.argv[1]
    total_fixed = 0
    skipped_empty = 0
    ambiguous = []
    round_num = 0

    while True:
        round_num += 1
        fixed, skipped_empty, ambiguous = scan_pass(sdk_root)
        total_fixed += fixed
        print(f"pass {round_num}: converted {fixed} symlinks")
        if fixed == 0:
            break

    print(f"\ntotal fixed (converted to real symlinks): {total_fixed}")
    print(f"skipped (legitimately empty files): {skipped_empty}")
    print(f"ambiguous (small text file, target not found after fixed-point, left untouched): {len(ambiguous)}")
    if ambiguous:
        print("\nambiguous files (review manually - likely legitimate small content files):")
        for path, text in ambiguous[:50]:
            print(f"  {path} -> {text!r}")
        if len(ambiguous) > 50:
            print(f"  ... and {len(ambiguous) - 50} more")

if __name__ == "__main__":
    main()
