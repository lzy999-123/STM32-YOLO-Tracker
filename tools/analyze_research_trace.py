"""Audit opt-in host metadata; never certify synchronization or motor safety.

Optional LOS output is a diagnostic using HOST arrival times, not acquisition
times. Raw traces are read-only and outputs are exclusively created.
"""
import argparse
from bisect import bisect_right
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics


def is_number(value):
    return type(value) in (int, float) and math.isfinite(value)


def quantile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return None
    position = (len(ordered) - 1) * fraction
    left = int(position)
    return ordered[left] + (ordered[min(left + 1, len(ordered) - 1)] - ordered[left]) * (position - left)


def summary(values):
    return {"n": len(values), "median": quantile(values, 0.5),
            "p95": quantile(values, 0.95), "maximum": max(values, default=None)}


def load_trace(path):
    raw = Path(path).read_bytes()
    rows = []
    for number, line in enumerate(raw.decode("utf-8-sig").splitlines(), 1):
        if not line.strip():
            raise ValueError(f"Blank record at line {number}")
        row = json.loads(line)
        if not isinstance(row, dict):
            raise ValueError(f"Record {number} is not an object")
        rows.append(row)
    return rows, hashlib.sha256(raw).hexdigest()


def audit(rows):
    issues = []
    if not rows:
        return {"trace_integrity_pass": False, "issues": ["empty_trace"],
                "synchronized_los_ready": False}
    first, last = rows[0], rows[-1]
    if first.get("kind") != "run_start":
        issues.append("missing_run_start")
    if last.get("kind") != "run_end":
        issues.append("missing_run_end")
    if last.get("complete") is not True or last.get("dropped_events") != 0:
        issues.append("dropped_or_incomplete_trace")
    run_id = first.get("run_id")
    if not isinstance(run_id, str) or not run_id:
        issues.append("missing_run_id")
    if first.get("clock") != "host_steady_clock_us":
        issues.append("unexpected_clock")
    counts, frames, seen_observations = Counter(), {}, set()
    gaps, inference_ms, pipeline_ms = [], [], []
    stream_previous = {}
    previous_time = -1
    fresh = 0
    for index, row in enumerate(rows):
        kind = row.get("kind")
        counts[str(kind)] += 1
        if row.get("schema_version") != 1 or row.get("run_id") != run_id:
            issues.append(f"schema_or_run_mismatch:{index}")
        if type(row.get("event_seq")) is not int or row["event_seq"] != index:
            issues.append(f"event_sequence_gap:{index}")
        now = row.get("host_mono_us")
        if type(now) is not int or now < previous_time:
            issues.append(f"invalid_host_time:{index}")
        else:
            previous_time = now
        if kind == "frame":
            key = (row.get("stream_id"), row.get("frame_id"))
            received = row.get("host_receive_us")
            if (type(key[0]) is not int or type(key[1]) is not int or key[1] <= 0
                    or key in frames or not is_number(received) or received < 0
                    or not is_number(now) or received > now):
                issues.append(f"invalid_frame:{index}")
                continue
            frames[key] = row
            old = stream_previous.get(key[0])
            if old is not None:
                if received < old:
                    issues.append(f"frame_receive_time_reversed:{index}")
                else:
                    gaps.append((received - old) / 1000)
            stream_previous[key[0]] = received
        elif kind == "observation":
            key = (row.get("stream_id"), row.get("frame_id"))
            identity = (row.get("dnn_session"),) + key
            if identity in seen_observations:
                issues.append(f"duplicate_observation:{index}")
            seen_observations.add(identity)
            frame = frames.get(key)
            if frame is None or row.get("host_receive_us") != frame.get("host_receive_us"):
                issues.append(f"unmatched_observation_frame:{index}")
            start, done = row.get("inference_start_us"), row.get("inference_done_us")
            received = row.get("host_receive_us")
            if (not all(is_number(t) for t in (start, done, now, received))
                    or not 0 <= received <= start <= done <= now):
                issues.append(f"invalid_pipeline_time:{index}")
            else:
                inference_ms.append((done - start) / 1000)
                pipeline_ms.append((done - received) / 1000)
            if row.get("fresh_measurement") is True:
                if row.get("outcome") != "detected" or row.get("session_current_at_completion") is not True:
                    issues.append(f"held_or_stale_marked_fresh:{index}")
                else:
                    fresh += 1
        elif kind == "telemetry":
            received = row.get("host_receive_us")
            if (not is_number(received) or not is_number(now) or not 0 <= received <= now
                    or not all(is_number(row.get(k)) for k in ("horizontal_deg", "vertical_deg"))):
                issues.append(f"invalid_telemetry:{index}")
    if counts["run_start"] != 1 or counts["run_end"] != 1:
        issues.append("multiple_or_missing_run_boundaries")
    blockers = ["camera_acquisition_clock_not_mapped", "encoder_acquisition_time_unavailable",
                "calibration_and_physical_sign_require_independent_verification",
                "target_identity_and_static_scene_require_independent_annotation",
                "firmware_and_source_build_manifest_require_recording"]
    if not fresh:
        blockers.append("no_fresh_matched_detections")
    if not counts["telemetry"]:
        blockers.append("no_encoder_telemetry")
    if not counts["model_loaded"]:
        blockers.append("no_model_provenance")
    if issues:
        blockers.append("invalid_or_incomplete_trace")
    return {"trace_integrity_pass": not issues, "issues": sorted(set(issues)),
            "event_counts": dict(counts), "fresh_detection_count": fresh,
            "frame_host_receive_gap_ms": summary(gaps),
            "preprocess_inference_postprocess_ms": summary(inference_ms),
            "host_frame_receive_to_result_ms": summary(pipeline_ms),
            "synchronized_los_ready": False, "blockers": blockers,
            "optical_end_to_end_latency_ms": None,
            "warning": "Host durations exclude upstream exposure/encoding/network delay; not physical response."}


