#include "rc_ctrl_service_style_2.h"

#include "LAYER2/NR_MAC_gNB/gNB_scheduler_dlsch_default_policies.h"
#include "common/utils/LOG/log.h"
#include "common/ran_context.h"
#include "openair2/E2AP/flexric/src/sm/rc_sm/ie/ir/lst_ran_param.h"
#include "openair2/E2AP/flexric/src/sm/rc_sm/ie/ir/ran_param_list.h"
#include "openair2/E2AP/flexric/src/sm/rc_sm/ie/ir/ran_parameter_value.h"
#include "openair2/E2AP/flexric/src/util/byte_array.h"
#include "assertions.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern RAN_CONTEXT_t RC;

void fill_rc_ctrl_style_2(seq_ctrl_style_t *ctrl_style)
{
  memset(ctrl_style, 0, sizeof(*ctrl_style));

  ctrl_style->style_type = 2;
  ctrl_style->name = cp_str_to_ba("Radio Resource Allocation Control");
  ctrl_style->hdr = FORMAT_1_E2SM_RC_CTRL_HDR;
  ctrl_style->msg = FORMAT_1_E2SM_RC_CTRL_MSG;
  ctrl_style->call_proc_id_type = NULL;
  ctrl_style->out_frmt = FORMAT_1_E2SM_RC_CTRL_OUT;

  ctrl_style->sz_seq_ctrl_act = 1;
  ctrl_style->seq_ctrl_act = calloc(ctrl_style->sz_seq_ctrl_act, sizeof(*ctrl_style->seq_ctrl_act));
  AssertFatal(ctrl_style->seq_ctrl_act != NULL, "Memory exhausted\n");

  seq_ctrl_act_2_t *ctrl_act = &ctrl_style->seq_ctrl_act[0];
  ctrl_act->id = RC_CTRL_ACTION_SLICE_LEVEL_PRB_QUOTA;
  ctrl_act->name = cp_str_to_ba("Slice-level PRB quota");
  ctrl_act->sz_seq_assoc_ran_param = 1;
  ctrl_act->assoc_ran_param = calloc(ctrl_act->sz_seq_assoc_ran_param, sizeof(*ctrl_act->assoc_ran_param));
  AssertFatal(ctrl_act->assoc_ran_param != NULL, "Memory exhausted\n");

  seq_ran_param_3_t *rrm_policy_ratio_list = &ctrl_act->assoc_ran_param[0];
  rrm_policy_ratio_list->id = RC_SLICE_PARAM_RRM_POLICY_RATIO_LIST;
  rrm_policy_ratio_list->name = cp_str_to_ba("RRM Policy Ratio List");
  rrm_policy_ratio_list->def = NULL;

  ctrl_style->sz_ran_param_ctrl_out = 0;
  ctrl_style->ran_param_ctrl_out = NULL;
}

static bool get_integer_param(const seq_ran_param_t *param, uint32_t expected_id, int64_t *value)
{
  AssertError(param->ran_param_id == expected_id, return false, "Unexpected RAN parameter id %u, expected %u\n", param->ran_param_id, expected_id);
  AssertError(param->ran_param_val.type == ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE
                  || param->ran_param_val.type == ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE,
              return false,
              "RAN parameter %u is not an element value\n",
              expected_id);
  const ran_parameter_value_t *ran_value = param->ran_param_val.type == ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE
                                               ? param->ran_param_val.flag_true
                                               : param->ran_param_val.flag_false;
  AssertError(ran_value != NULL && ran_value->type == INTEGER_RAN_PARAMETER_VALUE,
              return false,
              "RAN parameter %u is not an integer\n",
              expected_id);
  *value = ran_value->int_ran;
  return true;
}

