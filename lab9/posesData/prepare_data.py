"""
prepare_dataset.py
------------------
Converts accelerometer CSV files into a windowed HDF5 dataset
for pose classification.

Expected CSV format (no header):
    s.no, timestamp, accel_x, accel_y, accel_z

Output HDF5 structure:
    /X        → float32 array of shape (N, window_size, 3)
    /y        → int32   array of shape (N,)   — integer class labels
    /labels   → string  array listing class names (index = label int)
    /metadata → group with window_size, axes, sampling info

Usage:
    python prepare_dataset.py --data_dir ./data --output dataset.h5

Folder structure expected:
    data/
      class_0/   ← folder name becomes the class label
        file1.csv
        file2.csv
      class_1/
        file3.csv
      ...

    OR flat with a class map (see --class_map option):
    data/
      walking_001.csv
      running_001.csv

See --help for all options.
"""

import os
import argparse
import numpy as np
import pandas as pd
import h5py
from pathlib import Path



def load_csv(path: Path) -> np.ndarray:
    """Load a CSV file and return accel_x, accel_y, accel_z as (N, 3) float32."""
    df = pd.read_csv(
        path,
        header=None,
        names=["sno", "timestamp", "accel_x", "accel_y", "accel_z"],
        usecols=["accel_x", "accel_y", "accel_z"],
    )
    return df.values.astype(np.float32)


def make_windows(data: np.ndarray, window_size: int, step: int) -> np.ndarray:
    """
    Slice (N, 3) data into non-overlapping windows of shape (W, window_size, 3).
    Trailing rows that don't fill a full window are dropped.
    """
    n_windows = (len(data) - window_size) // step + 1
    windows = np.stack(
        [data[i * step : i * step + window_size] for i in range(n_windows)],
        axis=0,
    )
    return windows  # shape: (n_windows, window_size, 3)


def discover_files(data_dir: Path, extension: str = ".csv"):
    """
    Walk data_dir. If it contains sub-folders, each sub-folder is a class.
    If it's flat, all files share a single class (label 0) — user should
    pass --class_map instead.o

    Returns: list of (Path, class_name) tuples.
    """
    entries = []
    subdirs = [d for d in data_dir.iterdir() if d.is_dir()]
    if subdirs:
        for subdir in sorted(subdirs):
            for csv_file in sorted(subdir.glob(f"*{extension}")):
                entries.append((csv_file, subdir.name))
    else:
        for csv_file in sorted(data_dir.glob(f"*{extension}")):
            class_name = csv_file.stem.split("_")[0]
            entries.append((csv_file, class_name))
    return entries



def build_dataset(
    data_dir: str,
    output_path: str,
    window_size: int = 200,
    step: int = None,          # defaults to window_size (non-overlapping)
    add_channel_dim: bool = False,
    extension: str = ".csv",
):
    data_dir = Path(data_dir)
    step = step or window_size  # tumbling windows

    entries = discover_files(data_dir, extension)
    if not entries:
        raise FileNotFoundError(f"No {extension} files found in {data_dir}")

    class_names = sorted(set(cls for _, cls in entries))
    class_to_idx = {name: i for i, name in enumerate(class_names)}
    print(f"Classes found ({len(class_names)}): {class_names}")

    all_windows = []
    all_labels  = []

    for csv_path, class_name in entries:
        label = class_to_idx[class_name]
        try:
            data = load_csv(csv_path)
        except Exception as e:
            print(f" Skipping {csv_path.name}: {e}")
            continue

        if len(data) < window_size:
            print(f"Skipping {csv_path.name}: only {len(data)} rows < window_size {window_size}")
            continue

        windows = make_windows(data, window_size, step)
        labels  = np.full(len(windows), label, dtype=np.int32)
        all_windows.append(windows)
        all_labels.append(labels)
        print(f"   {csv_path.name}    {len(windows)} windows  (class '{class_name}' = {label})")

    if not all_windows:
        raise ValueError("No valid data found. Check your files and window_size.")

    X = np.concatenate(all_windows, axis=0)   # (N, window_size, 3)
    y = np.concatenate(all_labels,  axis=0)   # (N,)

    if add_channel_dim:
        X = X[..., np.newaxis]                # (N, window_size, 3, 1)

    print(f"\nFinal dataset  X: {X.shape}  y: {y.shape}")
    print(f"Class distribution: { {class_names[i]: int((y==i).sum()) for i in range(len(class_names))} }")

    with h5py.File(output_path, "w") as f:
        f.create_dataset("X", data=X, dtype="float32",
                         chunks=(min(64, len(X)), window_size, X.shape[2]),
                         compression="gzip", compression_opts=4)
        f.create_dataset("y", data=y, dtype="int32")
        f.create_dataset("labels",
                         data=np.array(class_names, dtype=h5py.special_dtype(vlen=str)))

        meta = f.create_group("metadata")
        meta.attrs["window_size"]      = window_size
        meta.attrs["step"]             = step
        meta.attrs["axes"]             = ["accel_x", "accel_y", "accel_z"]
        meta.attrs["n_classes"]        = len(class_names)
        meta.attrs["channel_dim"]      = add_channel_dim
        meta.attrs["x_shape_meaning"]  = "(N_windows, timesteps, n_axes)" + (", 1)" if add_channel_dim else "")




if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Prepare windowed HDF5 dataset from accelerometer CSVs"
    )
    parser.add_argument("--data_dir",  required=True,
                        help="Root directory containing CSV files or class sub-folders")
    parser.add_argument("--output",    default="dataset.h5",
                        help="Output HDF5 file path  (default: dataset.h5)")
    parser.add_argument("--window_size", type=int, default=20,
                        help="Rows per window  (default: 20)")
    parser.add_argument("--step",      type=int, default=None,
                        help="Slide step; defaults to window_size (non-overlapping)")
    parser.add_argument("--channel_dim", action="store_true",
                        help="Add a trailing channel dim → (N, window_size, 3, 1)")
    parser.add_argument("--ext",       default=".csv",
                        help="File extension to look for  (default: .csv)")
    args = parser.parse_args()

    build_dataset(
        data_dir       = args.data_dir,
        output_path    = args.output,
        window_size    = args.window_size,  # 20 samples per window → 200 windows per 4000-row file
        step           = args.step,
        add_channel_dim= args.channel_dim,
        extension      = args.ext,
    )