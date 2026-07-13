"""
Data quality validation for generated training data.

Checks:
  1. No unresolved {placeholder} residues in assistant/user text
  2. Text length in acceptable range (15-300 chars for assistant)
  3. All required fields present (conversations, role, content)
  4. Exact deduplication
  5. Per-category statistics
  6. Print 3 random samples per category for manual inspection

Usage:
  python scripts/validate_data.py
  python scripts/validate_data.py --file scripts/data/training_data.jsonl
"""

import json
import os
import re
import sys
from collections import defaultdict
from typing import List, Dict, Any, Tuple


# =============================================================================
# Placeholder detection
# =============================================================================

# Pattern: {word} or {word:fmt} with optional format spec
PLACEHOLDER_RE = re.compile(r'\{[a-zA-Z_][a-zA-Z0-9_]*(?::[^}]*)?\}')

# Known valid placeholders (should all be filled)
KNOWN_PLACEHOLDERS = {
    'agl', 'alt', 'vs', 'ias', 'gs', 'mach', 'roll', 'pitch', 'hdg',
    'n1_0', 'n1_1', 'egt_0', 'egt_1', 'egt_max', 'flap', 'gear',
    'fuel', 'cdi', 'delta', 'phase', 'oat', 'cabin_alt', 'oil_press',
    'n1_diff', 'fuel_imb', 'bus_volts', 'apu_egt', 'apu_n1',
    'hours', 'fl', 'spdbrk', 'reverser', 'gear_status',
}

# Placeholders that are expected to remain in text_variants.py (template source)
# None should remain in final output!


def find_unresolved_placeholders(text: str) -> List[str]:
    """Find {placeholder} patterns that haven't been filled."""
    return PLACEHOLDER_RE.findall(text)


# =============================================================================
# Validation functions
# =============================================================================

def validate_record(record: dict, index: int) -> List[str]:
    """Validate a single ShareGPT record. Returns list of error messages."""
    errors = []

    # Check structure
    if "conversations" not in record:
        errors.append(f"Record {index}: missing 'conversations' key")
        return errors

    convs = record["conversations"]
    if len(convs) < 2:
        errors.append(f"Record {index}: conversations has only {len(convs)} entries (need 2+)")
        return errors

    roles = [c.get("role", "") for c in convs]

    # Check required roles
    if "user" not in roles:
        errors.append(f"Record {index}: missing 'user' role")
    if "assistant" not in roles:
        errors.append(f"Record {index}: missing 'assistant' role")

    # Validate each conversation turn
    for ci, conv in enumerate(convs):
        role = conv.get("role", f"missing_role_{ci}")
        content = conv.get("content", "")

        if not content:
            errors.append(f"Record {index}, {role}: empty content")
            continue

        # Check for unresolved placeholders
        unresolved = find_unresolved_placeholders(content)
        if unresolved:
            errors.append(f"Record {index}, {role}: unresolved placeholders: {unresolved}")

        # Length checks
        if role == "assistant":
            if len(content) < 10:
                errors.append(f"Record {index}, assistant: too short ({len(content)} chars)")
            elif len(content) > 350:
                errors.append(f"Record {index}, assistant: too long ({len(content)} chars)")

        if role == "user":
            if len(content) < 5:
                errors.append(f"Record {index}, user: too short ({len(content)} chars)")

    return errors


def validate_file(filepath: str) -> Tuple[List[dict], Dict[str, Any]]:
    """Validate all records in a JSONL file. Returns (records, stats)."""
    records = []
    errors_by_index: Dict[int, List[str]] = {}
    seen_hashes = set()
    duplicates = 0

    print(f"Loading: {filepath}")

    with open(filepath, "r", encoding="utf-8") as f:
        for i, line in enumerate(f):
            line = line.strip()
            if not line:
                continue

            try:
                record = json.loads(line)
            except json.JSONDecodeError as e:
                errors_by_index[i] = [f"JSON parse error: {e}"]
                continue

            records.append(record)

            # Validate
            errs = validate_record(record, i)
            if errs:
                errors_by_index[i] = errs

            # Dedup check (hash the assistant content + user content)
            try:
                user_content = ""
                asst_content = ""
                for c in record["conversations"]:
                    if c["role"] == "user":
                        user_content = c["content"]
                    elif c["role"] == "assistant":
                        asst_content = c["content"]
                hash_key = hash(user_content + "|||" + asst_content)
                if hash_key in seen_hashes:
                    duplicates += 1
                seen_hashes.add(hash_key)
            except (KeyError, TypeError):
                pass

    # Category stats
    category_counts = defaultdict(int)
    for r in records:
        cat = r.get("category", "unknown")
        category_counts[cat] += 1

    # Assistant length stats
    asst_lengths = []
    for r in records:
        for c in r.get("conversations", []):
            if c.get("role") == "assistant":
                asst_lengths.append(len(c["content"]))

    stats = {
        "file": filepath,
        "total_records": len(records),
        "error_count": len(errors_by_index),
        "duplicate_count": duplicates,
        "category_counts": dict(category_counts),
        "asst_len_min": min(asst_lengths) if asst_lengths else 0,
        "asst_len_max": max(asst_lengths) if asst_lengths else 0,
        "asst_len_avg": sum(asst_lengths) / len(asst_lengths) if asst_lengths else 0,
    }

    # Print errors
    if errors_by_index:
        print(f"\n  !! {len(errors_by_index)} records with errors:")
        for idx in sorted(errors_by_index.keys())[:20]:  # Show first 20
            for err in errors_by_index[idx]:
                print(f"    Line {idx}: {err}")
        if len(errors_by_index) > 20:
            print(f"    ... and {len(errors_by_index) - 20} more error records")

    return records, stats


