/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*!
 * \brief       Default pluggable policy functions for the DL scheduler pipeline.
 *
 * These are the built-in implementations behind the function pointers
 * (dl_ri_pmi_select, dl_tda_select, dl_beam_select, dl_mcs_select, dl_rb_alloc, dl_lcid_alloc)
 * wired up at MAC init time.  They can be replaced at runtime by external
 * scheduler plug-ins without touching the core scheduling loop in
 * gNB_scheduler_dlsch.c.
 */

#include "common/utils/nr/nr_common.h"
#include "gNB_scheduler_dlsch_default_policies.h"
/*MAC*/
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"

/*TAG*/
#include "NR_TAG-Id.h"

/*Softmodem params*/
#include "executables/softmodem-common.h"
#include "../../../nfapi/oai_integration/vendor_ext.h"
#include <inttypes.h>

// Default RI/PMI selector: reads rank and PMI from CSI feedback for new-tx,
// or from HARQ process state for retx.
void nr_dl_ri_pmi_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates)
{
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    NR_UE_sched_ctrl_t *sched_ctrl = &cand->UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &cand->UE->current_DL_BWP;
    if (cand->is_retx) {
      cand->sched_pdsch.nrOfLayers = sched_ctrl->harq_processes[cand->retx_harq_pid].sched_pdsch.nrOfLayers;
      cand->sched_pdsch.pm_index =
          get_pm_index(mac, cand->UE, dl_bwp->dci_format, cand->sched_pdsch.nrOfLayers, mac->radio_config.pdsch_AntennaPorts.XP);
    } else {
      cand->sched_pdsch.nrOfLayers = cand->csi_ri + 1;
      cand->sched_pdsch.pm_index = cand->csi_pm_index;
    }
  }
}

// Default TDA selector: picks the slot-wide TDA index from get_dl_tda(),
// then resolves tda_info per candidate using each UE's own BWP / search
// space / coreset. Marks invalids with skipped=true.
int nr_dl_tda_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates, frame_t frame, slot_t slot)
{
  int tda = get_dl_tda(mac, slot);
  AssertFatal(tda >= 0, "Unable to find PDSCH time domain allocation in list\n");
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;

  int n_valid = 0;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    if (cand->skipped)
      continue;
    NR_UE_info_t *UE = cand->UE;
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
    int coresetid = sched_ctrl->coreset->controlResourceSetId;
    NR_tda_info_t tda_info = get_dl_tda_info(dl_bwp,
                                             sched_ctrl->search_space->searchSpaceType->present,
                                             tda,
                                             scc->dmrs_TypeA_Position,
                                             1,
                                             TYPE_C_RNTI_,
                                             coresetid,
                                             false);
    if (!tda_info.valid_tda) {
      cand->skipped = true;
      continue;
    }

    /* For retransmissions with a changed TDA, refit rbSize to preserve TBS */
    if (cand->is_retx) {
      const NR_sched_pdsch_t *orig = &sched_ctrl->harq_processes[cand->retx_harq_pid].sched_pdsch;
      bool tda_changed =
          tda_info.startSymbolIndex != orig->tda_info.startSymbolIndex || tda_info.nrOfSymbols != orig->tda_info.nrOfSymbols;
      if (tda_changed) {
        uint16_t new_rbSize = check_dl_retx_feasibility(cand, tda, &tda_info, scc, dl_bwp->BWPSize);
        if (!new_rbSize) {
          cand->skipped = true;
          continue;
        }
        cand->retx_rbSize = new_rbSize;
      }
    }

    cand->sched_pdsch.time_domain_allocation = tda;
    cand->sched_pdsch.tda_info = tda_info;
    cand->alloc_slbitmap = SL_to_bitmap(tda_info.startSymbolIndex, tda_info.nrOfSymbols);
    n_valid++;
  }
  return n_valid;
}

static int compare_dl_pf_ptrs(const void *a, const void *b)
{
  const nr_dl_candidate_t *ca = *(const nr_dl_candidate_t *const *)a;
  const nr_dl_candidate_t *cb = *(const nr_dl_candidate_t *const *)b;
  /* retx first (INFINITY weight), then highest PF weight */
  float wa = ca->is_retx ? INFINITY : dl_pf_weight(ca->current_mcs, ca->mcs_table, ca->sched_pdsch.nrOfLayers, ca->avg_throughput);
  float wb = cb->is_retx ? INFINITY : dl_pf_weight(cb->current_mcs, cb->mcs_table, cb->sched_pdsch.nrOfLayers, cb->avg_throughput);
  return (wa < wb) - (wa > wb);
}

