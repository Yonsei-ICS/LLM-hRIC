#include "../../../../src/sm/rc_sm/ie/rc_data_ie.h"
#include "../../../../src/sm/rc_sm/ie/ir/lst_ran_param.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_list.h"
#include "../../../../src/sm/rc_sm/ie/ir/ran_param_struct.h"
#include "../../../../src/sm/rc_sm/rc_sm_id.h"
#include "../../../../src/util/alg_ds/alg/defer.h"
#include "../../../../src/xApp/e42_xapp_api.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

enum {
  RC_CTRL_STYLE_RADIO_RESOURCE_ALLOCATION = 2,
  RC_CTRL_ACTION_SLICE_LEVEL_PRB_QUOTA = 6,
  RC_SLICE_PARAM_RRM_POLICY_RATIO_LIST = 1,
  RC_SLICE_PARAM_RRM_POLICY = 3,
  RC_SLICE_PARAM_RRM_POLICY_MEMBER_LIST = 4,
  RC_SLICE_PARAM_PLMN_IDENTITY = 6,
  RC_SLICE_PARAM_S_NSSAI = 7,
  RC_SLICE_PARAM_SST = 8,
  RC_SLICE_PARAM_SD = 9,
  RC_SLICE_PARAM_MIN_PRB_POLICY_RATIO = 10,
  RC_SLICE_PARAM_MAX_PRB_POLICY_RATIO = 11,
  RC_SLICE_PARAM_DEDICATED_PRB_POLICY_RATIO = 12,
};

typedef struct {
  char plmn[8];
  char sst[8];
  char sd[16];
  int min_prb;
  int max_prb;
  int dedicated_prb;
} slice_policy_cfg_t;

static slice_policy_cfg_t *policies;
static size_t num_policies;

static void reset_policies(void)
{
  free(policies);
  policies = NULL;
  num_policies = 0;
}

