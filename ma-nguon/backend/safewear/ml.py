"""Schema-1 IMU features and an optional, trusted-local Random Forest.

No model is shipped. A missing model produces an explicit unknown result.
joblib uses pickle: only load artifacts created locally from trusted code.
"""
from __future__ import annotations

from dataclasses import dataclass
import math
from pathlib import Path
from typing import Any, Sequence

FEATURE_SCHEMA = 1
WINDOW_MS = 5000
SAMPLE_RATE_HZ = 50
MIN_SAMPLES = 225
MAX_GAP_MS = 60
TAIL_MS = 1000
FEATURE_NAMES = (
    "mean_x_mg", "mean_y_mg", "mean_z_mg", "norm_std_mg",
    "norm_min_mg", "norm_max_mg", "norm_rms_mg", "jerk_max_gps_x10",
    "tilt_range_deg_x100", "tail_norm_std_mg",
)
FEATURE_SCALES = (1000, 1000, 1000, 1000, 1000, 1000, 1000, 10, 100, 1000)
LABELS = frozenset({"rest", "walking", "running", "fall", "other"})


@dataclass(frozen=True)
class IMUSample:
    timestamp_ms: int
    ax_g: float
    ay_g: float
    az_g: float


@dataclass(frozen=True)
class FeatureWindow:
    features: list[int] | None
    valid: bool
    reason: str
    sample_count: int
    max_gap_ms: int


def quantize(value: float) -> int:
    """C lround semantics (ties away from zero), then int16 saturation."""
    if not math.isfinite(value):
        raise ValueError("Non-finite feature")
    rounded = math.floor(value + 0.5) if value >= 0 else math.ceil(value - 0.5)
    return max(-32768, min(32767, rounded))


def _population_std(values: Sequence[float]) -> float:
    mean = math.fsum(values) / len(values)
    return math.sqrt(math.fsum((value - mean) ** 2 for value in values) / len(values))


def extract_features(samples: Sequence[IMUSample], window_start_ms: int) -> FeatureWindow:
    """Extract one half-open [start, start+5000) window; never fill missing samples.

    Training uses double precision. MCU float accumulators can differ by one
    encoded LSB; compare golden vectors with that explicitly documented tolerance.
    """
    count = len(samples)
    end = window_start_ms + WINDOW_MS

    def invalid(reason: str, gap: int = 0) -> FeatureWindow:
        return FeatureWindow(None, False, reason, count, gap)

    if not samples:
        return invalid("no_samples")
    for sample in samples:
        if isinstance(sample.timestamp_ms, bool) or not isinstance(sample.timestamp_ms, int):
            return invalid("invalid_timestamp")
        if not window_start_ms <= sample.timestamp_ms < end:
            return invalid("outside_window")
        if not all(math.isfinite(v) for v in (sample.ax_g, sample.ay_g, sample.az_g)):
            return invalid("nonfinite_sample")
    deltas = [b.timestamp_ms - a.timestamp_ms for a, b in zip(samples, samples[1:])]
    if any(delta <= 0 for delta in deltas):
        return invalid("nonmonotonic_timestamp")
    gap = max([samples[0].timestamp_ms - window_start_ms,
               end - samples[-1].timestamp_ms, *deltas])
    if gap > MAX_GAP_MS:
        return invalid("sample_gap", gap)
    if count < MIN_SAMPLES:
        return invalid("insufficient_samples", gap)
    # A 100-Hz recording must not silently become schema-1 50-Hz training data.
    if count > 275:
        return invalid("unexpected_sample_rate", gap)
    norm = [math.sqrt(s.ax_g * s.ax_g + s.ay_g * s.ay_g + s.az_g * s.az_g)
            for s in samples]
    tail = [value for sample, value in zip(samples, norm)
            if sample.timestamp_ms >= end - TAIL_MS]
    if not tail:
        return invalid("no_tail_samples", gap)
    tilt = [math.degrees(math.acos(max(-1.0, min(1.0, sample.az_g / value))))
            if value > 0 else 0.0 for sample, value in zip(samples, norm)]
    jerk = max(abs(b - a) * 1000.0 / delta
               for a, b, delta in zip(norm, norm[1:], deltas))
    values = [math.fsum(s.ax_g for s in samples) / count,
              math.fsum(s.ay_g for s in samples) / count,
              math.fsum(s.az_g for s in samples) / count,
              _population_std(norm), min(norm), max(norm),
              math.sqrt(math.fsum(value * value for value in norm) / count),
              jerk, max(tilt) - min(tilt), _population_std(tail)]
    try:
        encoded = [quantize(value * scale) for value, scale in zip(values, FEATURE_SCALES)]
    except (ValueError, OverflowError):
        return invalid("nonfinite_feature", gap)
    return FeatureWindow(encoded, True, "ok", count, gap)