def validate_calibration(calibration):
    if calibration.get("schema_version") != 1:
        raise ValueError("Unsupported calibration schema")
    if calibration.get("reviewed_for_diagnostics") is not True:
        raise ValueError("Calibration must be reviewed for diagnostic use")
    if calibration.get("horizontal_sign") not in (-1, 1):
        raise ValueError("Explicit physically checked horizontal_sign (+1/-1) required")
    if not is_number(calibration.get("encoder_zero_deg")):
        raise ValueError("Explicit encoder_zero_deg required")
    if type(calibration.get("rotation")) is not int or calibration["rotation"] not in range(6):
        raise ValueError("Invalid image rotation")
    if not calibration.get("source_images"):
        raise ValueError("Missing calibration image provenance")
    matrix = calibration.get("camera_matrix")
    if (not isinstance(matrix, list) or len(matrix) != 3
            or any(not isinstance(row, list) or len(row) != 3 for row in matrix)
            or not all(is_number(v) for row in matrix for v in row)
            or matrix[0][0] <= 0 or matrix[1][1] <= 0
            or matrix[2] != [0, 0, 1] or matrix[0][1] != 0 or matrix[1][0] != 0):
        raise ValueError("Invalid OpenCV camera_matrix")
    distortion = calibration.get("distortion_coefficients")
    if (not isinstance(distortion, list) or len(distortion) not in (4,5,8,12,14)
            or not all(is_number(v) for v in distortion)):
        raise ValueError("Invalid distortion coefficients")
    size = calibration.get("image_size")
    if not isinstance(size, list) or len(size) != 2 or any(type(v) is not int or v <= 0 for v in size):
        raise ValueError("Invalid calibration image_size")