int nr_dl_beam_select_default(NR_beam_info_t *beam_info,
                              const int16_t *beam_index_list,
                              nr_dl_candidate_t *candidates,
                              int n_candidates,
                              frame_t frame,
                              slot_t slot,
                              int slots_per_frame)
{
  /* Build pointer array sorted by PF priority so retx and high-priority UEs claim beams first. */
  nr_dl_candidate_t *order[MAX_MOBILES_PER_GNB];
  int n_active = 0;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  if (!cand->skipped)
    order[n_active++] = cand;
  qsort(order, n_active, sizeof(*order), compare_dl_pf_ptrs);

  int n_valid = 0;
  for (int i = 0; i < n_active; i++) {
    nr_dl_candidate_t *cand = order[i];
    NR_beam_alloc_t beam = beam_allocation_procedure(beam_info, frame, slot, cand->alloc_beam_dir, slots_per_frame);
    if (beam.idx < 0) {
      cand->skipped = true;
      continue;
    }

    cand->alloc_beam_idx = beam.idx;
    cand->alloc_new_beam = beam.new_beam;
    n_valid++;
  }
  return n_valid;
}

void nr_dl_mcs_select_default(const gNB_MAC_INST *mac, nr_dl_candidate_t *candidates, int n_candidates)
{
  const NR_bler_options_t *bo = &mac->dl_bler;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  {
    int mcs;
    if (cand->is_retx) {
      mcs = cand->current_mcs; /* retx MCS is fixed by the HARQ round */
    } else if (bo->harq_round_max == 1) {
      mcs = max(bo->min_mcs, min(bo->max_mcs, cand->max_mcs));
    } else if (!cand->bler_updated) {
      mcs = cand->current_mcs;
    } else {
      mcs = nr_adapt_mcs_from_bler(cand->current_mcs,
                                   bo->min_mcs,
                                   cand->max_mcs,
                                   cand->bler,
                                   bo->lower,
                                   bo->upper,
                                   cand->last_num_sched);
    }
    cand->sched_pdsch.mcs = mcs;
    /* Persist for all candidates — BLER-based MCS ramps even for UEs the
     * policy rejects this slot (failed CCE, no free RBs, etc.). */
    if (!cand->is_retx)
      cand->UE->UE_sched_ctrl.dl_bler_stats.mcs = mcs;
  }
}

static int compare_dl_pf_rb_ptrs(const void *a, const void *b)
{
  const nr_dl_candidate_t *ca = *(const nr_dl_candidate_t *const *)a;
  const nr_dl_candidate_t *cb = *(const nr_dl_candidate_t *const *)b;
  /* retx first, then highest PF weight (uses sched_pdsch.mcs, which is set by mcs_select) */
  float wa =
      ca->is_retx ? INFINITY : dl_pf_weight(ca->sched_pdsch.mcs, ca->mcs_table, ca->sched_pdsch.nrOfLayers, ca->avg_throughput);
  float wb =
      cb->is_retx ? INFINITY : dl_pf_weight(cb->sched_pdsch.mcs, cb->mcs_table, cb->sched_pdsch.nrOfLayers, cb->avg_throughput);
  return (wa < wb) - (wa > wb);
}

static bool nssai_matches_policy(nssai_t candidate, nssai_t policy)
{
  return candidate.sst == policy.sst && candidate.sd == policy.sd;
}

static const nr_dl_slice_policy_t *find_slice_policy(const gNB_MAC_INST *mac, nssai_t nssai)
{
  if (!mac->dl_slice_policy_enabled)
    return NULL;

  for (int i = 0; i < mac->dl_slice_policy_count; i++) {
    const nr_dl_slice_policy_t *policy = &mac->dl_slice_policy[i];
    if (policy->active && nssai_matches_policy(nssai, policy->nssai))
      return policy;
  }
  return NULL;
}

static int find_slice_policy_index(const gNB_MAC_INST *mac, const nr_dl_slice_policy_t *policy)
{
  if (policy == NULL)
    return -1;
  for (int i = 0; i < mac->dl_slice_policy_count; i++) {
    if (&mac->dl_slice_policy[i] == policy)
      return i;
  }
  return -1;
}

static void add_slice_policy_prb_stats(const nr_dl_sched_params_t *params, const nr_dl_slice_policy_t *policy, int rb_size)
{
  gNB_MAC_INST *mac = params->mac;
  int policy_idx = find_slice_policy_index(mac, policy);
  if (policy_idx < 0)
    return;

  mac->dl_slice_policy_prb_used[policy_idx] += rb_size;
  if (params->slot != 0 || params->frame == mac->dl_slice_policy_last_log_frame || params->frame % 100 != 0)
    return;

  mac->dl_slice_policy_last_log_frame = params->frame;
  for (int i = 0; i < mac->dl_slice_policy_count; i++) {
    const nr_dl_slice_policy_t *p = &mac->dl_slice_policy[i];
    if (!p->active)
      continue;
    LOG_I(NR_MAC,
          "slice_prb sst=%u sd=%06x dedicated=%u used=%" PRIu64 "\n",
          p->nssai.sst,
          p->nssai.sd,
          p->dedicated_prb_ratio,
          mac->dl_slice_policy_prb_used[i]);
  }
}