static int64_t wallclock_ms(void)
{
  struct timespec ts = {0};
  clock_gettime(CLOCK_REALTIME, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static ran_parameter_value_t *make_int_value(int64_t value)
{
  ran_parameter_value_t *v = calloc(1, sizeof(*v));
  assert(v != NULL);
  v->type = INTEGER_RAN_PARAMETER_VALUE;
  v->int_ran = value;
  return v;
}

static ran_parameter_value_t *make_octet_value(const char *value)
{
  ran_parameter_value_t *v = calloc(1, sizeof(*v));
  assert(v != NULL);
  v->type = OCTET_STRING_RAN_PARAMETER_VALUE;
  v->octet_str_ran = cp_str_to_ba(value);
  return v;
}

static seq_ran_param_t make_int_param(uint32_t id, int64_t value)
{
  seq_ran_param_t p = {0};
  p.ran_param_id = id;
  p.ran_param_val.type = ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE;
  p.ran_param_val.flag_false = make_int_value(value);
  return p;
}

static seq_ran_param_t make_octet_param(uint32_t id, const char *value)
{
  seq_ran_param_t p = {0};
  p.ran_param_id = id;
  p.ran_param_val.type = ELEMENT_KEY_FLAG_FALSE_RAN_PARAMETER_VAL_TYPE;
  p.ran_param_val.flag_false = make_octet_value(value);
  return p;
}

static seq_ran_param_t make_struct_param(uint32_t id, size_t len)
{
  seq_ran_param_t p = {0};
  p.ran_param_id = id;
  p.ran_param_val.type = STRUCTURE_RAN_PARAMETER_VAL_TYPE;
  p.ran_param_val.strct = calloc(1, sizeof(*p.ran_param_val.strct));
  assert(p.ran_param_val.strct != NULL);
  p.ran_param_val.strct->sz_ran_param_struct = len;
  p.ran_param_val.strct->ran_param_struct = calloc(len, sizeof(*p.ran_param_val.strct->ran_param_struct));
  assert(p.ran_param_val.strct->ran_param_struct != NULL);
  return p;
}

static seq_ran_param_t make_list_param(uint32_t id, size_t len)
{
  seq_ran_param_t p = {0};
  p.ran_param_id = id;
  p.ran_param_val.type = LIST_RAN_PARAMETER_VAL_TYPE;
  p.ran_param_val.lst = calloc(1, sizeof(*p.ran_param_val.lst));
  assert(p.ran_param_val.lst != NULL);
  p.ran_param_val.lst->sz_lst_ran_param = len;
  p.ran_param_val.lst->lst_ran_param = calloc(len, sizeof(*p.ran_param_val.lst->lst_ran_param));
  assert(p.ran_param_val.lst->lst_ran_param != NULL);
  return p;
}

static lst_ran_param_t make_slice_policy_group(const slice_policy_cfg_t *cfg)
{
  lst_ran_param_t group = {0};
  group.ran_param_struct.sz_ran_param_struct = 4;
  group.ran_param_struct.ran_param_struct = calloc(4, sizeof(*group.ran_param_struct.ran_param_struct));
  assert(group.ran_param_struct.ran_param_struct != NULL);

  seq_ran_param_t rrm_policy = make_struct_param(RC_SLICE_PARAM_RRM_POLICY, 1);
  seq_ran_param_t member_list = make_list_param(RC_SLICE_PARAM_RRM_POLICY_MEMBER_LIST, 1);
  lst_ran_param_t *member = &member_list.ran_param_val.lst->lst_ran_param[0];
  member->ran_param_struct.sz_ran_param_struct = 2;
  member->ran_param_struct.ran_param_struct = calloc(2, sizeof(*member->ran_param_struct.ran_param_struct));
  assert(member->ran_param_struct.ran_param_struct != NULL);

  member->ran_param_struct.ran_param_struct[0] = make_octet_param(RC_SLICE_PARAM_PLMN_IDENTITY, cfg->plmn);
  seq_ran_param_t s_nssai = make_struct_param(RC_SLICE_PARAM_S_NSSAI, 2);
  s_nssai.ran_param_val.strct->ran_param_struct[0] = make_octet_param(RC_SLICE_PARAM_SST, cfg->sst);
  s_nssai.ran_param_val.strct->ran_param_struct[1] = make_octet_param(RC_SLICE_PARAM_SD, cfg->sd);
  member->ran_param_struct.ran_param_struct[1] = s_nssai;

  rrm_policy.ran_param_val.strct->ran_param_struct[0] = member_list;
  group.ran_param_struct.ran_param_struct[0] = rrm_policy;
  group.ran_param_struct.ran_param_struct[1] = make_int_param(RC_SLICE_PARAM_MIN_PRB_POLICY_RATIO, cfg->min_prb);
  group.ran_param_struct.ran_param_struct[2] = make_int_param(RC_SLICE_PARAM_MAX_PRB_POLICY_RATIO, cfg->max_prb);
  group.ran_param_struct.ran_param_struct[3] =
      make_int_param(RC_SLICE_PARAM_DEDICATED_PRB_POLICY_RATIO, cfg->dedicated_prb);
  return group;
}

static rc_ctrl_req_data_t gen_rc_slice_ctrl(void)
{
  rc_ctrl_req_data_t ctrl = {0};
  ctrl.hdr.format = FORMAT_1_E2SM_RC_CTRL_HDR;
  ctrl.hdr.frmt_1.ue_id.type = GNB_UE_ID_E2SM;
  ctrl.hdr.frmt_1.ric_style_type = RC_CTRL_STYLE_RADIO_RESOURCE_ALLOCATION;
  ctrl.hdr.frmt_1.ctrl_act_id = RC_CTRL_ACTION_SLICE_LEVEL_PRB_QUOTA;
  ctrl.hdr.frmt_1.ric_ctrl_decision = NULL;

  ctrl.msg.format = FORMAT_1_E2SM_RC_CTRL_MSG;
  ctrl.msg.frmt_1.sz_ran_param = 1;
  ctrl.msg.frmt_1.ran_param = calloc(1, sizeof(*ctrl.msg.frmt_1.ran_param));
  assert(ctrl.msg.frmt_1.ran_param != NULL);

  ctrl.msg.frmt_1.ran_param[0] = make_list_param(RC_SLICE_PARAM_RRM_POLICY_RATIO_LIST, num_policies);
  for (size_t i = 0; i < num_policies; i++)
    ctrl.msg.frmt_1.ran_param[0].ran_param_val.lst->lst_ran_param[i] = make_slice_policy_group(&policies[i]);

  return ctrl;
}

static void usage(const char *prog)
{
  fprintf(stderr,
          "Usage: %s (--policy-file <path> [--once] | --serve <socket>) "
          "[-c flexric.conf] [-p sm_lib_dir] [-a near_ric_ip] [-d db_dir] [-n db_name]\n",
          prog);
}

static char *read_text_file(const char *path)
{
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    fprintf(stderr, "Could not open policy file %s: %s\n", path, strerror(errno));
    return NULL;
  }
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long len = ftell(f);
  if (len < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char *buf = calloc((size_t)len + 1, 1);
  if (buf == NULL) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)len, f);
  fclose(f);
  if (n != (size_t)len) {
    free(buf);
    return NULL;
  }
  return buf;
}

