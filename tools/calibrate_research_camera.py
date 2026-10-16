"""Calibrate saved chessboard images without opening a camera or network.

Input images must already match the tracking orientation/crop/resolution.
Outputs are candidates, never automatically approved for LOS diagnostics.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path
import cv2
import numpy as np


def fit_points(object_points, image_points, image_size):
    if len(object_points) != len(image_points) or len(object_points) < 12:
        raise ValueError("At least 12 valid views are required")
    heldout = [i for i in range(len(image_points)) if i % 5 == 0]
    training = [i for i in range(len(image_points)) if i not in heldout]
    rms, matrix, distortion, rotations, translations = cv2.calibrateCamera(
        [object_points[i] for i in training], [image_points[i] for i in training], image_size, None, None)
    if not math.isfinite(rms) or not np.isfinite(matrix).all() or not np.isfinite(distortion).all():
        raise ValueError("Calibration returned nonfinite parameters")
    residuals = []
    for i in heldout:
        ok, rotation, translation = cv2.solvePnP(object_points[i], image_points[i], matrix, distortion)
        if not ok:
            raise ValueError(f"Held-out pose estimation failed for view {i}")
        projected, _ = cv2.projectPoints(object_points[i], rotation, translation, matrix, distortion)
        residuals.append(float(np.sqrt(np.mean(np.sum((projected-image_points[i]).reshape(-1,2)**2,axis=1)))))
    return {"camera_matrix":matrix.tolist(), "distortion_coefficients":distortion.reshape(-1).tolist(),
            "training_rms_px":float(rms), "training_indices":training, "heldout_indices":heldout,
            "heldout_pose_fitted_rms_px":residuals,
            "heldout_note":"Intrinsics held out; each held-out pose fits its own corners. Not external angular accuracy."}


def calibrate(paths, columns, rows, square_mm, rotation):
    if columns < 3 or rows < 3 or not math.isfinite(square_mm) or square_mm <= 0:
        raise ValueError("Positive square_mm and at least 3x3 inner corners required")
    object_template = np.zeros((columns*rows,3), np.float32)
    object_template[:,:2] = np.mgrid[0:columns,0:rows].T.reshape(-1,2) * square_mm
    hashes, objects, points, sources, rejected, size = set(), [], [], [], [], None
    for path in sorted(paths):
        raw = path.read_bytes()
        digest = hashlib.sha256(raw).hexdigest()
        if digest in hashes:
            raise ValueError(f"Duplicate image bytes: {path.name}")
        hashes.add(digest)
        gray = cv2.imdecode(np.frombuffer(raw,np.uint8), cv2.IMREAD_GRAYSCALE)
        if gray is None:
            raise ValueError(f"Unreadable image: {path.name}")
        current = (gray.shape[1], gray.shape[0])
        if size is None:
            size = current
        if current != size:
            raise ValueError("Mixed image sizes are not allowed")
        ok, corners = cv2.findChessboardCornersSB(gray, (columns, rows))
        source = {"file_name":path.name, "sha256":digest}
        if not ok:
            rejected.append(source)
            continue
        sources.append(source)
        objects.append(object_template.copy())
        points.append(corners.astype(np.float32))
    result = fit_points(objects, points, size)
    result.update({"schema_version":1, "image_size":list(size), "rotation":rotation,
        "inner_corners":[columns,rows], "square_mm":square_mm,
        "source_images":sources, "rejected_images":rejected, "opencv_version":cv2.__version__,
        "reviewed_for_diagnostics":False, "horizontal_sign":None, "encoder_zero_deg":None,
        "calibration_status":"candidate_requires_coverage_and_geometry_review",
        "warning":"No hardware movement, automatic acceptance or synchronized LOS claim. Fixed image split is not scene-independent validation."})
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image_directory", type=Path)
    parser.add_argument("--columns", type=int, required=True, help="Inner corner columns, not squares")
    parser.add_argument("--rows", type=int, required=True, help="Inner corner rows, not squares")
    parser.add_argument("--square-mm", type=float, required=True)
    parser.add_argument("--rotation", type=int, choices=range(6), required=True,
                        help="Existing tracking orientation; script does not transform images")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        paths = [p for p in args.image_directory.iterdir() if p.suffix.lower() in (".png",".jpg",".jpeg",".bmp",".tif",".tiff")]
        if len(paths) < 12:
            raise ValueError("At least 12 images with varied positions/tilts required; 20-30 recommended")
        result = calibrate(paths,args.columns,args.rows,args.square_mm,args.rotation)
        with args.output.open("x",encoding="utf-8") as out:
            json.dump(result,out,ensure_ascii=False,indent=2,allow_nan=False)
            out.write("\n")
        print(json.dumps({"output":str(args.output),"views":len(result["source_images"]),
                          "candidate_only":True},ensure_ascii=False))
    except (ValueError,OSError,cv2.error) as error:
        parser.exit(2,f"Calibration rejected: {error}\n")


if __name__ == "__main__":
    main()
