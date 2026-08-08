/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BAS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "../../../../src/xApp/e42_xapp_api.h"
#include "../../../../src/util/alg_ds/alg/defer.h"
#include "../../../../src/util/time_now_us.h"
#include "../../../../src/util/alg_ds/alg/murmur_hash_32.h"
#include "../../../../src/util/alg_ds/ds/lock_guard/lock_guard.h"
#include "../../../../src/util/alg_ds/ds/assoc_container/assoc_generic.h"
#include "../../../../src/util/e.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sqlite3.h>

static
uint64_t const period_ms = 1000;

static
pthread_mutex_t mtx;

static sqlite3 *kpm_db;

static int64_t kpm_epoch_ms(uint64_t timestamp)
{
  if (timestamp > 100000000000000000ULL) // nanoseconds
    return timestamp / 1000000;
  if (timestamp > 100000000000000ULL) // microseconds
    return timestamp / 1000;
  if (timestamp > 100000000000ULL) // milliseconds
    return timestamp;
  if (timestamp > 1000000000ULL) // seconds
    return timestamp * 1000;
  return time_now_us() / 1000;
}

static void init_kpm_db(void)
{
  const char *path = getenv("LLM_HRIC_DB_PATH");
  if (path == NULL || *path == '\0')
    return;
  int rc = sqlite3_open(path, &kpm_db);
  assert(rc == SQLITE_OK && "Cannot open LLM-hRIC SQLite DB");
  sqlite3_busy_timeout(kpm_db, 5000);
  sqlite3_exec(kpm_db, "PRAGMA journal_mode=WAL", NULL, NULL, NULL);
  const char *sql =
      "CREATE TABLE IF NOT EXISTS kpm_measurements_raw("
      "ts_ms INTEGER NOT NULL,recv_ts_ms INTEGER NOT NULL,node_id TEXT NOT NULL,scope TEXT NOT NULL,"
      "ue_id_type TEXT,ue_id TEXT,measurement TEXT NOT NULL,value_type TEXT NOT NULL,value REAL,"
      "unit TEXT,labels_json TEXT,reliable INTEGER NOT NULL)";
  rc = sqlite3_exec(kpm_db, sql, NULL, NULL, NULL);
  assert(rc == SQLITE_OK && "Cannot create KPM raw table");
}

