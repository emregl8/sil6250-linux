import argparse
import ctypes
import json
from pathlib import Path
import statistics
import struct
import time


IMAGE_SIZE = 5120
FEATURE_CAPACITY = 106412
Bytes = ctypes.POINTER(ctypes.c_uint8)


def load(path):
    lib = ctypes.CDLL(str(Path(path).resolve()))
    lib.sil6250_frame_extract.argtypes = [Bytes, ctypes.c_size_t, Bytes, ctypes.c_size_t]
    lib.sil6250_frame_extract.restype = ctypes.c_ssize_t
    lib.sil6250_frame_score.argtypes = [Bytes, ctypes.c_size_t, Bytes, ctypes.c_size_t]
    lib.sil6250_frame_score.restype = ctypes.c_int32
    if hasattr(lib, "sil6250_frame_matches"):
        lib.sil6250_frame_matches.argtypes = [Bytes, ctypes.c_size_t, Bytes, ctypes.c_size_t, ctypes.c_int32]
        lib.sil6250_frame_matches.restype = ctypes.c_int32
    if hasattr(lib, "sil6250_gallery_new"):
        lib.sil6250_gallery_new.argtypes = [Bytes, ctypes.c_size_t]
        lib.sil6250_gallery_new.restype = ctypes.c_void_p
        lib.sil6250_probe_new.argtypes = [Bytes, ctypes.c_size_t]
        lib.sil6250_probe_new.restype = ctypes.c_void_p
        lib.sil6250_probe_matches.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32]
        lib.sil6250_probe_matches.restype = ctypes.c_int32
        lib.sil6250_probe_free.argtypes = [ctypes.c_void_p]
        lib.sil6250_probe_free.restype = None
        lib.sil6250_gallery_free.argtypes = [ctypes.c_void_p]
        lib.sil6250_gallery_free.restype = None
    return lib


def image(seed):
    state = seed
    result = (ctypes.c_uint8 * IMAGE_SIZE)()
    for i in range(IMAGE_SIZE):
        state = (state ^ (state << 13)) & 0xFFFFFFFF
        state ^= state >> 17
        state = (state ^ (state << 5)) & 0xFFFFFFFF
        result[i] = state & 0xFF
    return result


def extract(lib, raw):
    output = (ctypes.c_uint8 * FEATURE_CAPACITY)()
    length = lib.sil6250_frame_extract(raw, IMAGE_SIZE, output, FEATURE_CAPACITY)
    if length <= 0:
        raise RuntimeError("Synthetic feature extraction failed")
    data = bytes(output[:length])
    return struct.pack("<I", length) + data


def gallery(entries):
    data = b"".join(entries)
    return (ctypes.c_uint8 * len(data)).from_buffer_copy(data)


def measure(function, loops):
    function()
    timings = []
    for _ in range(5):
        start = time.perf_counter_ns()
        for _ in range(loops):
            function()
        timings.append((time.perf_counter_ns() - start) / loops / 1_000_000)
    return statistics.median(timings)


def prepared_decision(lib, raw, gallery):
    probe = lib.sil6250_probe_new(raw, IMAGE_SIZE)
    if not probe:
        raise RuntimeError("Probe preparation failed")
    try:
        return lib.sil6250_probe_matches(probe, gallery, 5)
    finally:
        lib.sil6250_probe_free(probe)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--baseline", required=True)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--loops", type=int, default=10)
    args = parser.parse_args()
    if args.loops <= 0:
        parser.error("--loops must be positive")
    baseline, candidate = load(args.baseline), load(args.candidate)
    images = [image(seed) for seed in range(1, 9)]
    entries = [extract(baseline, raw) for raw in images]
    for raw, expected in zip(images, entries):
        if extract(candidate, raw) != expected:
            raise RuntimeError("Feature encoding changed")
    checked = 0
    results = []
    for i, raw in enumerate(images):
        others = entries[:i] + entries[i + 1:]
        for count in [1, 5, 15]:
            unrelated = [others[j % len(others)] for j in range(count)]
            for label, refs in [
                ("self-first", [entries[i]] + unrelated[1:]),
                ("self-last", unrelated[:-1] + [entries[i]]),
                ("other-images", unrelated),
            ]:
                data = gallery(refs)
                old_score = baseline.sil6250_frame_score(raw, IMAGE_SIZE, data, len(data))
                score = candidate.sil6250_frame_score(raw, IMAGE_SIZE, data, len(data))
                matched = candidate.sil6250_frame_matches(raw, IMAGE_SIZE, data, len(data), 5)
                if old_score < 0 or score != old_score or matched != int(old_score >= 5):
                    raise RuntimeError(f"Matching behavior changed: {old_score}, {score}, {matched}")
                prepared = None
                try:
                    if hasattr(candidate, "sil6250_gallery_new"):
                        prepared = candidate.sil6250_gallery_new(data, len(data))
                        if not prepared or prepared_decision(candidate, raw, prepared) != matched:
                            raise RuntimeError("Prepared matching behavior changed")
                    checked += 1
                    if i != 0:
                        continue
                    functions = {
                        "baseline_score_ms": lambda: baseline.sil6250_frame_score(raw, IMAGE_SIZE, data, len(data)),
                        "candidate_score_ms": lambda: candidate.sil6250_frame_score(raw, IMAGE_SIZE, data, len(data)),
                        "candidate_decision_ms": lambda: candidate.sil6250_frame_matches(raw, IMAGE_SIZE, data, len(data), 5),
                    }
                    if prepared:
                        functions["prepared_decision_ms"] = lambda: prepared_decision(candidate, raw, prepared)
                    results.append({
                        "case": label,
                        "reference_count": count,
                        "accepted": old_score >= 5,
                        **{name: measure(fn, args.loops) for name, fn in functions.items()},
                    })
                finally:
                    if prepared:
                        candidate.sil6250_gallery_free(prepared)

    identification = []
    if hasattr(candidate, "sil6250_gallery_new"):
        raw = images[0]
        for count in [2, 5, 10]:
            groups = [gallery([entries[1 + i % 7]] * 15) for i in range(count - 1)]
            groups.append(gallery([entries[0]] * 15))
            handles = [candidate.sil6250_gallery_new(data, len(data)) for data in groups]
            if not all(handles):
                raise RuntimeError("Gallery preparation failed")

            def repeated():
                for index, data in enumerate(groups):
                    if baseline.sil6250_frame_matches(raw, IMAGE_SIZE, data, len(data), 5) == 1:
                        return index
                return -1

            def prepared():
                probe = candidate.sil6250_probe_new(raw, IMAGE_SIZE)
                if not probe:
                    raise RuntimeError("Probe preparation failed")
                try:
                    for index, handle in enumerate(handles):
                        if candidate.sil6250_probe_matches(probe, handle, 5) == 1:
                            return index
                    return -1
                finally:
                    candidate.sil6250_probe_free(probe)

            try:
                if repeated() != count - 1 or prepared() != count - 1:
                    raise RuntimeError("Identification behavior changed")
                identification.append({"gallery_count": count,
                    "baseline_ms": measure(repeated, args.loops),
                    "prepared_ms": measure(prepared, args.loops)})
            finally:
                for handle in handles:
                    candidate.sil6250_gallery_free(handle)
    print(json.dumps({"input": "synthetic images only", "checked_cases": checked,
                      "results": results, "identification": identification}, indent=2))


if __name__ == "__main__":
    main()
