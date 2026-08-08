# LLM-hRIC OAI Network Slicing Paper

This directory contains the IEEE conference manuscript and a result generator for the OAI/FlexRIC LLM-hRIC deployment. The manuscript distinguishes the published LLM-hRIC framework from this implementation paper and labels incomplete campaigns as pilot evidence.

## Generate Results

> **The committed `generated/` directory is frozen and cannot be regenerated for the
> current pilot.** Its three source run databases were written under `/tmp` and were
> destroyed when the host was rebooted to repair an NVIDIA driver/library version
> mismatch. `generated/` is therefore the sole surviving evidence for every number in
> the manuscript: **do not delete it, and do not run `make results` against an empty or
> partial results directory**, which would overwrite it. Re-running the generator is
> only meaningful after a new campaign has been executed with `--results` pointing at a
> **persistent** path.

```bash
cd /home/ics1/openairinterface5g/doc/llm_hric_slicing_demo_paper

# Only for a NEW campaign; RESULTS must be a persistent path, never /tmp.
/home/ics1/anaconda3/bin/python scripts/build_results.py \
  --results "$HOME/llm_hric_results/five-ue-traffic-v5" \
  --spec ../../openair2/E2AP/flexric/examples/xApp/python3/llm_hric/experiments/five_ue_traffic_scenarios_v5.json \
  --output generated
```

The selector accepts only runs whose manifest is `complete`, whose nested validation data quality is `primary`, and whose SQLite database exists. If several valid runs share the same `scenario/arm/seed`, only the newest `end_ts_ms` is selected. All exclusions are recorded in `generated/artifact_manifest.json`.

The current dataset contains three balanced, seed-1 arms. It is therefore emitted as a pilot. A full campaign requires 3 scenarios x 3 arms x 5 seeds = 45 primary runs.

## Validate Sources

```bash
make test
make check
```

## Build PDF

The current host did not expose `pdflatex`, `latexmk`, or `IEEEtran.cls` when this artifact was created. Install a TeX distribution with IEEEtran, TikZ, PGFPlots, and the standard LaTeX extra packages, then run:

```bash
make pdf
```

On Debian/Ubuntu, the relevant packages are commonly provided by `latexmk`, `texlive-latex-base`, `texlive-latex-extra`, `texlive-publishers`, and `texlive-pictures`. This repository does not install system packages automatically.

## Editing Metadata

Author affiliations and the public artifact URL are defined once near the top of `main.tex` through `\affiliationYonsei`, `\affiliationETRI`, `\affiliationSUTD`, and `\codeurl`.

## Result Integrity

- `generated/selected_runs.csv` identifies every database used by the paper.
- `generated/run_metrics.csv` contains the source metrics used in generated tables.
- `generated/guided_vs_ddpg_table.tex` reports the direct guided-versus-DDPG-only pilot comparison, including unfavorable diagnostics.
- `generated/ddpg_only_phase_metrics.csv` separates calibration, training, and both frozen intent evaluations.
- `generated/ddpg_only_priority_metrics.csv` separates Slice-A-priority and Slice-B-priority training behavior.
- `generated/balanced_ddpg_only_diagnostics.png` shows DDPG-only reward, intent satisfaction, action, TD error, replay growth, and serving-Actor publication.
- `generated/artifact_manifest.json` records selected and excluded runs, spec hash, revisions, and campaign completeness.
- The generator never treats repeated windows from one run as independent seeds.
- Old v3/v4 result directories are not read unless explicitly passed as `--results`.
