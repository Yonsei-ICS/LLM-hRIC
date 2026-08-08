#!/usr/bin/env python3
"""
Train a PPO or SAC agent for semantic-detection PRB scheduling.

Usage
-----
  # PPO (default, stable & easy to tune)
  python3 train.py --algo ppo --total-timesteps 500000

  # SAC (better sample efficiency, continuous-action native)
  python3 train.py --algo sac --total-timesteps 300000

  # Customise environment
  python3 train.py --num-ues 3 --total-prb 106 --detection-prob 0.2

  # Resume from checkpoint
  python3 train.py --algo ppo --resume checkpoints/ppo_semantic_prb_best.zip

The trained model is saved to  checkpoints/<algo>_semantic_prb_best.zip
Tensorboard logs go to        tb_logs/
"""

from __future__ import annotations

import argparse
import os
import sys

import gymnasium as gym
import numpy as np

# Make sure the env module is importable
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import prb_env  # noqa: F401  (triggers gym.register)

from stable_baselines3 import PPO, SAC
from stable_baselines3.common.callbacks import (
    BaseCallback,
    CheckpointCallback,
    EvalCallback,
)
from stable_baselines3.common.env_util import make_vec_env
from stable_baselines3.common.monitor import Monitor
from stable_baselines3.common.vec_env import SubprocVecEnv, VecNormalize


# ── Custom metrics callback ──────────────────────────────────
class MetricsCallback(BaseCallback):
    """Log domain-specific metrics to Tensorboard."""

    def __init__(self, verbose: int = 0):
        super().__init__(verbose)
        self._episode_rewards: list[float] = []

    def _on_step(self) -> bool:
        infos = self.locals.get("infos", [])
        for info in infos:
            if "success_rate" in info:
                self.logger.record("env/success_rate", info["success_rate"])
                self.logger.record("env/avg_delay", info["avg_delay"])
                self.logger.record("env/jain_fairness", info["jain_fairness"])
                self.logger.record("env/waste_ratio", info["waste_ratio"])
        return True


# ── Baseline comparisons ──────────────────────────────────────
def evaluate_baseline(env_id: str, env_kwargs: dict, strategy: str, n_episodes: int = 50) -> dict:
    """Evaluate a heuristic baseline for comparison."""
    env = gym.make(env_id, **env_kwargs)
    all_rewards = []
    all_success = []
    all_fairness = []

    for _ in range(n_episodes):
        obs, _ = env.reset()
        ep_reward = 0.0
        done = False
        last_info: dict = {}
        while not done:
            n = env_kwargs.get("num_ues", 2)
            if strategy == "equal":
                action = np.ones(n, dtype=np.float32) / n
            elif strategy == "random":
                action = np.random.dirichlet(np.ones(n)).astype(np.float32)
            elif strategy == "heuristic":
                action = _heuristic_action(obs, n)
            else:
                action = np.ones(n, dtype=np.float32) / n

            obs, reward, terminated, truncated, info = env.step(action)
            ep_reward += reward
            last_info = info
            done = terminated or truncated

        all_rewards.append(ep_reward)
        all_success.append(last_info.get("success_rate", 0))
        all_fairness.append(last_info.get("jain_fairness", 0))
    env.close()

    return {
        "strategy": strategy,
        "mean_reward": float(np.mean(all_rewards)),
        "std_reward": float(np.std(all_rewards)),
        "mean_success": float(np.mean(all_success)),
        "mean_fairness": float(np.mean(all_fairness)),
    }


def _heuristic_action(obs: np.ndarray, n: int) -> np.ndarray:
    """Static 3-level heuristic matching the rule-based xApp."""
    per_ue = len(obs) // n
    weights = np.zeros(n, dtype=np.float32)
    for i in range(n):
        base = i * per_ue
        # one-hot status is at indices 5-8: idle(5), verifying(6), highres(7), verified(8)
        if obs[base + 7] > 0.5:  # requesting_highres
            weights[i] = 0.60
        elif obs[base + 6] > 0.5:  # verifying
            weights[i] = 0.30
        elif obs[base + 8] > 0.5:  # verified
            weights[i] = 0.15
        else:  # idle
            weights[i] = 0.10
    s = weights.sum()
    if s > 0:
        weights /= s
    else:
        weights[:] = 1.0 / n
    return weights


