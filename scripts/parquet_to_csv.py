#!/usr/bin/env python3
"""Fallback Parquet to CSV converter used only when Apache Arrow C++ is unavailable."""

import argparse


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True)
    parser.add_argument("--output", required=True)
    args = parser.parse_args()

    import pandas as pd

    frame = pd.read_parquet(args.input)
    frame.to_csv(args.output, index=False)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
