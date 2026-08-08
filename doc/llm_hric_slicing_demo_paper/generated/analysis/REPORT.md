# Five-UE LLM-hRIC Traffic Scenario Results

The three traffic scenarios are analyzed independently. Cross-scenario claims use equal-weight macro averages, not pooled windows.

# Traffic Scenario: balanced

Only runs with at least 90% five-UE coverage enter the primary paired comparison.

Primary runs: 3; degraded/audit-only runs: 0.

| Arm | Runs | Intent 1 satisfaction | Intent 2 satisfaction | I1 total TH | I2 total TH | Early SLA violation | Early reward AUC | Traffic recovery |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| llm_guided_ddpg | 1 | 1.0000 | 0.9994 | 97.347 | 96.437 | 0.0000 | 261.427 | nan s |
| llm_only | 1 | 0.9994 | 0.9939 | 94.576 | 92.135 | 0.0000 | 276.506 | nan s |
| ddpg_only | 1 | 0.7994 | 0.6978 | 95.494 | 93.735 | 0.1200 | 157.555 | nan s |

## Paired Comparisons

- `llm_guided_ddpg - ddpg_only` / `early_sla_violation_rate`: -0.1200, 95% CI [-0.1200, -0.1200], n=1
- `llm_guided_ddpg - ddpg_only` / `early_cumulative_reward`: 103.8726, 95% CI [103.8726, 103.8726], n=1
- `llm_guided_ddpg - ddpg_only` / `intent1_intent_satisfaction_rate`: 0.2006, 95% CI [0.2006, 0.2006], n=1
- `llm_guided_ddpg - ddpg_only` / `intent2_intent_satisfaction_rate`: 0.3017, 95% CI [0.3017, 0.3017], n=1
- `llm_guided_ddpg - ddpg_only` / `intent1_mean_total_dl_th_mbps`: 1.8531, 95% CI [1.8531, 1.8531], n=1
- `llm_guided_ddpg - ddpg_only` / `intent2_mean_total_dl_th_mbps`: 2.7016, 95% CI [2.7016, 2.7016], n=1
- `llm_guided_ddpg - ddpg_only` / `traffic_step_sla_recovery_mean_s`: nan, 95% CI [nan, nan], n=0
- `llm_guided_ddpg - ddpg_only` / `intent1_sla_deficit_auc`: -2.1899, 95% CI [-2.1899, -2.1899], n=1
- `llm_guided_ddpg - ddpg_only` / `intent2_sla_deficit_auc`: -0.0922, 95% CI [-0.0922, -0.0922], n=1
- `llm_guided_ddpg - llm_only` / `early_sla_violation_rate`: 0.0000, 95% CI [0.0000, 0.0000], n=1
- `llm_guided_ddpg - llm_only` / `early_cumulative_reward`: -15.0789, 95% CI [-15.0789, -15.0789], n=1
- `llm_guided_ddpg - llm_only` / `intent1_intent_satisfaction_rate`: 0.0006, 95% CI [0.0006, 0.0006], n=1
- `llm_guided_ddpg - llm_only` / `intent2_intent_satisfaction_rate`: 0.0056, 95% CI [0.0056, 0.0056], n=1
- `llm_guided_ddpg - llm_only` / `intent1_mean_total_dl_th_mbps`: 2.7711, 95% CI [2.7711, 2.7711], n=1
- `llm_guided_ddpg - llm_only` / `intent2_mean_total_dl_th_mbps`: 4.3012, 95% CI [4.3012, 4.3012], n=1
- `llm_guided_ddpg - llm_only` / `traffic_step_sla_recovery_mean_s`: nan, 95% CI [nan, nan], n=0
- `llm_guided_ddpg - llm_only` / `intent1_sla_deficit_auc`: -0.0265, 95% CI [-0.0265, -0.0265], n=1
- `llm_guided_ddpg - llm_only` / `intent2_sla_deficit_auc`: -0.3535, 95% CI [-0.3535, -0.3535], n=1
- `llm_only - ddpg_only` / `early_sla_violation_rate`: -0.1200, 95% CI [-0.1200, -0.1200], n=1
- `llm_only - ddpg_only` / `early_cumulative_reward`: 118.9515, 95% CI [118.9515, 118.9515], n=1
- `llm_only - ddpg_only` / `intent1_intent_satisfaction_rate`: 0.2000, 95% CI [0.2000, 0.2000], n=1
- `llm_only - ddpg_only` / `intent2_intent_satisfaction_rate`: 0.2961, 95% CI [0.2961, 0.2961], n=1
- `llm_only - ddpg_only` / `intent1_mean_total_dl_th_mbps`: -0.9180, 95% CI [-0.9180, -0.9180], n=1
- `llm_only - ddpg_only` / `intent2_mean_total_dl_th_mbps`: -1.5995, 95% CI [-1.5995, -1.5995], n=1
- `llm_only - ddpg_only` / `traffic_step_sla_recovery_mean_s`: nan, 95% CI [nan, nan], n=0
- `llm_only - ddpg_only` / `intent1_sla_deficit_auc`: -2.1634, 95% CI [-2.1634, -2.1634], n=1
- `llm_only - ddpg_only` / `intent2_sla_deficit_auc`: 0.2613, 95% CI [0.2613, 0.2613], n=1

## Macro Average

Each available scenario contributes one equal-weight arm mean.

- `llm_guided_ddpg`: intent satisfaction=0.9997, total DL TH=96.892 Mbps
- `llm_only`: intent satisfaction=0.9967, total DL TH=93.356 Mbps
- `ddpg_only`: intent satisfaction=0.7486, total DL TH=94.615 Mbps