# =============================================================================
# Print samples
# =============================================================================

def print_samples(records: List[dict], num_per_category: int = 3):
    """Print random samples from each category for manual inspection."""
    import random
    rng = random.Random(42)

    # Group by category
    by_category = defaultdict(list)
    for r in records:
        cat = r.get("category", "unknown")
        by_category[cat].append(r)

    print(f"\n{'=' * 60}")
    print("  Sample Outputs (random selection per category)")
    print(f"{'=' * 60}")

    for cat in sorted(by_category.keys()):
        pool = by_category[cat]
        n = min(num_per_category, len(pool))
        samples = rng.sample(pool, n)

        print(f"\n--- {cat} ({len(pool)} total) ---")
        for si, sample in enumerate(samples):
            for conv in sample.get("conversations", []):
                role = conv.get("role", "?")
                content = conv.get("content", "")
                if role == "system":
                    print(f"  [{role}]: {content[:100]}...")
                elif role == "user":
                    # Just show first 2 lines
                    lines = content.split("\n")
                    preview = "\n           ".join(lines[:3])
                    if len(lines) > 3:
                        preview += f"\n           ... ({len(lines)} lines total)"
                    print(f"  [{role}]: {preview}")
                elif role == "assistant":
                    print(f"  [{role}]: {content}")
            print()


# =============================================================================
# Main
# =============================================================================

def main():
    # Determine file(s) to validate
    script_dir = os.path.dirname(os.path.abspath(__file__))
    data_dir = os.path.join(script_dir, "data")

    files_to_check = []
    if "--file" in sys.argv:
        idx = sys.argv.index("--file")
        if idx + 1 < len(sys.argv):
            files_to_check.append(sys.argv[idx + 1])
    else:
        # Default: check both train and eval
        train_file = os.path.join(data_dir, "training_data.jsonl")
        eval_file = os.path.join(data_dir, "eval_data.jsonl")
        if os.path.exists(train_file):
            files_to_check.append(train_file)
        if os.path.exists(eval_file):
            files_to_check.append(eval_file)

    if not files_to_check:
        print("ERROR: No data files found. Run generate_training_data.py first.")
        print(f"Expected files in: {data_dir}")
        sys.exit(1)

    all_ok = True

    for filepath in files_to_check:
        if not os.path.exists(filepath):
            print(f"ERROR: File not found: {filepath}")
            all_ok = False
            continue

        print(f"\n{'=' * 60}")
        print(f"  Validating: {os.path.basename(filepath)}")
        print(f"{'=' * 60}")

        records, stats = validate_file(filepath)

        # Print stats
        print(f"\n  [Stats]")
        print(f"    Total records:     {stats['total_records']}")
        print(f"    Errors:            {stats['error_count']}")
        print(f"    Duplicates:        {stats['duplicate_count']}")
        print(f"    Asst len range:    {stats['asst_len_min']}-{stats['asst_len_max']} chars")
        print(f"    Asst len avg:      {stats['asst_len_avg']:.1f} chars")

        print(f"\n  [Category distribution]")
        for cat, count in sorted(stats["category_counts"].items()):
            print(f"    {cat:<15}: {count:>6}")

        # Print samples
        print_samples(records, num_per_category=2)

        if stats["error_count"] > 0:
            all_ok = False
        if stats["duplicate_count"] > stats["total_records"] * 0.05:
            print(f"\n  !! Warning: {stats['duplicate_count']} duplicates (>5%)")
            all_ok = False

    # Summary
    print(f"\n{'=' * 60}")
    if all_ok:
        print("  [PASS] Validation PASSED — data quality OK")
    else:
        print("  [FAIL] Validation FAILED — review errors above")
    print(f"{'=' * 60}")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
