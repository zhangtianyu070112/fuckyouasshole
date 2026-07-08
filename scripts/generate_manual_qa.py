"""
Convert B737 SOP/QRH/Limitations knowledge base into ShareGPT training data.

Reads manual_knowledge.json, generates Q&A pairs for each knowledge entry,
and outputs ShareGPT-format JSONL that can be merged with the alert training data.

Each knowledge entry generates 3-5 Q&A pairs:
  - 1-2: Direct question → short answer (pure knowledge recall)
  - 1-2: Contextual question with flight parameters → answer applying the knowledge
  - 0-1: Scenario-based question → procedural answer

Usage:
  python scripts/generate_manual_qa.py
  python scripts/generate_manual_qa.py --merge   # merge with training data
"""

import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from text_variants import SYSTEM_PROMPT

# =============================================================================
# Configuration
# =============================================================================

KNOWLEDGE_FILE = os.path.join(os.path.dirname(__file__), "manual_knowledge.json")
OUTPUT_FILE = os.path.join(os.path.dirname(__file__), "data", "manual_qa.jsonl")
TRAIN_FILE = os.path.join(os.path.dirname(__file__), "data", "training_data.jsonl")
EVAL_FILE = os.path.join(os.path.dirname(__file__), "data", "eval_data.jsonl")
MERGED_TRAIN = os.path.join(os.path.dirname(__file__), "data", "training_with_manual.jsonl")
MERGED_EVAL = os.path.join(os.path.dirname(__file__), "data", "eval_with_manual.jsonl")

BASE_SEED = 42

# =============================================================================
# Context templates — mix flight parameters into user questions
# =============================================================================

CONTEXT_PREFIXES = [
    "当前飞行阶段：{phase}，高度{alt:.0f}英尺，空速{ias:.0f}节。",
    "飞机当前{phase}，{alt:.0f}英尺，N1 {n1_0:.0f}%/{n1_1:.0f}%。",
    "当前在{phase}阶段，高度{alt:.0f}英尺AGL。",
    "飞机在{phase}，IAS {ias:.0f}节，燃油{fuel:.0f}磅。",
    "B737-800在{phase}中，高度{alt:.0f}英尺，{ias:.0f}节。",
]

PHASE_CN = ["起飞", "初始爬升", "爬升至巡航", "巡航", "下降", "进近", "着陆", "滑行"]

# =============================================================================
# Scenario generators for contextual Q&A
# =============================================================================

def generate_phase_params(rng):
    """Generate plausible random flight parameters."""
    phase_idx = rng.randint(0, 7)
    phase = PHASE_CN[phase_idx]
    if phase_idx == 0:
        alt = rng.uniform(0, 200)
        ias = rng.uniform(120, 160)
    elif phase_idx in (1, 2):
        alt = rng.uniform(5000, 30000)
        ias = rng.uniform(200, 280)
    elif phase_idx == 3:
        alt = rng.uniform(33000, 37000)
        ias = rng.uniform(270, 290)
    elif phase_idx == 4:
        alt = rng.uniform(10000, 35000)
        ias = rng.uniform(250, 280)
    elif phase_idx == 5:
        alt = rng.uniform(500, 5000)
        ias = rng.uniform(130, 150)
    else:
        alt = rng.uniform(0, 100)
        ias = rng.uniform(30, 140)

    return {
        "phase": phase,
        "alt": alt,
        "ias": ias,
        "n1_0": rng.uniform(22, 98),
        "n1_1": rng.uniform(22, 98),
        "fuel": rng.uniform(1000, 15000),
    }


def contextualize_question(question, params):
    """Add flight context to a question."""
    prefix_tmpl = random.Random().choice(CONTEXT_PREFIXES)
    prefix = prefix_tmpl.format(**params)
    return f"{prefix}\n{question}"


# =============================================================================
# Main generation
# =============================================================================

