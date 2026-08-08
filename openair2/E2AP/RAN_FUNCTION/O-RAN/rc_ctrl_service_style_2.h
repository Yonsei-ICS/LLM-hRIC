#ifndef RC_CTRL_SERVICE_STYLE_2_H
#define RC_CTRL_SERVICE_STYLE_2_H

#include "openair2/E2AP/flexric/src/sm/rc_sm/ie/rc_data_ie.h"
#include "openair2/E2AP/flexric/src/sm/rc_sm/ie/ir/e2sm_rc_ctrl_msg_frmt_1.h"
#include "openair2/E2AP/flexric/src/sm/rc_sm/ie/ir/ran_func_def_ctrl.h"

#include <stdbool.h>

typedef enum {
  RC_CTRL_ACTION_DRX_PARAMETER_CONFIGURATION = 1,
  RC_CTRL_ACTION_SR_PERIODICITY_CONFIGURATION = 2,
  RC_CTRL_ACTION_SPS_PARAMETERS_CONFIGURATION = 3,
  RC_CTRL_ACTION_CONFIGURED_GRANT_CONTROL = 4,
  RC_CTRL_ACTION_CQI_TABLE_CONFIGURATION = 5,
  RC_CTRL_ACTION_SLICE_LEVEL_PRB_QUOTA = 6,
} rc_ctrl_service_style_2_act_id_e;

typedef enum {
  RC_SLICE_PARAM_RRM_POLICY_RATIO_LIST = 1,
  RC_SLICE_PARAM_RRM_POLICY_RATIO_GROUP = 2,
  RC_SLICE_PARAM_RRM_POLICY = 3,
  RC_SLICE_PARAM_RRM_POLICY_MEMBER_LIST = 4,
  RC_SLICE_PARAM_RRM_POLICY_MEMBER = 5,
  RC_SLICE_PARAM_PLMN_IDENTITY = 6,
  RC_SLICE_PARAM_S_NSSAI = 7,
  RC_SLICE_PARAM_SST = 8,
  RC_SLICE_PARAM_SD = 9,
  RC_SLICE_PARAM_MIN_PRB_POLICY_RATIO = 10,
  RC_SLICE_PARAM_MAX_PRB_POLICY_RATIO = 11,
  RC_SLICE_PARAM_DEDICATED_PRB_POLICY_RATIO = 12,
} rc_ctrl_slice_level_prb_quota_param_id_e;

void fill_rc_ctrl_style_2(seq_ctrl_style_t *ctrl_style);
bool apply_rc_ctrl_style_2_slice_level_prb_quota(const e2sm_rc_ctrl_msg_frmt_1_t *msg);

#endif /* RC_CTRL_SERVICE_STYLE_2_H */