static void write_kpm_row(int64_t ts_ms,
                          const char *scope,
                          const char *ue_id_type,
                          const char *ue_id,
                          const char *measurement,
                          const char *value_type,
                          double value,
                          const char *unit,
                          bool reliable)
{
  if (kpm_db == NULL)
    return;
  const char *sql =
      "INSERT INTO kpm_measurements_raw(ts_ms,recv_ts_ms,node_id,scope,ue_id_type,ue_id,measurement,"
      "value_type,value,unit,labels_json,reliable) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
  sqlite3_stmt *stmt = NULL;
  int rc = sqlite3_prepare_v2(kpm_db, sql, -1, &stmt, NULL);
  if (rc != SQLITE_OK)
    return;
  sqlite3_bind_int64(stmt, 1, ts_ms);
  sqlite3_bind_int64(stmt, 2, time_now_us() / 1000);
  sqlite3_bind_text(stmt, 3, "e2-node", -1, SQLITE_STATIC);
  sqlite3_bind_text(stmt, 4, scope, -1, SQLITE_TRANSIENT);
  if (ue_id_type != NULL)
    sqlite3_bind_text(stmt, 5, ue_id_type, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 5);
  if (ue_id != NULL)
    sqlite3_bind_text(stmt, 6, ue_id, -1, SQLITE_TRANSIENT);
  else
    sqlite3_bind_null(stmt, 6);
  sqlite3_bind_text(stmt, 7, measurement, -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 8, value_type, -1, SQLITE_TRANSIENT);
  sqlite3_bind_double(stmt, 9, value);
  sqlite3_bind_text(stmt, 10, unit != NULL ? unit : "", -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, "{}", -1, SQLITE_STATIC);
  sqlite3_bind_int(stmt, 12, reliable);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

static
assoc_ht_open_t ht = {0};

static
uint32_t hash_func(const void* key_v)
{
  char* key = *(char**)(key_v);
  static const uint32_t seed = 42;
  return murmur3_32((uint8_t*)key, strlen(key), seed);
}

static
bool cmp_str(const void* a, const void* b)
{
  char* a_str = *(char**)(a);
  char* b_str = *(char**)(b);

  int const ret = strcmp(a_str, b_str);
  return ret == 0;
}

static
void free_str(void* key, void* value)
{
  free(*(char**)key);
  free(value);
}

static
void free_kpm_meas_unit_hash_table(void)
{
  assoc_ht_open_free(&ht);
}

static
void init_kpm_meas_unit_hash_table(void)
{
  FILE *fp = fopen(KPM_MEAS_LIST, "r");
  if (!fp) {
    printf("Cannot open the file \"%s\".\n", KPM_MEAS_LIST);
    perror("Error");
    return;
  }

  assoc_ht_open_init(&ht, sizeof(char*), cmp_str, free_str, hash_func);
  char line[128];
  while (fgets(line, sizeof(line), fp)) {
    char *col1, *col2;
    sscanf(line, "%ms %ms", &col1, &col2);
    assoc_ht_open_insert(&ht, &col1, sizeof(char*), col2);
  }
  fclose(fp);
}

static
char *get_meas_unit(const char *name)
{
  return assoc_ht_open_value(&ht, &name);
}

static
void log_gnb_ue_id(ue_id_e2sm_t ue_id)
{
  if (ue_id.gnb.gnb_cu_ue_f1ap_lst != NULL) {
    for (size_t i = 0; i < ue_id.gnb.gnb_cu_ue_f1ap_lst_len; i++) {
      printf("UE ID type = gNB-CU, gnb_cu_ue_f1ap = %u\n", ue_id.gnb.gnb_cu_ue_f1ap_lst[i]);
    }
  } else {
    printf("UE ID type = gNB, amf_ue_ngap_id = %lu\n", ue_id.gnb.amf_ue_ngap_id);
  }
  if (ue_id.gnb.ran_ue_id != NULL) {
    printf("ran_ue_id = %lx\n", *ue_id.gnb.ran_ue_id); // RAN UE NGAP ID
  }
}

static
void log_du_ue_id(ue_id_e2sm_t ue_id)
{
  printf("UE ID type = gNB-DU, gnb_cu_ue_f1ap = %u\n", ue_id.gnb_du.gnb_cu_ue_f1ap);
  if (ue_id.gnb_du.ran_ue_id != NULL) {
    printf("ran_ue_id = %lx\n", *ue_id.gnb_du.ran_ue_id); // RAN UE NGAP ID
  }
}

static
void log_cuup_ue_id(ue_id_e2sm_t ue_id)
{
  printf("UE ID type = gNB-CU-UP, gnb_cu_cp_ue_e1ap = %u\n", ue_id.gnb_cu_up.gnb_cu_cp_ue_e1ap);
  if (ue_id.gnb_cu_up.ran_ue_id != NULL) {
    printf("ran_ue_id = %lx\n", *ue_id.gnb_cu_up.ran_ue_id); // RAN UE NGAP ID
  }
}

typedef void (*log_ue_id)(ue_id_e2sm_t ue_id);

static
log_ue_id log_ue_id_e2sm[END_UE_ID_E2SM] = {
    log_gnb_ue_id, // common for gNB-mono, CU and CU-CP
    log_du_ue_id,
    log_cuup_ue_id,
    NULL,
    NULL,
    NULL,
    NULL,
};

static
void log_int_value(const char *name_str, const label_info_lst_t label_info, const meas_record_lst_t meas_record)
{
  char *name_unit = get_meas_unit(name_str);
  if (label_info.noLabel != NULL) {
    printf("%s = %d %s\n", name_str, meas_record.int_val, name_unit);
  } else if (label_info.distBinX != NULL && meas_record.int_val > 0) {
    printf("%s[BinX=%d][BinY=%d][BinZ=%d] = %d %s\n", name_str, *label_info.distBinX, *label_info.distBinY, *label_info.distBinZ, meas_record.int_val, name_unit);
  }
}

static
void log_real_value(const char *name_str, const label_info_lst_t label_info, const meas_record_lst_t meas_record)
{
  (void)label_info;
  char *name_unit = get_meas_unit(name_str);
  printf("%s = %.2f %s\n", name_str, meas_record.real_val, name_unit);
}

typedef void (*log_meas_value)(const char *name_str, const label_info_lst_t label_info, const meas_record_lst_t meas_record);

static
log_meas_value get_meas_value[END_MEAS_VALUE] = {
    log_int_value,
    log_real_value,
    NULL,
};

static
void match_meas_name_type(const meas_type_t meas_type, const label_info_lst_t label_info, const meas_record_lst_t record_item)
{
  // Get the value of the Measurement
  char *name_str = cp_ba_to_str(meas_type.name);
  get_meas_value[record_item.value](name_str, label_info, record_item);
  free(name_str);
}

static
void match_id_meas_type(const meas_type_t meas_type, const label_info_lst_t label_info, const meas_record_lst_t record_item)
{
  (void)meas_type;
  (void)label_info;
  (void)record_item;
  assert(false && "ID Measurement Type not yet supported");
}

typedef void (*check_meas_type)(const meas_type_t meas_type, const label_info_lst_t label_info, const meas_record_lst_t meas_record);

static
check_meas_type match_meas_type[END_MEAS_TYPE] = {
    match_meas_name_type,
    match_id_meas_type,
};

static
void log_kpm_measurements(kpm_ind_msg_format_1_t const* msg_frm_1,
                          int64_t base_ts_ms,
                          const char *scope,
                          const char *ue_id_type,
                          const char *ue_id)
{
  assert(msg_frm_1->meas_info_lst_len > 0 && "Cannot correctly print measurements");

  // UE Measurements per granularity period
  for (size_t j = 0; j < msg_frm_1->meas_data_lst_len; j++) {
    meas_data_lst_t const data_item = msg_frm_1->meas_data_lst[j];

    for (size_t i = 0; i < msg_frm_1->meas_info_lst_len; i++) {
      if (i >= data_item.meas_record_len) {
        printf("KPM record length mismatch: info=%zu records=%zu\n",
               msg_frm_1->meas_info_lst_len,
               data_item.meas_record_len);
        break;
      }
      const meas_info_format_1_lst_t info_item = msg_frm_1->meas_info_lst[i];
      const label_info_lst_t label_info = info_item.label_info_lst_len > 0
                                              ? info_item.label_info_lst[0]
                                              : (label_info_lst_t){0};
      const meas_record_lst_t record_item = data_item.meas_record_lst[i];
      if (record_item.value >= END_MEAS_VALUE || record_item.value == NO_VALUE_MEAS_VALUE)
        continue;
      if (info_item.meas_type.type >= END_MEAS_TYPE || match_meas_type[info_item.meas_type.type] == NULL)
        continue;

      match_meas_type[info_item.meas_type.type](info_item.meas_type, label_info, record_item);

      if (info_item.meas_type.type == NAME_MEAS_TYPE) {
        char *name = cp_ba_to_str(info_item.meas_type.name);
        char *unit = get_meas_unit(name);
        bool reliable = data_item.incomplete_flag == NULL || *data_item.incomplete_flag != TRUE_ENUM_VALUE;
        double value = record_item.value == INTEGER_MEAS_VALUE ? record_item.int_val : record_item.real_val;
        write_kpm_row(base_ts_ms + j * period_ms,
                      scope,
                      ue_id_type,
                      ue_id,
                      name,
                      record_item.value == INTEGER_MEAS_VALUE ? "integer" : "real",
                      value,
                      unit,
                      reliable);
        free(name);
      }

      if (data_item.incomplete_flag && *data_item.incomplete_flag == TRUE_ENUM_VALUE)
        printf("Measurement Record not reliable");
    }
  }
}

static
void log_kpm_ind_msg_frm_3(kpm_ind_msg_format_3_t const* msg, int64_t base_ts_ms)
{
  // Reported list of measurements per UE
  for (size_t i = 0; i < msg->ue_meas_report_lst_len; i++) {
    // log UE ID
    ue_id_e2sm_t const ue_id_e2sm = msg->meas_report_per_ue[i].ue_meas_report_lst;
    ue_id_e2sm_e const type = ue_id_e2sm.type;
    if (type < END_UE_ID_E2SM && log_ue_id_e2sm[type] != NULL)
      log_ue_id_e2sm[type](ue_id_e2sm);

    char ue_id_type[32] = {0};
    char ue_id[64] = {0};
    snprintf(ue_id_type, sizeof(ue_id_type), "%d", type);
    if (type == GNB_UE_ID_E2SM)
      snprintf(ue_id, sizeof(ue_id), "%lu", ue_id_e2sm.gnb.amf_ue_ngap_id);
    else if (type == GNB_DU_UE_ID_E2SM)
      snprintf(ue_id, sizeof(ue_id), "%u", ue_id_e2sm.gnb_du.gnb_cu_ue_f1ap);
    else
      snprintf(ue_id, sizeof(ue_id), "unknown");

    // log measurements
    log_kpm_measurements(&msg->meas_report_per_ue[i].ind_msg_format_1, base_ts_ms, "ue", ue_id_type, ue_id);
  }
}

static
void sm_cb_kpm(sm_ag_if_rd_t const* rd)
{
  assert(rd != NULL);
  assert(rd->type == INDICATION_MSG_AGENT_IF_ANS_V0);
  assert(rd->ind.type == KPM_STATS_V3_0);

  // Reading Indication Message Format 3
  kpm_ind_data_t const* ind = &rd->ind.kpm.ind;
  kpm_ric_ind_hdr_format_1_t const* hdr_frm_1 = &ind->hdr.kpm_ric_ind_hdr_format_1;

  int64_t const now = time_now_us();
  static int counter = 1;
  {
    lock_guard(&mtx);

    printf("\n%7d KPM ind_msg latency = %ld [μs]\n", counter, now - kpm_epoch_ms(hdr_frm_1->collectStartTime) * 1000); // xApp <-> E2 Node

    if (ind->msg.type == FORMAT_1_INDICATION_MESSAGE) {
      log_kpm_measurements(&ind->msg.frm_1, kpm_epoch_ms(hdr_frm_1->collectStartTime), "cell", NULL, NULL);
    } else if (ind->msg.type == FORMAT_3_INDICATION_MESSAGE) {
      log_kpm_ind_msg_frm_3(&ind->msg.frm_3, kpm_epoch_ms(hdr_frm_1->collectStartTime));
    } else {
      printf("KPM Indication Message %d logging not yet implemented.\n", ind->msg.type);
    }
    counter++;
  }
}

static
test_info_lst_t filter_predicate(test_cond_type_e type, test_cond_e cond, int value)
{
  test_info_lst_t dst = {0};

  dst.test_cond_type = type;
  // It can only be TRUE_TEST_COND_TYPE so it does not matter the type
  // but ugly ugly...
  dst.S_NSSAI = TRUE_TEST_COND_TYPE;

  dst.test_cond = calloc(1, sizeof(test_cond_e));
  assert(dst.test_cond != NULL && "Memory exhausted");
  *dst.test_cond = cond;

  dst.test_cond_value = calloc(1, sizeof(test_cond_value_t));
  assert(dst.test_cond_value != NULL && "Memory exhausted");
  dst.test_cond_value->type = OCTET_STRING_TEST_COND_VALUE;

  dst.test_cond_value->octet_string_value = calloc(1, sizeof(byte_array_t));
  assert(dst.test_cond_value->octet_string_value != NULL && "Memory exhausted");
  const size_t len_nssai = 1;
  dst.test_cond_value->octet_string_value->len = len_nssai;
  dst.test_cond_value->octet_string_value->buf = calloc(len_nssai, sizeof(uint8_t));
  assert(dst.test_cond_value->octet_string_value->buf != NULL && "Memory exhausted");
  dst.test_cond_value->octet_string_value->buf[0] = value;

  return dst;
}

static
label_info_lst_t fill_kpm_label(void)
{
  label_info_lst_t label_item = {0};

  label_item.noLabel = ecalloc(1, sizeof(enum_value_e));
  *label_item.noLabel = TRUE_ENUM_VALUE;

  return label_item;
}

static
kpm_act_def_format_1_t fill_act_def_frm_1(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);

  kpm_act_def_format_1_t ad_frm_1 = {0};

  size_t const sz = report_item->meas_info_for_action_lst_len;

  // [1, 65535]
  ad_frm_1.meas_info_lst_len = sz;
  ad_frm_1.meas_info_lst = calloc(sz, sizeof(meas_info_format_1_lst_t));
  assert(ad_frm_1.meas_info_lst != NULL && "Memory exhausted");

  for (size_t i = 0; i < sz; i++) {
    meas_info_format_1_lst_t* meas_item = &ad_frm_1.meas_info_lst[i];
    // 8.3.9
    // Measurement Name
    meas_item->meas_type.type = NAME_MEAS_TYPE;
    meas_item->meas_type.name = copy_byte_array(report_item->meas_info_for_action_lst[i].name);

    // [1, 2147483647]
    // 8.3.11
    meas_item->label_info_lst_len = 1;
    meas_item->label_info_lst = ecalloc(1, sizeof(label_info_lst_t));
    meas_item->label_info_lst[0] = fill_kpm_label();
  }

  // 8.3.8 [0, 4294967295]
  ad_frm_1.gran_period_ms = period_ms;

  // 8.3.20 - OPTIONAL
  ad_frm_1.cell_global_id = NULL;

#if defined KPM_V2_03 || defined KPM_V3_00
  // [0, 65535]
  ad_frm_1.meas_bin_range_info_lst_len = 0;
  ad_frm_1.meas_bin_info_lst = NULL;
#endif

  return ad_frm_1;
}