static int get_limited_free_block(const nr_dl_sched_params_t *params,
                                  nr_dl_candidate_t *cand,
                                  int rb_budget,
                                  int *rb_start)
{
  int max_rb_size = find_largest_free_block(params->vrb_map[cand->alloc_beam_idx],
                                            cand->alloc_slbitmap,
                                            cand->bwp_start,
                                            cand->bwp_size,
                                            rb_start);
  if (rb_budget > 0)
    max_rb_size = min(max_rb_size, rb_budget);
  return max_rb_size;
}

static int alloc_newtx_pf(const nr_dl_sched_params_t *params,
                          nr_dl_candidate_t **order,
                          int n_active,
                          const nr_dl_slice_policy_t *policy,
                          int rb_budget,
                          int *n_scheduled)
{
  const int min_rbSize = 5;
  int used_budget = 0;

  for (int j = 0; j < n_active; j++) {
    nr_dl_candidate_t *cand = order[j];
    if (cand->scheduled || cand->is_retx || cand->pending_bytes == 0)
      continue;
    if (policy != NULL && !nssai_matches_policy(cand->nssai, policy->nssai))
      continue;
    if (policy == NULL && find_slice_policy(params->mac, cand->nssai) != NULL)
      continue;

    int rbStart;
    int remaining_budget = rb_budget > 0 ? rb_budget - used_budget : 0;
    if (rb_budget > 0 && remaining_budget < min_rbSize)
      break;
    int max_rbSize = get_limited_free_block(params, cand, remaining_budget, &rbStart);
    if (max_rbSize < min_rbSize)
      continue;

    int mcs = cand->sched_pdsch.mcs;
    uint8_t Qm = nr_get_Qm_dl(mcs, cand->mcs_table);
    uint16_t R = nr_get_code_rate_dl(mcs, cand->mcs_table);
    NR_pdsch_dmrs_t dmrs = get_dl_dmrs_params(params->mac->common_channels->ServingCellConfigCommon,
                                              &cand->UE->current_DL_BWP,
                                              &cand->sched_pdsch.tda_info,
                                              cand->sched_pdsch.nrOfLayers);
    const int oh = 3 * 4 + (cand->UE->UE_sched_ctrl.ta_apply ? 2 : 0);
    uint32_t tbs;
    uint16_t rbSize;
    nr_find_nb_rb(Qm,
                  R,
                  1,
                  cand->sched_pdsch.nrOfLayers,
                  cand->sched_pdsch.tda_info.nrOfSymbols,
                  dmrs.N_PRB_DMRS * dmrs.N_DMRS_SLOT,
                  cand->pending_bytes + oh,
                  min_rbSize,
                  max_rbSize,
                  &tbs,
                  &rbSize);

    COMMIT_ALLOC(params, cand, rbStart, rbSize, mcs, *n_scheduled);
    add_slice_policy_prb_stats(params, policy, rbSize);
    used_budget += rbSize;
  }

  return used_budget;
}

static int alloc_retx_and_no_data_pf(const nr_dl_sched_params_t *params, nr_dl_candidate_t **order, int n_active, int *n_scheduled)
{
  const int min_rbSize = 5;

  /* Phase 1: HARQ retransmissions (highest priority, exact RBs). */
  for (int j = 0; j < n_active; j++) {
    nr_dl_candidate_t *cand = order[j];
    if (cand->scheduled || !cand->is_retx)
      continue;

    int needed_rbs = cand->retx_rbSize;
    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart, rbSize;
    if (!get_rb_alloc(needed_rbs,
                      cand->bwp_size,
                      cand->bwp_start,
                      cand->bwp_size,
                      vrb_map,
                      cand->alloc_slbitmap,
                      &rbStart,
                      &rbSize))
      continue;

    COMMIT_ALLOC(params, cand, rbStart, needed_rbs, cand->sched_pdsch.mcs, *n_scheduled);
  }

  /* Phase 2: No-data UEs (TA command or beam switch MAC CE, no RLC data). */
  for (int j = 0; j < n_active; j++) {
    nr_dl_candidate_t *cand = order[j];
    if (cand->scheduled || cand->is_retx || cand->pending_bytes > 0)
      continue;

    uint16_t *vrb_map = params->vrb_map[cand->alloc_beam_idx];
    int rbStart, rbSize;
    if (!get_rb_alloc(min_rbSize,
                      cand->bwp_size,
                      cand->bwp_start,
                      cand->bwp_size,
                      vrb_map,
                      cand->alloc_slbitmap,
                      &rbStart,
                      &rbSize))
      continue;

    COMMIT_ALLOC(params, cand, rbStart, min_rbSize, cand->sched_pdsch.mcs, *n_scheduled);
  }

  return *n_scheduled;
}

