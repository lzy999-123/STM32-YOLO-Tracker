"""Check distinct source frames and Qt paints; never infer optical latency from FPS.

Input is tests/video_cadence_probe JSON. Exit 0 requires a continuous run;
one-frame boundary differences (29/31) are reported, never averaged away.
"""
import argparse
import collections
import json
import math
from pathlib import Path


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    i = int(position)
    return ordered[i] + (ordered[min(i + 1, len(ordered) - 1)] - ordered[i]) * (position - i)


def analyze_samples(samples, seconds, source_pts_available=True):
    unique = []
    previous = None
    repeated_paints = 0
    for sample in samples:
        identity = tuple(sample[1:4])  # session, sequence, source PTS
        if identity == previous:
            repeated_paints += 1
            continue
        unique.append(sample)
        previous = identity
    buckets = collections.Counter(int(sample[0] // 1000) for sample in unique)
    per_second = [buckets[i] for i in range(math.floor(seconds))]
    gaps = [b[0] - a[0] for a, b in zip(unique, unique[1:])]
    pts_steps = [b[3] - a[3] for a, b in zip(unique, unique[1:]) if a[1] == b[1]] if source_pts_available else []
    source_missing = sum(max(0, round(step * 30 / 1000000) - 1) for step in pts_steps) if source_pts_available else None
    source_backwards = sum(step <= 0 for step in pts_steps) if source_pts_available else None
    return {
        "distinct_frames": len(unique), "repeated_paint_events": repeated_paints,
        "full_seconds": len(per_second), "per_second_distinct_frames": per_second,
        "minimum_per_second": min(per_second, default=0), "maximum_per_second": max(per_second, default=0),
        "seconds_outside_29_31": [i for i, count in enumerate(per_second) if not 29 <= count <= 31],
        "source_missing_slots": source_missing, "source_nonmonotonic_steps": source_backwards,
        "source_pts_step_us_min": min(pts_steps, default=None), "source_pts_step_us_max": max(pts_steps, default=None),
        "gap_ms": {"p50": percentile(gaps, .50), "p95": percentile(gaps, .95),
                   "p99": percentile(gaps, .99), "maximum": max(gaps, default=seconds * 1000),
                   "over_50ms": sum(g > 50 for g in gaps), "under_20ms": sum(g < 20 for g in gaps)},
        "leading_ms": unique[0][0] if unique else seconds * 1000,
        "trailing_ms": max(0, seconds * 1000 - unique[-1][0]) if unique else seconds * 1000,
    }


def analyze(data):
    summary = data["summary"]
    seconds = summary.get("seconds", 0)
    source_pts_available = summary.get("source_pts_available", True)
    result = {"summary": summary}
    for key in ("submitted", "painted"):
        result[key] = analyze_samples(data.get(key, []), seconds, source_pts_available)
    if "decoded" in data:
        result["decoded"] = analyze_samples(data["decoded"], seconds, source_pts_available)
        lags = [sample[4] for sample in data["decoded"]]
        result["receiver_backlog_ms"] = {"p50": percentile(lags, .5), "p95": percentile(lags, .95),
                                         "maximum": max(lags, default=None)}
    p = result["painted"]
    result["continuous_cadence_pass"] = bool(
        source_pts_available and not summary.get("startup_failed") and seconds >= 60
        and summary.get("frame_width") == 1280 and summary.get("frame_height") == 720
        and not any(event[0] >= 0 for event in summary.get("camera_errors", []))
        and not any(event[0] >= 0 and event[1] != 2 for event in summary.get("camera_states", []))
        and not p["seconds_outside_29_31"] and p["source_missing_slots"] == 0
        and p["source_nonmonotonic_steps"] == 0
        and p["gap_ms"]["over_50ms"] == 0 and p["gap_ms"]["under_20ms"] == 0
        and p["leading_ms"] <= 50 and p["trailing_ms"] <= 50
    )
    result["latency_note"] = "Qt paint timing is not synchronized camera-to-screen latency."
    if not source_pts_available:
        result["source_timing_note"] = "Source PTS is unavailable; arrival and paint counts cannot certify source cadence."
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    result = analyze(json.loads(args.input.read_text(encoding="utf-8")))
    text = json.dumps(result, ensure_ascii=False, indent=2)
    if args.output:
        args.output.write_text(text + "\n", encoding="utf-8")
    print(text)
    return 0 if result["continuous_cadence_pass"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