static
kpm_act_def_t fill_report_style_4(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);
  assert(report_item->act_def_format_type == FORMAT_4_ACTION_DEFINITION);

  kpm_act_def_t act_def = {.type = FORMAT_4_ACTION_DEFINITION};

  // Fill matching condition
  // [1, 32768]
  act_def.frm_4.matching_cond_lst_len = 1;
  act_def.frm_4.matching_cond_lst = calloc(act_def.frm_4.matching_cond_lst_len, sizeof(matching_condition_format_4_lst_t));
  assert(act_def.frm_4.matching_cond_lst != NULL && "Memory exhausted");
  // Filter connected UEs by S-NSSAI criteria
  test_cond_type_e const type = S_NSSAI_TEST_COND_TYPE; // CQI_TEST_COND_TYPE
  test_cond_e const condition = EQUAL_TEST_COND; // GREATERTHAN_TEST_COND
  int const value = 1;
  act_def.frm_4.matching_cond_lst[0].test_info_lst = filter_predicate(type, condition, value);

  // Fill Action Definition Format 1
  // 8.2.1.2.1
  act_def.frm_4.action_def_format_1 = fill_act_def_frm_1(report_item);

  return act_def;
}

static
label_info_lst_t fill_distribution_bin_label(const uint32_t x, const uint32_t y, const uint32_t z)
{
  label_info_lst_t label_item = {0};

  label_item.distBinX = calloc(1, sizeof(uint32_t));
  assert(label_item.distBinX != NULL);
  *label_item.distBinX = x;

  label_item.distBinY = calloc(1, sizeof(uint32_t));
  assert(label_item.distBinY != NULL);
  *label_item.distBinY = y;

  label_item.distBinZ = calloc(1, sizeof(uint32_t));
  assert(label_item.distBinZ != NULL);
  *label_item.distBinZ = z;

  return label_item;
}