def schema_metadata() -> dict[str, Any]:
    return {
        "feature_schema": FEATURE_SCHEMA,
        "feature_names": list(FEATURE_NAMES),
        "feature_scales": list(FEATURE_SCALES),
        "feature_count": len(FEATURE_NAMES),
        "input_encoding": "int16_quantized_schema1",
        "window_ms": WINDOW_MS,
        "sample_rate_hz": SAMPLE_RATE_HZ,
        "min_samples": MIN_SAMPLES,
        "max_gap_ms": MAX_GAP_MS,
        "tail_ms": TAIL_MS,
        "std_ddof": 0,
        "rounding": "half_away_from_zero_saturate_int16",
    }


def validate_metadata(metadata: Any) -> None:
    if not isinstance(metadata, dict):
        raise ValueError("Missing model metadata")
    if metadata.get("bundle_version") != 1 or metadata.get("model_type") != "RandomForestClassifier":
        raise ValueError("Unsupported model bundle")
    for key, expected in schema_metadata().items():
        if metadata.get(key) != expected:
            raise ValueError(f"Model schema mismatch: {key}")
    if metadata.get("data_kind") not in {"REAL", "SYNTHETIC"}:
        raise ValueError("Missing REAL/SYNTHETIC provenance declaration")
    digest = metadata.get("training_data_sha256", "")
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in "0123456789abcdef" for c in digest):
        raise ValueError("Missing training data SHA-256")
    labels = metadata.get("class_labels")
    if not isinstance(labels, list) or len(set(labels)) != len(labels) or len(labels) < 2:
        raise ValueError("Invalid model class labels")
    if any(not isinstance(label, str) or label not in LABELS for label in labels):
        raise ValueError("Unsupported model class label")


class ActivityClassifier:
    """Runtime annotation only; predictions never create or resolve an alarm."""

    def __init__(self, model_path: str | Path | None = None):
        self._estimator = None
        self._metadata: dict[str, Any] = {}
        self._state = "untrained"
        self._reason = "Chưa có mô hình được huấn luyện; trạng thái vận động chưa xác định."
        if model_path is None:
            return
        try:
            import joblib
            from sklearn.ensemble import RandomForestClassifier
            # Deliberately not accepted from HTTP uploads: pickle is trusted-local only.
            bundle = joblib.load(Path(model_path))
            if not isinstance(bundle, dict):
                raise ValueError("Expected a model bundle, not a bare estimator")
            metadata = bundle.get("metadata")
            validate_metadata(metadata)
            estimator = bundle.get("estimator")
            if not isinstance(estimator, RandomForestClassifier):
                raise ValueError("Expected a RandomForestClassifier")
            if getattr(estimator, "n_features_in_", None) != len(FEATURE_NAMES):
                raise ValueError("Estimator feature count mismatch")
            if list(estimator.classes_) != metadata["class_labels"]:
                raise ValueError("Estimator classes do not match metadata")
            self._estimator = estimator
            self._metadata = dict(metadata)
            self._state = "synthetic" if metadata["data_kind"] == "SYNTHETIC" else "trained_experimental"
            self._reason = ("Mô hình chỉ dùng dữ liệu giả lập; chưa xác nhận trên người lao động."
                            if self._state == "synthetic" else
                            "Mô hình thử nghiệm từ dữ liệu khai báo là thật; chưa xác nhận an toàn công nghiệp.")
        except Exception as exc:
            self._state = "invalid_model"
            self._reason = f"Không dùng được mô hình: {type(exc).__name__}: {exc}"

    def status(self) -> dict[str, Any]:
        return {"state": self._state, "trained": self._estimator is not None,
                "synthetic": self._state == "synthetic", "reason": self._reason,
                "feature_schema": FEATURE_SCHEMA, "feature_count": len(FEATURE_NAMES),
                "metadata": dict(self._metadata)}

    def predict(self, features: list[int], valid: bool) -> dict[str, Any]:
        unknown = {"label": "unknown", "source": "untrained", "confidence": None,
                   "reason": self._reason}
        if not valid:
            return {**unknown, "source": "invalid", "reason": "Cửa sổ IMU không hợp lệ hoặc thiếu mẫu."}
        if (not isinstance(features, (list, tuple)) or len(features) != len(FEATURE_NAMES)
                or any(isinstance(v, bool) or not isinstance(v, int) or not -32768 <= v <= 32767
                       for v in features)):
            return {**unknown, "source": "invalid", "reason": "Đặc trưng không đúng int16 schema 1."}
        if self._estimator is None:
            return {**unknown, "source": "model_error" if self._state == "invalid_model" else "untrained"}
        try:
            probabilities = self._estimator.predict_proba([features])[0]
            index = int(probabilities.argmax())
            confidence = float(probabilities[index])
            if not math.isfinite(confidence):
                raise ValueError("Non-finite prediction")
            return {"label": str(self._estimator.classes_[index]),
                    "source": "random_forest_synthetic" if self._state == "synthetic" else "random_forest",
                    "confidence": confidence,
                    "reason": self._reason + " Điểm mô hình chưa được hiệu chuẩn thành xác suất sự cố."}
        except Exception as exc:
            return {**unknown, "source": "model_error", "reason": f"Lỗi suy luận: {type(exc).__name__}"}