static const char *skip_ws(const char *p)
{
  while (*p != '\0' && isspace((unsigned char)*p))
    p++;
  return p;
}

static const char *find_json_key(const char *start, const char *end, const char *key)
{
  char needle[64];
  int n = snprintf(needle, sizeof(needle), "\"%s\"", key);
  assert(n > 0 && (size_t)n < sizeof(needle));
  const char *p = start;
  while ((p = strstr(p, needle)) != NULL && p < end) {
    const char *colon = skip_ws(p + n);
    if (*colon == ':')
      return colon + 1;
    p += n;
  }
  return NULL;
}

static bool parse_json_string(const char *start, const char *end, const char *key, char *out, size_t out_len)
{
  const char *p = find_json_key(start, end, key);
  if (p == NULL)
    return false;
  p = skip_ws(p);
  if (*p != '"')
    return false;
  p++;
  const char *q = p;
  while (q < end && *q != '"' && *q != '\\')
    q++;
  if (q >= end || *q != '"' || (size_t)(q - p) >= out_len)
    return false;
  memcpy(out, p, q - p);
  out[q - p] = '\0';
  return true;
}

static bool parse_json_int(const char *start, const char *end, const char *key, int *out)
{
  const char *p = find_json_key(start, end, key);
  if (p == NULL)
    return false;
  p = skip_ws(p);
  char *q = NULL;
  errno = 0;
  long v = strtol(p, &q, 0);
  if (errno != 0 || q == p || q > end || v < INT32_MIN || v > INT32_MAX)
    return false;
  *out = (int)v;
  return true;
}

static bool parse_json_string_or_int(const char *start, const char *end, const char *key, char *out, size_t out_len)
{
  if (parse_json_string(start, end, key, out, out_len))
    return true;
  int value = 0;
  if (!parse_json_int(start, end, key, &value))
    return false;
  int n = snprintf(out, out_len, "%d", value);
  return n > 0 && (size_t)n < out_len;
}

static bool parse_uint_text(const char *text, unsigned long max, unsigned long *out)
{
  char *end = NULL;
  errno = 0;
  unsigned long v = strtoul(text, &end, 0);
  if (errno != 0 || end == text || *end != '\0' || v > max)
    return false;
  *out = v;
  return true;
}

static bool validate_policy(const slice_policy_cfg_t *p)
{
  unsigned long sst = 0;
  unsigned long sd = 0;
  if (strlen(p->plmn) < 5 || strlen(p->plmn) > 6)
    return false;
  for (const char *c = p->plmn; *c != '\0'; c++)
    if (!isdigit((unsigned char)*c))
      return false;
  if (!parse_uint_text(p->sst, UINT8_MAX, &sst) || !parse_uint_text(p->sd, 0xffffff, &sd))
    return false;
  if (p->min_prb < 0 || p->min_prb > 100 || p->max_prb < 0 || p->max_prb > 100 || p->dedicated_prb < 0
      || p->dedicated_prb > 100)
    return false;
  return p->min_prb <= p->dedicated_prb && p->dedicated_prb <= p->max_prb;
}

static const char *find_matching_brace(const char *open)
{
  int depth = 0;
  bool in_str = false;
  for (const char *p = open; *p != '\0'; p++) {
    if (*p == '"' && (p == open || p[-1] != '\\')) {
      in_str = !in_str;
      continue;
    }
    if (in_str)
      continue;
    if (*p == '{')
      depth++;
    else if (*p == '}') {
      depth--;
      if (depth == 0)
        return p;
    }
  }
  return NULL;
}