static int build_pf_order(nr_dl_candidate_t *candidates, int n_candidates, nr_dl_candidate_t **order)
{
  int n_active = 0;
  FOR_EACH_CANDIDATE(cand, candidates, n_candidates)
  if (!cand->skipped)
    order[n_active++] = cand;
  qsort(order, n_active, sizeof(*order), compare_dl_pf_rb_ptrs);
  return n_active;
}

int nr_dl_proportional_fair(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates)
{
  int n_scheduled = 0;
  nr_dl_candidate_t *order[MAX_MOBILES_PER_GNB];
  int n_active = build_pf_order(candidates, n_candidates, order);

  alloc_retx_and_no_data_pf(params, order, n_active, &n_scheduled);
  alloc_newtx_pf(params, order, n_active, NULL, 0, &n_scheduled);

  return n_scheduled;
}

int nr_dl_slice_prb_policy(const nr_dl_sched_params_t *params, nr_dl_candidate_t *candidates, int n_candidates)
{
  const gNB_MAC_INST *mac = params->mac;
  if (!mac->dl_slice_policy_enabled || mac->dl_slice_policy_count == 0)
    return nr_dl_proportional_fair(params, candidates, n_candidates);

  int n_scheduled = 0;
  nr_dl_candidate_t *order[MAX_MOBILES_PER_GNB];
  int n_active = build_pf_order(candidates, n_candidates, order);

  alloc_retx_and_no_data_pf(params, order, n_active, &n_scheduled);

  for (int i = 0; i < mac->dl_slice_policy_count; i++) {
    const nr_dl_slice_policy_t *policy = &mac->dl_slice_policy[i];
    if (!policy->active || policy->dedicated_prb_ratio == 0)
      continue;
    int total_budget = 0;
    for (int b = 0; b < params->num_beams; b++)
      total_budget += params->n_rb_avail[b] * policy->dedicated_prb_ratio / 100;
    if (total_budget <= 0)
      continue;
    alloc_newtx_pf(params, order, n_active, policy, total_budget, &n_scheduled);
  }

  alloc_newtx_pf(params, order, n_active, NULL, 0, &n_scheduled);
  return n_scheduled;
}

bool nr_mac_set_dl_slice_policies(gNB_MAC_INST *mac, const nr_dl_slice_policy_t *policies, int count)
{
  if (mac == NULL || count < 0 || count > MAX_NUM_SLICES)
    return false;

  NR_SCHED_LOCK(&mac->sched_lock);
  memset(mac->dl_slice_policy, 0, sizeof(mac->dl_slice_policy));
  memset(mac->dl_slice_policy_prb_used, 0, sizeof(mac->dl_slice_policy_prb_used));
  mac->dl_slice_policy_last_log_frame = 0;
  for (int i = 0; i < count; i++) {
    if (policies[i].min_prb_ratio > 100 || policies[i].max_prb_ratio > 100 || policies[i].dedicated_prb_ratio > 100
        || policies[i].min_prb_ratio > policies[i].dedicated_prb_ratio
        || policies[i].dedicated_prb_ratio > policies[i].max_prb_ratio) {
      NR_SCHED_UNLOCK(&mac->sched_lock);
      return false;
    }
    mac->dl_slice_policy[i] = policies[i];
    mac->dl_slice_policy[i].active = true;
  }
  mac->dl_slice_policy_count = count;
  mac->dl_slice_policy_enabled = count > 0;
  NR_SCHED_UNLOCK(&mac->sched_lock);
  return true;
}

void nr_dl_lcid_alloc_default(const gNB_MAC_INST *mac,
                              const nr_dl_candidate_t *candidate,
                              int tbs_available,
                              int lcid_alloc[NR_MAX_NUM_LCID])
{
  (void)mac;
  (void)tbs_available;
  memset(lcid_alloc, 0, NR_MAX_NUM_LCID * sizeof(int));
  for (int lcid = 0; lcid < NR_MAX_NUM_LCID; lcid++)
    lcid_alloc[lcid] = candidate->pending_bytes_per_lcid[lcid];
}
