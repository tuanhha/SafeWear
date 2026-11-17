"""Experimental BP inference. Only trusted local joblib artifacts may be loaded."""
from __future__ import annotations

import json
import math
import struct
from pathlib import Path

FEATURE_SCHEMA = 1
WINDOW_MS = 10000
FEATURE_NAMES = (
    "mean_ir_div10", "mean_red_div10", "ir_cv_x10000", "red_cv_x10000",
    "correlation_x10000", "interval_mean_ms", "interval_std_ms",
    "rise_mean_ms", "decay_mean_ms", "half_width_mean_ms",
    "rise_slope_x100", "decay_slope_x100",
)


def extract_features(samples, window_start_ms=0, lost=0):
    """Mirror node schema 1. Samples are (timestamp_ms, red, ir); no interpolation."""
    n = len(samples)
    if not 225 <= n <= 250 or lost:
        return None
    t, red, ir = zip(*samples)
    if (t[0] < window_start_ms or t[-1] >= window_start_ms + WINDOW_MS
            or t[0] - window_start_ms > 80 or window_start_ms + WINDOW_MS - t[-1] > 80
            or any(b <= a or b-a > 80 for a, b in zip(t, t[1:]))
            or any(not 1000 <= v < 250000 for v in (*red, *ir))):
        return None
    mr, mi = sum(red)/n, sum(ir)/n
    rr = sum((v-mr)**2 for v in red)
    ii = sum((v-mi)**2 for v in ir)
    ri = sum((r-mr)*(v-mi) for r, v in zip(red, ir))
    if rr <= 0 or ii <= 0:
        return None
    ar, ai, corr = math.sqrt(rr/n)/mr, math.sqrt(ii/n)/mi, ri/math.sqrt(rr*ii)
    if not (.001 <= ar <= .2 and .001 <= ai <= .2 and corr >= .8):
        return None
    beats, previous = [], None
    for i in range(1, n-1):
        if ir[i] <= ir[i-1] and ir[i] < ir[i+1]:
            if previous is not None:
                a, b = previous, i
                p = max(range(a, b+1), key=lambda j: ir[j])
                duration = t[b]-t[a]
                base = (ir[a]+ir[b])/2
                amp = ir[p]-base
                if 320 <= duration <= 2000 and a < p < b and amp >= .5*math.sqrt(ii/n):
                    slopes = [(ir[j]-ir[j-1])*1000/(t[j]-t[j-1])/amp for j in range(a+1, b+1)]
                    width = sum(t[j]-t[j-1] for j in range(a+1,b+1) if (ir[j]+ir[j-1])/2 >= base+amp/2)
                    beats.append((duration,t[p]-t[a],t[b]-t[p],width,max(0,max(slopes)),max(0,-min(slopes))))
            previous = i
    if len(beats) < 3:
        return None
    means = [sum(b[j] for b in beats)/len(beats) for j in range(6)]
    std = math.sqrt(max(0, sum(b[0]**2 for b in beats)/len(beats)-means[0]**2))
    values = [mi/10,mr/10,ai*10000,ar*10000,corr*10000,means[0],std,*means[1:4],means[4]*100,means[5]*100]
    def quantize(v):
        v = struct.unpack("f", struct.pack("f", v))[0]
        return max(-32768,min(32767, math.floor(v+.5) if v >= 0 else math.ceil(v-.5)))
    return [quantize(v) for v in values]


def valid_features(features):
    return (isinstance(features, (list, tuple)) and len(features) == 12
            and all(isinstance(x, int) and not isinstance(x, bool)
                    and -32768 <= x <= 32767 for x in features))


class BloodPressureEstimator:
    def __init__(self, path=None, calibration_path=None):
        self.artifact = None
        self.calibration = {}
        self._state = "model_missing"
        try:
            if calibration_path:
                self.calibration = json.loads(Path(calibration_path).read_text(encoding="utf-8"))
                if not isinstance(self.calibration, dict):
                    raise ValueError("Calibration must map node IDs to person IDs")
            if path:
                import joblib
                from sklearn.ensemble import RandomForestRegressor
                from sklearn.utils.validation import check_is_fitted
                artifact = joblib.load(path)  # Trusted administrator-owned local file only.
                meta = artifact["metadata"]
                if (meta["schema"] != FEATURE_SCHEMA or meta["feature_names"] != list(FEATURE_NAMES)
                        or meta["window_ms"] != WINDOW_MS or meta["data_kind"] != "REAL"
                        or meta["position"] != "wrist" or meta["accepted"] is not True
                        or not meta["calibrated_people"]):
                    raise ValueError("Incompatible or unvalidated model")
                if not all(callable(getattr(artifact[k], "predict", None)) for k in ("systolic", "diastolic")):
                    raise ValueError("Missing regressors")
                for name in ("systolic", "diastolic"):
                    model = artifact[name]
                    if not isinstance(model, RandomForestRegressor) or model.n_features_in_ != 12:
                        raise ValueError("Expected fitted 12-feature RF")
                    check_is_fitted(model)
                    metrics = meta["metrics"][name]
                    if not all(math.isfinite(metrics[k]) for k in ("mae", "bias", "error_sd", "baseline_mae")) or not 0 <= metrics["mae"] < metrics["baseline_mae"]:
                        raise ValueError("Baseline acceptance failed")
                if not all(isinstance(p,str) and p for p in meta["calibrated_people"]):
                    raise ValueError("Invalid calibration coverage")
                if len(meta["source_sha256"]) != 64 or any(c not in "0123456789abcdef" for c in meta["source_sha256"]):
                    raise ValueError("Missing provenance digest")
                if not valid_features(meta["feature_min"]) or not valid_features(meta["feature_max"]) or any(a>b for a,b in zip(meta["feature_min"],meta["feature_max"])):
                    raise ValueError("Missing training domain")
                self.artifact = artifact
                self._state = "ready"
        except Exception:
            self._state = "model_invalid"

    def status(self):
        return {"status": self._state, "experimental": True, "schema": FEATURE_SCHEMA}

    def predict(self, features, *, valid, node_id, simulated=False):
        result = {"status": self._state, "systolic_mmhg": None,
                  "diastolic_mmhg": None, "experimental": True}
        if simulated:
            result["status"] = "simulated"
        elif not valid or not valid_features(features):
            result["status"] = "signal_invalid"
        elif self.artifact:
            person = self.calibration.get(str(node_id))
            if person not in self.artifact["metadata"]["calibrated_people"]:
                result["status"] = "uncalibrated"
            else:
                try:
                    meta = self.artifact["metadata"]
                    if "feature_min" in meta and any(not low <= value <= high for low,value,high in zip(meta["feature_min"],features,meta["feature_max"])):
                        result["status"] = "out_of_domain"
                        return result
                    sbp = float(self.artifact["systolic"].predict([features])[0])
                    dbp = float(self.artifact["diastolic"].predict([features])[0])
                    if not (math.isfinite(sbp) and math.isfinite(dbp) and 40 <= dbp < sbp <= 260):
                        raise ValueError("Invalid prediction")
                    result.update(status="ready", systolic_mmhg=round(sbp, 1), diastolic_mmhg=round(dbp, 1))
                except Exception:
                    result["status"] = "prediction_invalid"
        return result