static
kpm_act_def_t fill_report_style_1(ric_report_style_item_t const* report_item)
{
  assert(report_item != NULL);
  assert(report_item->act_def_format_type == FORMAT_1_ACTION_DEFINITION);

  kpm_act_def_t act_def = {.type = FORMAT_1_ACTION_DEFINITION};

  // [1, 65535]
  act_def.frm_1.meas_info_lst_len = report_item->meas_info_for_action_lst_len;
  act_def.frm_1.meas_info_lst = ecalloc(act_def.frm_1.meas_info_lst_len, sizeof(meas_info_format_1_lst_t));
  for (size_t i = 0; i < act_def.frm_1.meas_info_lst_len; i++) {
    meas_info_format_1_lst_t* meas_item = &act_def.frm_1.meas_info_lst[i];
    // 8.3.9
    // Measurement Name
    meas_item->meas_type.type = NAME_MEAS_TYPE;
    meas_item->meas_type.name = copy_byte_array(report_item->meas_info_for_action_lst[i].name);

    // [1, 2147483647]
    // 8.3.11
    if (cmp_str_ba("CARR.PDSCHMCSDist", meas_item->meas_type.name) == 0) {
      /// 1-8 RI, 1-3 MCS table, 0-31 MCS value
      meas_item->label_info_lst_len = 8 * 3 * 32;
      meas_item->label_info_lst = ecalloc(meas_item->label_info_lst_len, sizeof(label_info_lst_t));
      size_t idx = 0;
      for (uint32_t x = 1; x <= 8; x++) {
        for (uint32_t y = 1; y <= 3; y++) {
          for(uint32_t z = 0; z <= 31; z++) {
            meas_item->label_info_lst[idx++] = fill_distribution_bin_label(x, y, z);
          }
        }
      }
    } else {
      meas_item->label_info_lst_len = 1;
      meas_item->label_info_lst = ecalloc(meas_item->label_info_lst_len, sizeof(label_info_lst_t));
      meas_item->label_info_lst[0] = fill_kpm_label();
    }
  }

  // 8.3.8 [0, 4294967295]
  act_def.frm_1.gran_period_ms = period_ms;

  // 8.3.20 - OPTIONAL
  act_def.frm_1.cell_global_id = NULL;

#if defined KPM_V2_03 || defined KPM_V3_00
  // [0, 65535]
  act_def.frm_1.meas_bin_range_info_lst_len = 0;
  act_def.frm_1.meas_bin_info_lst = NULL;
#endif

  return act_def;
}