static bool load_policy_file(const char *path)
{
  reset_policies();
  char *json = read_text_file(path);
  if (json == NULL)
    return false;
  const char *list = strstr(json, "\"policies\"");
  if (list == NULL) {
    fprintf(stderr, "Policy file must contain a top-level \"policies\" array\n");
    free(json);
    return false;
  }
  const char *array = strchr(list, '[');
  if (array == NULL) {
    free(json);
    return false;
  }

  size_t cap = 4;
  policies = calloc(cap, sizeof(*policies));
  if (policies == NULL) {
    free(json);
    return false;
  }

  int dedicated_sum = 0;
  for (const char *p = array + 1; (p = strchr(p, '{')) != NULL;) {
    const char *end = find_matching_brace(p);
    if (end == NULL) {
      free(json);
      return false;
    }
    if (num_policies == cap) {
      cap *= 2;
      slice_policy_cfg_t *new_policies = realloc(policies, cap * sizeof(*policies));
      if (new_policies == NULL) {
        free(json);
        return false;
      }
      policies = new_policies;
      memset(&policies[num_policies], 0, (cap - num_policies) * sizeof(*policies));
    }

    slice_policy_cfg_t cfg = {0};
    if (!parse_json_string(p, end, "plmn", cfg.plmn, sizeof(cfg.plmn))
        || !parse_json_string_or_int(p, end, "sst", cfg.sst, sizeof(cfg.sst))
        || !parse_json_string_or_int(p, end, "sd", cfg.sd, sizeof(cfg.sd))
        || !parse_json_int(p, end, "min_prb", &cfg.min_prb)
        || !parse_json_int(p, end, "max_prb", &cfg.max_prb)
        || !parse_json_int(p, end, "dedicated_prb", &cfg.dedicated_prb)
        || !validate_policy(&cfg)) {
      fprintf(stderr, "Invalid slice policy entry near %.32s\n", p);
      free(json);
      return false;
    }
    dedicated_sum += cfg.dedicated_prb;
    if (dedicated_sum > 100) {
      fprintf(stderr, "Invalid slice policy: sum of dedicated_prb exceeds 100\n");
      free(json);
      return false;
    }
    policies[num_policies++] = cfg;
    p = end + 1;
  }

  free(json);
  if (num_policies == 0) {
    fprintf(stderr, "Policy file contains no policies\n");
    return false;
  }
  return true;
}

static void build_flexric_args(int argc, char *argv[], int *fr_argc, char *fr_argv[])
{
  fr_argv[0] = argv[0];
  *fr_argc = 1;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-d") == 0
        || strcmp(argv[i], "-n") == 0) {
      if (i + 1 >= argc) {
        usage(argv[0]);
        exit(EXIT_FAILURE);
      }
      fr_argv[(*fr_argc)++] = argv[i];
      fr_argv[(*fr_argc)++] = argv[++i];
    }
  }
}

static uint16_t find_rc_ran_function_id(const e2_node_connected_xapp_t *node)
{
  for (size_t i = 0; i < node->len_rf; i++) {
    if (node->rf[i].id == SM_RC_ID || node->rf[i].defn.type == RC_RAN_FUNC_DEF_E)
      return node->rf[i].id;
  }
  return 0;
}

static bool send_slice_controls(const e2_node_arr_xapp_t *nodes)
{
  bool sent = false;
  bool success = true;
  for (size_t i = 0; i < nodes->len; i++) {
    e2_node_connected_xapp_t *node = &nodes->n[i];
    uint16_t rc_ran_function = find_rc_ran_function_id(node);
    if (rc_ran_function == 0) {
      printf("Node %zu has no ORAN-E2SM-RC RAN function, skipping\n", i);
      continue;
    }

    rc_ctrl_req_data_t ctrl = gen_rc_slice_ctrl();
    sm_ans_xapp_t ans = control_sm_xapp_api(&node->id, rc_ran_function, &ctrl);
    free_rc_ctrl_req_data(&ctrl);
    printf("RC Slice-level PRB quota control sent to node %zu via ran_func_id %u: success=%d\n",
           i,
           rc_ran_function,
           ans.success);
    sent = true;
    success = success && ans.success;
  }
  return sent && success;
}

