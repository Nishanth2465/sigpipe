"""Make the two README graphs: filter result and thread speedup.

usage: python3 scripts/plot.py   (run `make plot` and `make bench` first)
"""
import csv
import os

import matplotlib.pyplot as plt

os.makedirs("results", exist_ok=True)

# 1. noisy vs filtered signal
if os.path.exists("results/signal.csv"):
    with open("results/signal.csv") as f:
        rows = list(csv.DictReader(f))
    t = [int(r["i"]) / 48.0 for r in rows]  # sample index -> milliseconds at 48 kHz
    plt.figure(figsize=(9, 4))
    plt.plot(t, [float(r["noisy"]) for r in rows], lw=0.5, alpha=0.5, label="noisy input")
    plt.plot(t, [float(r["filtered"]) for r in rows], lw=2, label="FIR filtered")
    plt.plot(t, [float(r["clean"]) for r in rows], "k--", lw=1, label="true signal")
    plt.xlabel("time (ms)")
    plt.ylabel("amplitude")
    plt.title("Low-pass FIR filter removing hum and noise")
    plt.legend()
    plt.tight_layout()
    plt.savefig("results/signal.png", dpi=150)
    print("saved results/signal.png")

# 2. speedup vs thread count
if os.path.exists("results/bench.csv"):
    with open("results/bench.csv") as f:
        rows = list(csv.DictReader(f))
    threads = [int(r["threads"]) for r in rows]
    times = [float(r["time_s"]) for r in rows]
    speedup = [times[0] / x for x in times]
    plt.figure(figsize=(6, 4))
    plt.plot(threads, speedup, "o-", label="measured")
    plt.plot(threads, threads, "k--", lw=1, label="ideal (linear)")
    plt.xticks(threads)
    plt.xlabel("worker threads")
    plt.ylabel("speedup vs 1 thread")
    plt.title(f"Throughput scaling ({os.cpu_count()} CPU cores)")
    plt.legend()
    plt.tight_layout()
    plt.savefig("results/speedup.png", dpi=150)
    print("saved results/speedup.png")