typedef kpm_act_def_t (*fill_kpm_act_def)(ric_report_style_item_t const* report_item);

static
fill_kpm_act_def get_kpm_act_def[END_RIC_SERVICE_REPORT] = {
    fill_report_style_1,
    NULL,
    NULL,
    fill_report_style_4,
    NULL,
};

static
kpm_sub_data_t gen_kpm_subs(kpm_ran_function_def_t const* ran_func, ric_report_style_item_t const* report_item)
{
  assert(ran_func != NULL);
  assert(ran_func->ric_event_trigger_style_list != NULL);

  kpm_sub_data_t kpm_sub = {0};

  // Generate Event Trigger
  assert(ran_func->ric_event_trigger_style_list[0].format_type == FORMAT_1_RIC_EVENT_TRIGGER);
  kpm_sub.ev_trg_def.type = FORMAT_1_RIC_EVENT_TRIGGER;
  kpm_sub.ev_trg_def.kpm_ric_event_trigger_format_1.report_period_ms = period_ms;

  // Generate Action Definition
  kpm_sub.sz_ad = 1;
  kpm_sub.ad = calloc(kpm_sub.sz_ad, sizeof(kpm_act_def_t));
  assert(kpm_sub.ad != NULL && "Memory exhausted");

  // Multiple Action Definitions in one SUBSCRIPTION message is not supported in this project
  // Multiple REPORT Styles = Multiple Action Definition = Multiple SUBSCRIPTION messages
  ric_service_report_e const report_style_type = report_item->report_style_type;
  *kpm_sub.ad = get_kpm_act_def[report_style_type](report_item);

  return kpm_sub;
}