static bool get_octet_uint_param(const seq_ran_param_t *param, uint32_t expected_id, uint32_t *value)
{
  AssertError(param->ran_param_id == expected_id, return false, "Unexpected RAN parameter id %u, expected %u\n", param->ran_param_id, expected_id);
  AssertError(param->ran_param_val.type == ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE
                  || param->ran_param_val.type == ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE,
              return false,
              "RAN parameter %u is not an element value\n",
              expected_id);
  const ran_parameter_value_t *ran_value = param->ran_param_val.type == ELEMENT_KEY_FLAG_TRUE_RAN_PARAMETER_VAL_TYPE
                                               ? param->ran_param_val.flag_true
                                               : param->ran_param_val.flag_false;
  AssertError(ran_value != NULL, return false, "RAN parameter %u has NULL value\n", expected_id);
  if (ran_value->type == INTEGER_RAN_PARAMETER_VALUE) {
    AssertError(ran_value->int_ran >= 0 && ran_value->int_ran <= 0xffffff,
                return false,
                "RAN parameter %u integer value out of range\n",
                expected_id);
    *value = ran_value->int_ran;
    return true;
  }
  AssertError(ran_value->type == OCTET_STRING_RAN_PARAMETER_VALUE,
              return false,
              "RAN parameter %u is not an octet string\n",
              expected_id);
  char *str = cp_ba_to_str(ran_value->octet_str_ran);
  AssertError(str != NULL, return false, "Could not convert RAN parameter %u octet string\n", expected_id);
  char *end = NULL;
  unsigned long parsed = strtoul(str, &end, 0);
  bool ok = end != str && *end == '\0' && parsed <= 0xffffff;
  free(str);
  AssertError(ok, return false, "RAN parameter %u octet string is not a valid uint\n", expected_id);
  *value = parsed;
  return true;
}

static bool parse_slice_policy_group(const lst_ran_param_t *group, nr_dl_slice_policy_t *policy)
{
  AssertError(group->ran_param_struct.sz_ran_param_struct == 4, return false, "RRM Policy Ratio Group must contain 4 parameters\n");
  seq_ran_param_t *params = group->ran_param_struct.ran_param_struct;
  AssertError(params != NULL, return false, "RRM Policy Ratio Group has NULL parameters\n");

  seq_ran_param_t *rrm_policy = &params[0];
  AssertError(rrm_policy->ran_param_id == RC_SLICE_PARAM_RRM_POLICY
                  && rrm_policy->ran_param_val.type == STRUCTURE_RAN_PARAMETER_VAL_TYPE
                  && rrm_policy->ran_param_val.strct != NULL
                  && rrm_policy->ran_param_val.strct->sz_ran_param_struct == 1,
              return false,
              "Malformed RRM Policy parameter\n");

  seq_ran_param_t *member_list = &rrm_policy->ran_param_val.strct->ran_param_struct[0];
  AssertError(member_list->ran_param_id == RC_SLICE_PARAM_RRM_POLICY_MEMBER_LIST
                  && member_list->ran_param_val.type == LIST_RAN_PARAMETER_VAL_TYPE
                  && member_list->ran_param_val.lst != NULL
                  && member_list->ran_param_val.lst->sz_lst_ran_param >= 1,
              return false,
              "Malformed RRM Policy Member List\n");

  lst_ran_param_t *member = &member_list->ran_param_val.lst->lst_ran_param[0];
  AssertError(member->ran_param_struct.sz_ran_param_struct == 2, return false, "RRM Policy Member must contain PLMN and S-NSSAI\n");
  seq_ran_param_t *member_params = member->ran_param_struct.ran_param_struct;
  AssertError(member_params != NULL, return false, "RRM Policy Member has NULL parameters\n");

  seq_ran_param_t *s_nssai = &member_params[1];
  AssertError(s_nssai->ran_param_id == RC_SLICE_PARAM_S_NSSAI
                  && s_nssai->ran_param_val.type == STRUCTURE_RAN_PARAMETER_VAL_TYPE
                  && s_nssai->ran_param_val.strct != NULL,
              return false,
              "Malformed S-NSSAI parameter\n");
  AssertError(s_nssai->ran_param_val.strct->sz_ran_param_struct >= 1, return false, "S-NSSAI must contain at least SST\n");

  uint32_t sst = 0;
  uint32_t sd = 0xffffff;
  seq_ran_param_t *s_nssai_params = s_nssai->ran_param_val.strct->ran_param_struct;
  if (!get_octet_uint_param(&s_nssai_params[0], RC_SLICE_PARAM_SST, &sst))
    return false;
  if (s_nssai->ran_param_val.strct->sz_ran_param_struct > 1
      && !get_octet_uint_param(&s_nssai_params[1], RC_SLICE_PARAM_SD, &sd))
    return false;
  AssertError(sst <= UINT8_MAX && sd <= 0xffffff, return false, "Invalid S-NSSAI SST %u SD %u\n", sst, sd);

  int64_t min_ratio = 0;
  int64_t max_ratio = 0;
  int64_t dedicated_ratio = 0;
  if (!get_integer_param(&params[1], RC_SLICE_PARAM_MIN_PRB_POLICY_RATIO, &min_ratio)
      || !get_integer_param(&params[2], RC_SLICE_PARAM_MAX_PRB_POLICY_RATIO, &max_ratio)
      || !get_integer_param(&params[3], RC_SLICE_PARAM_DEDICATED_PRB_POLICY_RATIO, &dedicated_ratio))
    return false;
  AssertError(min_ratio >= 0 && min_ratio <= 100 && max_ratio >= 0 && max_ratio <= 100 && dedicated_ratio >= 0
                  && dedicated_ratio <= 100 && min_ratio <= dedicated_ratio && dedicated_ratio <= max_ratio,
              return false,
              "Invalid PRB ratios min=%ld dedicated=%ld max=%ld\n",
              min_ratio,
              dedicated_ratio,
              max_ratio);

  *policy = (nr_dl_slice_policy_t){
      .active = true,
      .nssai = {.sst = sst, .sd = sd},
      .min_prb_ratio = min_ratio,
      .max_prb_ratio = max_ratio,
      .dedicated_prb_ratio = dedicated_ratio,
  };
  LOG_I(NR_MAC,
        "RC slice policy SST %u SD %06x min %ld dedicated %ld max %ld\n",
        sst,
        sd,
        min_ratio,
        dedicated_ratio,
        max_ratio);
  return true;
}

