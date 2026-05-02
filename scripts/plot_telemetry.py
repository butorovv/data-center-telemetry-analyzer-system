#!/usr/bin/env python3
"""Draw a telemetry time series and highlight anomaly points."""

import argparse
import csv


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--title", default="Telemetry")
    args = parser.parse_args()

    timestamps = []
    values = []
    anomaly_x = []
    anomaly_y = []

    with open(args.input, newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for index, row in enumerate(reader):
            try:
                value = float(row["value"])
            except (KeyError, ValueError):
                continue
            timestamps.append(row.get("timestamp") or str(index))
            values.append(value)
            if row.get("anomaly") == "1":
                anomaly_x.append(len(values) - 1)
                anomaly_y.append(value)

    import matplotlib.pyplot as plt

    x = list(range(len(values)))
    plt.figure(figsize=(12, 5))
    plt.plot(x, values, linewidth=1.0, color="#1f77b4", label="value")
    if anomaly_x:
        plt.scatter(anomaly_x, anomaly_y, s=28, color="#d62728", label="anomaly", zorder=3)
    if timestamps:
        step = max(1, len(timestamps) // 8)
        plt.xticks(x[::step], timestamps[::step], rotation=30, ha="right")
    plt.title(args.title)
    plt.xlabel("time")
    plt.ylabel("metric")
    plt.grid(True, alpha=0.25)
    plt.legend()
    plt.tight_layout()
    plt.savefig(args.output, dpi=160)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