# ── Main ─────────────────────────────────────────────────────
def main():
    ap = argparse.ArgumentParser(description="Train RL agent for PRB scheduling")
    ap.add_argument("--algo", choices=["ppo", "sac"], default="ppo")
    ap.add_argument("--total-timesteps", type=int, default=500_000)
    ap.add_argument("--num-ues", type=int, default=2)
    ap.add_argument("--total-prb", type=int, default=106)
    ap.add_argument("--reserve-prb", type=int, default=6)
    ap.add_argument("--detection-prob", type=float, default=0.15)
    ap.add_argument("--highres-deadline-ms", type=int, default=500)
    ap.add_argument("--epoch-ms", type=int, default=100)
    ap.add_argument("--max-steps", type=int, default=1000)
    ap.add_argument("--n-envs", type=int, default=4,
                    help="Parallel training envs (PPO only)")
    ap.add_argument("--eval-episodes", type=int, default=20)
    ap.add_argument("--checkpoint-dir", default="checkpoints")
    ap.add_argument("--tb-log", default="tb_logs")
    ap.add_argument("--resume", default=None, help="Path to .zip model to resume")
    ap.add_argument("--eval-baselines", action="store_true",
                    help="Evaluate heuristic baselines before training")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    os.makedirs(args.checkpoint_dir, exist_ok=True)
    os.makedirs(args.tb_log, exist_ok=True)

    env_kwargs = dict(
        num_ues=args.num_ues,
        total_prb=args.total_prb,
        reserve_prb=args.reserve_prb,
        epoch_ms=args.epoch_ms,
        max_steps=args.max_steps,
        detection_prob=args.detection_prob,
        highres_deadline_ms=args.highres_deadline_ms,
    )

    # ── Baseline evaluation ──
    if args.eval_baselines:
        print("=" * 60)
        print("Baseline evaluation")
        print("=" * 60)
        for strategy in ["equal", "random", "heuristic"]:
            result = evaluate_baseline("SemanticPRB-v0", env_kwargs, strategy)
            print(f"  {strategy:>10s}: reward={result['mean_reward']:+.2f}±{result['std_reward']:.2f}"
                  f"  success={result['mean_success']:.3f}  fairness={result['mean_fairness']:.3f}")
        print()

    # ── Create vectorised training env ──
    def make_env(rank: int):
        def _init():
            e = gym.make("SemanticPRB-v0", **env_kwargs)
            e = Monitor(e)
            return e
        return _init

    if args.algo == "ppo":
        train_env = SubprocVecEnv([make_env(i) for i in range(args.n_envs)])
    else:
        train_env = Monitor(gym.make("SemanticPRB-v0", **env_kwargs))

    eval_env = Monitor(gym.make("SemanticPRB-v0", **env_kwargs))

    # ── Build or load model ──
    model_name = f"{args.algo}_semantic_prb"

    if args.resume:
        print(f"Resuming from {args.resume}")
        cls = PPO if args.algo == "ppo" else SAC
        model = cls.load(args.resume, env=train_env)
    else:
        if args.algo == "ppo":
            model = PPO(
                "MlpPolicy",
                train_env,
                learning_rate=3e-4,
                n_steps=2048,
                batch_size=64,
                n_epochs=10,
                gamma=0.99,
                gae_lambda=0.95,
                clip_range=0.2,
                ent_coef=0.01,
                vf_coef=0.5,
                max_grad_norm=0.5,
                policy_kwargs=dict(net_arch=[256, 256]),
                verbose=1,
                tensorboard_log=args.tb_log,
                seed=args.seed,
            )
        else:
            model = SAC(
                "MlpPolicy",
                train_env,
                learning_rate=3e-4,
                buffer_size=100_000,
                learning_starts=1000,
                batch_size=256,
                tau=0.005,
                gamma=0.99,
                train_freq=1,
                gradient_steps=1,
                policy_kwargs=dict(net_arch=[256, 256]),
                verbose=1,
                tensorboard_log=args.tb_log,
                seed=args.seed,
            )

    # ── Callbacks ──
    eval_cb = EvalCallback(
        eval_env,
        best_model_save_path=args.checkpoint_dir,
        log_path=args.tb_log,
        eval_freq=max(5000 // max(1, args.n_envs if args.algo == "ppo" else 1), 1000),
        n_eval_episodes=args.eval_episodes,
        deterministic=True,
    )
    ckpt_cb = CheckpointCallback(
        save_freq=max(10000 // max(1, args.n_envs if args.algo == "ppo" else 1), 2000),
        save_path=args.checkpoint_dir,
        name_prefix=model_name,
    )
    metrics_cb = MetricsCallback()

    # ── Train ──
    print(f"\nTraining {args.algo.upper()} for {args.total_timesteps:,} timesteps "
          f"({args.num_ues} UEs, {args.total_prb} PRBs)")
    print(f"Checkpoints → {args.checkpoint_dir}/")
    print(f"Tensorboard → {args.tb_log}/")
    print()

    model.learn(
        total_timesteps=args.total_timesteps,
        callback=[eval_cb, ckpt_cb, metrics_cb],
        progress_bar=True,
    )

    # ── Save final model ──
    final_path = os.path.join(args.checkpoint_dir, f"{model_name}_final")
    model.save(final_path)
    print(f"\nFinal model saved to {final_path}.zip")

    # ── Final evaluation ──
    print("\n" + "=" * 60)
    print("Final evaluation (50 episodes)")
    print("=" * 60)
    from stable_baselines3.common.evaluation import evaluate_policy
    mean_r, std_r = evaluate_policy(model, eval_env, n_eval_episodes=50, deterministic=True)
    print(f"  RL agent:  reward={mean_r:+.2f}±{std_r:.2f}")

    for strategy in ["equal", "heuristic"]:
        result = evaluate_baseline("SemanticPRB-v0", env_kwargs, strategy, n_episodes=50)
        print(f"  {strategy:>10s}: reward={result['mean_reward']:+.2f}±{result['std_reward']:.2f}")

    train_env.close()
    eval_env.close()


if __name__ == "__main__":
    main()