def generate_manual_qa(knowledge_file, output_file, rng_seed=42):
    """Generate ShareGPT Q&A pairs from knowledge base with context variants."""
    rng = random.Random(rng_seed)

    with open(knowledge_file, "r", encoding="utf-8") as f:
        kb = json.load(f)

    records = []

    CONTEXT_MODES = ["pure", "contextual", "scenario", "extra_context",
                      "alt_context", "short_context", "phase_specific", "emergency_prefix"]

    for section_key in ["limitations", "memory_items", "normal_sop", "systems", "sop_flows", "comms_weather"]:
        entries = kb.get(section_key, [])
        for entry in entries:
            questions = entry.get("questions", [])
            short_answer = entry.get("short_answer", "")
            triggers = entry.get("triggers", [])
            title = entry.get("title", "")

            if not questions or not short_answer:
                continue

            for question in questions:
                for vi in range(8):  # 8 context variants per question for solid knowledge injection
                    mode_idx = vi % len(CONTEXT_MODES)
                    mode = CONTEXT_MODES[mode_idx]

                    if mode == "pure":
                        user_text = question

                    elif mode == "contextual":
                        params = generate_phase_params(rng)
                        user_text = contextualize_question(question, params)

                    elif mode == "scenario":
                        if triggers:
                            trigger = rng.choice(triggers)
                            params = generate_phase_params(rng)
                            user_text = f"当前{params['phase']}阶段，{params['alt']:.0f}英尺，触发了{trigger}告警。{question}"
                        else:
                            params = generate_phase_params(rng)
                            user_text = contextualize_question(question, params)

                    elif mode == "extra_context":
                        params = generate_phase_params(rng)
                        # More detailed context
                        detail = (
                            f"当前{params['phase']}阶段，高度{params['alt']:.0f}英尺，"
                            f"空速{params['ias']:.0f}节，"
                            f"左侧N1 {params['n1_0']:.0f}%，右侧N1 {params['n1_1']:.0f}%，"
                            f"剩余燃油{params['fuel']:.0f}磅。\n{question}"
                        )
                        user_text = detail

                    elif mode == "alt_context":
                        params2 = generate_phase_params(rng)
                        user_text = (
                            f"当前{params2['phase']}，高度{params2['alt']:.0f}英尺，"
                            f"IAS {params2['ias']:.0f}节。{question}"
                        )

                    elif mode == "short_context":
                        params3 = generate_phase_params(rng)
                        user_text = f"({params3['phase']}，{params3['alt']:.0f}ft，{params3['ias']:.0f}kt) {question}"

                    elif mode == "phase_specific":
                        params4 = generate_phase_params(rng)
                        if triggers:
                            trigger = rng.choice(triggers)
                            user_text = f"B737-800飞行中，{params4['phase']}阶段，{params4['ias']:.0f}节。{trigger}告警条件下：{question}"
                        else:
                            user_text = f"B737-800飞行中，{params4['phase']}阶段，{params4['ias']:.0f}节。{question}"

                    else:  # emergency_prefix — adds urgency to the question
                        params5 = generate_phase_params(rng)
                        urgency = rng.choice(["紧急咨询：", "快速检查：", "立即回答：", ""])
                        user_text = f"{urgency}{question}（{params5['phase']}，{params5['alt']:.0f}英尺）"

                    record = {
                        "conversations": [
                            {"role": "system", "content": SYSTEM_PROMPT},
                            {"role": "user", "content": user_text},
                            {"role": "assistant", "content": short_answer},
                        ],
                        "category": "F_MANUAL",
                        "knowledge_id": entry.get("id", "?"),
                        "section": section_key,
                        "title": title,
                    }
                    records.append(record)

    # Shuffle
    rng.shuffle(records)

    # Write output
    with open(output_file, "w", encoding="utf-8") as f:
        for r in records:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")

    # Stats
    section_counts = {}
    for r in records:
        sec = r.get("section", "?")
        section_counts[sec] = section_counts.get(sec, 0) + 1

    print(f"Generated {len(records)} manual Q&A samples")
    print(f"\nSection distribution:")
    for sec, count in sorted(section_counts.items()):
        print(f"  {sec:<20}: {count:>5}")
    print(f"\nWritten to: {output_file}")

    return records


def merge_with_training(train_file, eval_file, manual_file, merged_train, merged_eval):
    """Merge manual Q&A with existing training data."""
    rng = random.Random(42)

    # Load manual QA
    manual_records = []
    with open(manual_file, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                manual_records.append(json.loads(line))

    # Load existing training
    train_records = []
    with open(train_file, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                train_records.append(json.loads(line))

    eval_records = []
    with open(eval_file, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line:
                eval_records.append(json.loads(line))

    # Split manual QA: 90% train, 10% eval
    rng.shuffle(manual_records)
    split_idx = int(len(manual_records) * 0.9)
    manual_train = manual_records[:split_idx]
    manual_eval = manual_records[split_idx:]

    # Merge
    merged_train_data = train_records + manual_train
    merged_eval_data = eval_records + manual_eval
    rng.shuffle(merged_train_data)
    rng.shuffle(merged_eval_data)

    # Write
    with open(merged_train, "w", encoding="utf-8") as f:
        for r in merged_train_data:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")

    with open(merged_eval, "w", encoding="utf-8") as f:
        for r in merged_eval_data:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")

    print(f"\nMerge results:")
    print(f"  Original train:   {len(train_records)}")
    print(f"  Manual QA train:  {len(manual_train)}")
    print(f"  Merged train:     {len(merged_train_data)}")
    print(f"  Original eval:    {len(eval_records)}")
    print(f"  Manual QA eval:   {len(manual_eval)}")
    print(f"  Merged eval:      {len(merged_eval_data)}")

    # Stats by category
    cat_counts = {}
    for r in merged_train_data:
        cat = r.get("category", "unknown")
        cat_counts[cat] = cat_counts.get(cat, 0) + 1
    print(f"\n  Train category distribution:")
    for cat, count in sorted(cat_counts.items()):
        pct = count / len(merged_train_data) * 100
        print(f"    {cat:<15}: {count:>6} ({pct:5.1f}%)")


# =============================================================================
# Main
# =============================================================================

def main():
    print("=" * 60)
    print("  B737 Manual Knowledge → Q&A Training Data Generator")
    print("=" * 60)
    print()

    print(f"[1/2] Building Q&A from knowledge base...")
    records = generate_manual_qa(KNOWLEDGE_FILE, OUTPUT_FILE, BASE_SEED)

    if "--merge" in sys.argv:
        print(f"\n[2/2] Merging with existing training data...")
        merge_with_training(TRAIN_FILE, EVAL_FILE, OUTPUT_FILE,
                           MERGED_TRAIN, MERGED_EVAL)
    else:
        print(f"\n[2/2] Skipping merge (use --merge to merge).")
        print(f"  Output: {OUTPUT_FILE} ({len(records)} records)")

    print(f"\n{'=' * 60}")
    print("  Complete!")
    print(f"{'=' * 60}")


if __name__ == "__main__":
    main()