bool apply_rc_ctrl_style_2_slice_level_prb_quota(const e2sm_rc_ctrl_msg_frmt_1_t *msg)
{
#if defined(NGRAN_GNB_DU)
  AssertError(msg != NULL && msg->sz_ran_param == 1 && msg->ran_param != NULL,
              return false,
              "Slice PRB quota control message must contain exactly one RAN parameter\n");
  seq_ran_param_t *rrm_policy_ratio_list = &msg->ran_param[0];
  AssertError(rrm_policy_ratio_list->ran_param_id == RC_SLICE_PARAM_RRM_POLICY_RATIO_LIST,
              return false,
              "Unexpected RAN parameter id %u for RRM Policy Ratio List\n",
              rrm_policy_ratio_list->ran_param_id);
  AssertError(rrm_policy_ratio_list->ran_param_val.type == LIST_RAN_PARAMETER_VAL_TYPE
                  && rrm_policy_ratio_list->ran_param_val.lst != NULL,
              return false,
              "RRM Policy Ratio List must be a non-NULL list\n");

  ran_param_list_t *list = rrm_policy_ratio_list->ran_param_val.lst;
  AssertError(list->sz_lst_ran_param <= MAX_NUM_SLICES,
              return false,
              "Too many slice policies %zu, max %d\n",
              list->sz_lst_ran_param,
              MAX_NUM_SLICES);

  nr_dl_slice_policy_t policies[MAX_NUM_SLICES] = {0};
  for (size_t i = 0; i < list->sz_lst_ran_param; i++) {
    if (!parse_slice_policy_group(&list->lst_ran_param[i], &policies[i]))
      return false;
  }

  AssertError(RC.nrmac != NULL && RC.nrmac[0] != NULL, return false, "No NR MAC instance available for slice policy\n");
  return nr_mac_set_dl_slice_policies(RC.nrmac[0], policies, list->sz_lst_ran_param);
#else
  (void)msg;
  LOG_W(NR_MAC, "RC Style 2 Slice-level PRB quota received on a non-DU build; ignoring control\n");
  return true;
#endif
}
