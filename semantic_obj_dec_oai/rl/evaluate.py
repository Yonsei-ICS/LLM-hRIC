#!/usr/bin/env python3
"""
Evaluate a trained RL model and compare against baselines.
Generates a summary table and optionally saves per-episode CSV.

Usage:
  python3 evaluate.py --model checkpoints/best_model.zip --episodes 200
  python3 evaluate.py --model checkpoints/best_model.zip --save-csv results.csv
"""

from __future__ import annotations

import argparse
import csv
import os
import sys

import gymnasium as gym
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prb_env  # noqa: F401

from stable_baselines3 import PPO, SAC


def run_episodes(
    env: gym.Env,
    policy_fn,
    n_episodes: int,
) -> list[dict]:
    """Run n_episodes and collect per-episode metrics."""
    results = []
    for ep in range(n_episodes):
        obs, _ = env.reset(seed=ep)
        ep_reward = 0.0
        done = False
        info: dict = {}
        steps = 0
        while not done:
            action = policy_fn(obs)
            obs, reward, terminated, truncated, info = env.step(action)
            ep_reward += reward
            steps += 1
            done = terminated or truncated
        results.append({
            "episode": ep,
            "reward": ep_reward,
            "steps": steps,
            "success_rate": info.get("success_rate", 0),
            "avg_delay": info.get("avg_delay", 0),
            "jain_fairness": info.get("jain_fairness", 0),
            "waste_ratio": info.get("waste_ratio", 0),
        })
    return results


def summarise(name: str, results: list[dict]) -> dict:
    rewards = [r["reward"] for r in results]
    return {
        "strategy": name,
        "mean_reward": np.mean(rewards),
        "std_reward": np.std(rewards),
        "mean_success": np.mean([r["success_rate"] for r in results]),
        "mean_delay": np.mean([r["avg_delay"] for r in results]),
        "mean_fairness": np.mean([r["jain_fairness"] for r in results]),
        "mean_waste": np.mean([r["waste_ratio"] for r in results]),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--model", required=True, help="Path to trained .zip model")
    ap.add_argument("--episodes", type=int, default=100)
    ap.add_argument("--num-ues", type=int, default=2)
    ap.add_argument("--total-prb", type=int, default=106)
    ap.add_argument("--save-csv", default=None)
    args = ap.parse_args()

    env_kwargs = dict(num_ues=args.num_ues, total_prb=args.total_prb)
    env = gym.make("SemanticPRB-v0", **env_kwargs)

    # Load model
    if "sac" in os.path.basename(args.model).lower():
        model = SAC.load(args.model)
    else:
        model = PPO.load(args.model)

    # Define policies
    def rl_policy(obs):
        action, _ = model.predict(obs, deterministic=True)
        return action

    def equal_policy(obs):
        return np.ones(args.num_ues, dtype=np.float32) / args.num_ues

    def heuristic_policy(obs):
        n = args.num_ues
        per_ue = len(obs) // n
        w = np.zeros(n, dtype=np.float32)
        for i in range(n):
            b = i * per_ue
            if obs[b + 7] > 0.5:
                w[i] = 0.60
            elif obs[b + 6] > 0.5:
                w[i] = 0.30
            elif obs[b + 8] > 0.5:
                w[i] = 0.15
            else:
                w[i] = 0.10
        s = w.sum()
        return w / s if s > 0 else np.ones(n) / n

    strategies = {
        "RL": rl_policy,
        "Heuristic": heuristic_policy,
        "Equal": equal_policy,
    }

    all_results = {}
    print(f"\nEvaluating over {args.episodes} episodes ({args.num_ues} UEs, {args.total_prb} PRBs)\n")

    for name, policy_fn in strategies.items():
        results = run_episodes(env, policy_fn, args.episodes)
        all_results[name] = results
        s = summarise(name, results)
        print(f"  {name:>12s}: reward={s['mean_reward']:+7.2f}±{s['std_reward']:5.2f}"
              f"  success={s['mean_success']:.3f}"
              f"  delay={s['mean_delay']:.3f}"
              f"  fairness={s['mean_fairness']:.3f}"
              f"  waste={s['mean_waste']:.4f}")

    rl_s = summarise("RL", all_results["RL"])
    he_s = summarise("Heuristic", all_results["Heuristic"])
    improve = ((rl_s["mean_reward"] - he_s["mean_reward"]) / abs(he_s["mean_reward"] + 1e-9)) * 100
    print(f"\n  RL vs Heuristic: {improve:+.1f}% reward improvement")

    if args.save_csv:
        with open(args.save_csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["strategy", "episode", "reward",
                                                     "success_rate", "avg_delay",
                                                     "jain_fairness", "waste_ratio"])
            writer.writeheader()
            for name, results in all_results.items():
                for r in results:
                    writer.writerow({"strategy": name, **r})
        print(f"\n  Results saved to {args.save_csv}")

    env.close()


if __name__ == "__main__":
    main()