def approximate_los(rows, calibration, max_age_ms):
    """Causal prior telemetry only. Never interpolate using future arrivals."""
    validate_calibration(calibration)
    if not is_number(max_age_ms) or max_age_ms <= 0:
        raise ValueError("max_age_ms must be positive and explicitly chosen")
    if not audit(rows)["trace_integrity_pass"]:
        raise ValueError("Trace integrity failed; LOS diagnostics refused")
    import cv2
    import numpy as np
    matrix = np.asarray(calibration["camera_matrix"], dtype=np.float64)
    distortion = np.asarray(calibration["distortion_coefficients"], dtype=np.float64)
    # Ordered by host log time, so only already available telemetry can enter.
    telemetry, received_times, samples, rejects = [], [], [], Counter()
    for row in rows:
        if row["kind"] == "telemetry":
            if received_times and row["host_receive_us"] < received_times[-1]:
                raise ValueError("Telemetry host receive time reversed")
            telemetry.append(row)
            received_times.append(row["host_receive_us"])
        if row["kind"] != "observation":
            continue
        if row.get("fresh_measurement") is not True:
            rejects["not_fresh"] += 1
            continue
        if [row.get("width"), row.get("height")] != calibration["image_size"] or row.get("rotation") != calibration["rotation"]:
            rejects["geometry_mismatch"] += 1
            continue
        candidate = bisect_right(received_times, row["host_receive_us"]) - 1
        if candidate < 0:
            rejects["no_prior_telemetry"] += 1
            continue
        encoder = telemetry[candidate]
        # Do not cross a reconnection, fault, or invalid calibration seen before decision.
        recent = telemetry[candidate:]
        if any(t.get("connection_id") != encoder.get("connection_id") or t.get("flags") != 0
               or t.get("calibration_valid") is not True for t in recent):
            rejects["fault_or_session_boundary"] += 1
            continue
        age_ms = (row["host_receive_us"] - encoder["host_receive_us"]) / 1000
        if age_ms > max_age_ms:
            rejects["stale_telemetry"] += 1
            continue
        x, y, w, h = (row.get(k) for k in ("raw_x", "raw_y", "raw_w", "raw_h"))
        if not all(is_number(v) for v in (x,y,w,h)) or min(w,h) <= 0 or x < 0 or y < 0 or x+w > row["width"] or y+h > row["height"]:
            rejects["invalid_box"] += 1
            continue
        point = np.array([[[x+w/2, y+h/2]]], dtype=np.float64)
        ray = cv2.undistortPoints(point, matrix, distortion)[0,0]
        alpha = calibration["horizontal_sign"] * math.degrees(math.atan(float(ray[0])))
        theta = encoder["horizontal_deg"] - calibration["encoder_zero_deg"]
        los = theta + alpha
        if not math.isfinite(los):
            rejects["invalid_projection"] += 1
            continue
        samples.append({"frame_id": row["frame_id"], "stream_id": row["stream_id"],
                        "dnn_session": row["dnn_session"], "connection_id": encoder["connection_id"],
                        "host_receive_us": row["host_receive_us"], "encoder_event_seq": encoder["event_seq"],
                        "encoder_host_age_ms": age_ms, "approx_los_deg": los})
    # Session-specific de-meaning avoids mixing different targets or reconnect origins.
    groups = {}
    for sample in samples:
        key = (sample["stream_id"], sample["dnn_session"], sample["connection_id"])
        groups.setdefault(key, []).append(sample["approx_los_deg"])
    statistics_by_group = []
    for key, values in groups.items():
        center = statistics.mean(values)
        deviation = [abs(x-center) for x in values]
        statistics_by_group.append({"stream_id":key[0], "dnn_session":key[1], "connection_id":key[2],
            "n":len(values), "mean_deg":center, "demeaned_rms_deg":math.sqrt(statistics.mean(x*x for x in deviation)),
            "p95_absolute_deviation_deg":quantile(deviation,.95), "peak_to_peak_deg":max(values)-min(values)})
    return {"evidence_scope":"approximate_host_arrival_alignment_only", "acceptance_pass":False,
            "max_telemetry_age_ms":max_age_ms, "rejected":dict(rejects),
            "groups":statistics_by_group, "samples":samples,
            "warning":"Not true angular accuracy or synchronized LOS. Static-target declaration is not proof."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--calibration", type=Path)
    parser.add_argument("--static-target", action="store_true")
    parser.add_argument("--max-telemetry-age-ms", type=float)
    args = parser.parse_args()
    try:
        rows, digest = load_trace(args.trace)
        report = audit(rows)
        report["raw_trace_sha256"] = digest
        if args.calibration:
            if not args.static_target or args.max_telemetry_age_ms is None:
                parser.error("LOS diagnostics require --static-target and --max-telemetry-age-ms")
            raw_calibration = args.calibration.read_bytes()
            report["calibration_sha256"] = hashlib.sha256(raw_calibration).hexdigest()
            report["approximate_los"] = approximate_los(rows, json.loads(raw_calibration), args.max_telemetry_age_ms)
        text = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
        if args.output:
            with args.output.open("x", encoding="utf-8") as out:
                out.write(text + "\n")
        else:
            print(text)
        return 0 if report["trace_integrity_pass"] else 2
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(2, f"Research trace rejected: {error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