static
bool eq_sm(sm_ran_function_t const* elem, int const id)
{
  if (elem->id == id)
    return true;

  return false;
}

static
size_t find_sm_idx(sm_ran_function_t* rf, size_t sz, bool (*f)(sm_ran_function_t const*, int const), int const id)
{
  for (size_t i = 0; i < sz; i++) {
    if (f(&rf[i], id))
      return i;
  }

  assert(0 != 0 && "SM ID could not be found in the RAN Function List");
  return 0;
}

int main(int argc, char* argv[])
{
  fr_args_t args = init_fr_args(argc, argv);

  // Init the xApp
  init_xapp_api(&args);
  sleep(1);

  init_kpm_meas_unit_hash_table();
  init_kpm_db();

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
  defer({ free_e2_node_arr_xapp(&nodes); });

  assert(nodes.len > 0);

  printf("Connected E2 nodes = %d\n", nodes.len);

  pthread_mutexattr_t attr = {0};
  int rc = pthread_mutex_init(&mtx, &attr);
  assert(rc == 0);

  sm_ans_xapp_t** hndl = (sm_ans_xapp_t**)calloc(nodes.len, sizeof(sm_ans_xapp_t*));
  assert(hndl != NULL);

  ////////////
  // START KPM
  ////////////
  int const KPM_ran_function = 2;

  for (size_t i = 0; i < nodes.len; ++i) {
    e2_node_connected_xapp_t* n = &nodes.n[i];

    size_t const idx = find_sm_idx(n->rf, n->len_rf, eq_sm, KPM_ran_function);
    assert(n->rf[idx].defn.type == KPM_RAN_FUNC_DEF_E && "KPM is not the received RAN Function");
    // if REPORT Service is supported by E2 node, send SUBSCRIPTION
    // e.g. OAI CU-CP
    const size_t sz_report_styles = n->rf[idx].defn.kpm.sz_ric_report_style_list;
    hndl[i] = calloc(sz_report_styles, sizeof(sm_ans_xapp_t));
    assert(hndl[i] != NULL);
    for (size_t j = 0; j < sz_report_styles; j++) {
      ric_report_style_item_t *report_item = &n->rf[idx].defn.kpm.ric_report_style_list[j];
      // Generate KPM SUBSCRIPTION message
      kpm_sub_data_t kpm_sub = gen_kpm_subs(&n->rf[idx].defn.kpm, report_item);

      hndl[i][j] = report_sm_xapp_api(&n->id, KPM_ran_function, &kpm_sub, sm_cb_kpm);
      assert(hndl[i][j].success == true);

      free_kpm_sub_data(&kpm_sub);
    }
  }
  ////////////
  // END KPM
  ////////////

  xapp_wait_end_api();

  for (int i = 0; i < nodes.len; ++i) {
    e2_node_connected_xapp_t* n = &nodes.n[i];
    size_t const idx = find_sm_idx(n->rf, n->len_rf, eq_sm, KPM_ran_function);
    for (size_t j = 0; j < n->rf[idx].defn.kpm.sz_ric_report_style_list; j++) {
      // Remove the handle previously returned
      if (hndl[i][j].success == true)
        rm_report_sm_xapp_api(hndl[i][j].u.handle);
    }
    free(hndl[i]);
  }
  free(hndl);

  free_kpm_meas_unit_hash_table();
  if (kpm_db != NULL)
    sqlite3_close(kpm_db);

  // Stop the xApp
  while (try_stop_xapp_api() == false)
    usleep(1000);

  printf("Test xApp run SUCCESSFULLY\n");
}
