import pandas as pd
import numpy as np
import matplotlib.pyplot as plt
import glob

import os

# find CSVs in the same folder as this script, not wherever you run it from
script_dir = os.path.dirname(os.path.abspath(__file__))
csv_files = sorted(glob.glob(os.path.join(script_dir, "pose*.csv")))

print(f"Found {len(csv_files)} files: {csv_files}")  # debug line

poses = {}
for f in csv_files:
    df = pd.read_csv(f, names=["row","timestamp_ms","accel_x","accel_y","accel_z"])
    poses[f] = df

def extract_features(df, window=20):
    records = []
    for axis in ["accel_x", "accel_y", "accel_z"]:
        signal = df[axis].values
        global_mean = np.mean(signal)
        for start in range(0, len(signal) - window + 1, window):
            w = signal[start:start + window]
            mu = np.mean(w)
            var = np.var(w)
            # remove mean then count sign changes
            centered = w - global_mean
            zcr = np.sum(np.diff(np.sign(centered)) != 0)
            records.append({
                "axis": axis,
                "window": start // window,
                "mean": mu,
                "variance": var,
                "zcr": zcr
            })
    return pd.DataFrame(records)

fig, axes = plt.subplots(
    nrows=len(poses),
    ncols=3,
    figsize=(14, 4 * len(poses)),
    sharex=False
)

# make axes 2D even if only one pose
if len(poses) == 1:
    axes = [axes]

feature_cols = ["mean", "variance", "zcr"]
feature_labels = ["Mean μ", "Variance σ²", "Zero-Crossing Rate"]
axis_colors = {"accel_x": "steelblue", "accel_y": "seagreen", "accel_z": "tomato"}

for row_idx, (fname, df) in enumerate(poses.items()):
    feats = extract_features(df, window=20)
    
    for col_idx, (feat, label) in enumerate(zip(feature_cols, feature_labels)):
        ax = axes[row_idx][col_idx]
        
        for axis_name, color in axis_colors.items():
            subset = feats[feats["axis"] == axis_name]
            ax.plot(subset["window"], subset[feat],
                    label=axis_name, color=color, marker="o", markersize=3)
        
        ax.set_title(f"{os.path.basename(fname)}  —  {label}", fontsize=10)
        ax.set_xlabel("Window index")
        ax.set_ylabel(label)
        ax.legend(fontsize=8)
        ax.grid(True, alpha=0.3)

plt.tight_layout()
plt.savefig("pose_features.svg", dpi=150)
plt.show()