static int serve_requests(const char *socket_path, const e2_node_arr_xapp_t *nodes)
{
  if (strlen(socket_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    fprintf(stderr, "Unix socket path is too long: %s\n", socket_path);
    return EXIT_FAILURE;
  }
  int server = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server < 0) {
    perror("socket");
    return EXIT_FAILURE;
  }
  struct sockaddr_un addr = {.sun_family = AF_UNIX};
  strcpy(addr.sun_path, socket_path);
  unlink(socket_path);
  if (bind(server, (const struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(server, 16) != 0) {
    perror("bind/listen");
    close(server);
    return EXIT_FAILURE;
  }
  printf("RC slice actuator listening on %s\n", socket_path);
  fflush(stdout);

  for (;;) {
    int client = accept(server, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      perror("accept");
      break;
    }
    char request[4096] = {0};
    ssize_t len = recv(client, request, sizeof(request) - 1, 0);
    char request_id[96] = {0};
    char policy_file[1024] = {0};
    bool parsed = len > 0
                  && parse_json_string(request, request + len, "request_id", request_id, sizeof(request_id))
                  && parse_json_string(request, request + len, "policy_file", policy_file, sizeof(policy_file));
    int64_t sent_ts_ms = wallclock_ms();
    bool success = parsed && load_policy_file(policy_file) && send_slice_controls(nodes);
    int64_t response_ts_ms = wallclock_ms();
    if (!parsed)
      snprintf(request_id, sizeof(request_id), "invalid-request");
    dprintf(client,
            "{\"request_id\":\"%s\",\"success\":%s,\"sent_ts_ms\":%" PRId64
            ",\"response_ts_ms\":%" PRId64 "%s}\n",
            request_id,
            success ? "true" : "false",
            sent_ts_ms,
            response_ts_ms,
            success ? "" : ",\"error\":\"RC control failed or request was invalid\"");
    close(client);
    fflush(stdout);
  }
  close(server);
  unlink(socket_path);
  return EXIT_FAILURE;
}

int main(int argc, char *argv[])
{
  const char *policy_file = NULL;
  const char *serve_path = NULL;
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "--policy-file") == 0) {
      if (++i >= argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
      }
      policy_file = argv[i];
    } else if (strcmp(argv[i], "--serve") == 0) {
      if (++i >= argc) {
        usage(argv[0]);
        return EXIT_FAILURE;
      }
      serve_path = argv[i];
    } else if (strcmp(argv[i], "--once") == 0) {
      continue;
    } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "-a") == 0 || strcmp(argv[i], "-d") == 0
               || strcmp(argv[i], "-n") == 0) {
      i++;
    } else {
      usage(argv[0]);
      return EXIT_FAILURE;
    }
  }
  if ((policy_file == NULL) == (serve_path == NULL)) {
    usage(argv[0]);
    return EXIT_FAILURE;
  }
  if (policy_file != NULL && !load_policy_file(policy_file))
    return EXIT_FAILURE;

  char *fr_argv[12] = {0};
  int fr_argc = 0;
  build_flexric_args(argc, argv, &fr_argc, fr_argv);
  fr_args_t args = init_fr_args(fr_argc, fr_argv);
  init_xapp_api(&args);
  sleep(1);

  e2_node_arr_xapp_t nodes = e2_nodes_xapp_api();
  defer({ free_e2_node_arr_xapp(&nodes); });

  if (nodes.len == 0) {
    fprintf(stderr, "No E2 nodes are connected\n");
    return EXIT_FAILURE;
  }
  printf("Connected E2 nodes len = %d\n", nodes.len);

  if (serve_path != NULL)
    return serve_requests(serve_path, &nodes);

  printf("Sending %zu slice PRB policies from %s\n", num_policies, policy_file);
  bool success = send_slice_controls(&nodes);

  while (try_stop_xapp_api() == false)
    usleep(1000);
  reset_policies();
  return success ? EXIT_SUCCESS : EXIT_FAILURE;
}
