/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

/*
 * \brief xnap mobility management functions for gNB
 */

#include "xnap_gNB_mobility_management.h"
#include "xnap_lib_common.h"

/**
 * @brief XnAP security capabilities encoding
 */
static XNAP_UESecurityCapabilities_t xnap_encode_security_capabilities(const xnap_security_capabilities_t *in)
{
  DevAssert(in != NULL);

  XNAP_UESecurityCapabilities_t out = {0};
  ENCRALG_TO_BIT_STRING(in->nRencryption_algorithms, &out.nr_EncyptionAlgorithms);
  INTPROTALG_TO_BIT_STRING(in->nRintegrity_algorithms, &out.nr_IntegrityProtectionAlgorithms);
  ENCRALG_TO_BIT_STRING(in->eUTRAencryption_algorithms, &out.e_utra_EncyptionAlgorithms);
  INTPROTALG_TO_BIT_STRING(in->eUTRAintegrity_algorithms, &out.e_utra_IntegrityProtectionAlgorithms);

  return out;
}

/**
 * @brief XnAP security capabilities decoding
 */
static bool decode_xnap_ue_security_capabilities(const XNAP_UESecurityCapabilities_t *in, xnap_security_capabilities_t *out)
{
  DevAssert(in != NULL);
  DevAssert(out != NULL);

  out->nRencryption_algorithms = BIT_STRING_to_uint16(&in->nr_EncyptionAlgorithms);
  out->nRintegrity_algorithms = BIT_STRING_to_uint16(&in->nr_IntegrityProtectionAlgorithms);
  out->eUTRAencryption_algorithms = BIT_STRING_to_uint16(&in->e_utra_EncyptionAlgorithms);
  out->eUTRAintegrity_algorithms = BIT_STRING_to_uint16(&in->e_utra_IntegrityProtectionAlgorithms);

  return true;
}

/**
 * @brief XnAP UL NGU TNL information encoding
 */
static XNAP_UPTransportLayerInformation_t xnap_encode_ul_ngu_tnl_info(const gtpu_tunnel_t *in)
{
  DevAssert(in != NULL);

  XNAP_UPTransportLayerInformation_t out = {0};
  out.present = XNAP_UPTransportLayerInformation_PR_gtpTunnel;
  asn1cCalloc(out.choice.gtpTunnel, gtp);
  uint32_t addr = 0;
  memcpy(&addr, in->addr.buffer, sizeof(addr));
  TRANSPORT_LAYER_ADDRESS_IPv4_TO_BIT_STRING(addr, &gtp->tnl_address);
  INT32_TO_OCTET_STRING(in->teid, &gtp->gtp_teid);

  return out;
}

static bool decode_xnap_ul_ngu_tnl_info(const XNAP_UPTransportLayerInformation_t *in, gtpu_tunnel_t *out)
{
  DevAssert(in != NULL);
  DevAssert(out != NULL);

  if (in->present != XNAP_UPTransportLayerInformation_PR_gtpTunnel)
    return false;

  const XNAP_GTPtunnelTransportLayerInformation_t *gtp = in->choice.gtpTunnel;
  DevAssert(gtp != NULL);

  BIT_STRING_TO_TRANSPORT_LAYER_ADDRESS_IPv4(&gtp->tnl_address, *(uint32_t *)out->addr.buffer);
  out->addr.length = 4;
  OCTET_STRING_TO_INT32(&gtp->gtp_teid, out->teid);

  return true;
}

static bool decode_xnap_ue_history_information(const XNAP_UEHistoryInformation_t *in, xnap_handover_req_t *out)
{
  DevAssert(in != NULL);
  DevAssert(out != NULL);

  out->num_last_visited_cells = in->list.count;
  DevAssert(out->num_last_visited_cells > 0);

  out->ue_history_info = calloc(out->num_last_visited_cells, sizeof(*out->ue_history_info));
  DevAssert(out->ue_history_info != NULL);

  for (int i = 0; i < out->num_last_visited_cells; i++) {
    XNAP_LastVisitedCell_Item_t *lv = in->list.array[i];
    if (lv == NULL)
      continue;

    if (lv->present != XNAP_LastVisitedCell_Item_PR_nG_RAN_Cell)
      continue;

    XNAP_LastVisitedNGRANCellInformation_t *nr = &lv->choice.nG_RAN_Cell;
    ue_history_info_t *cell = &out->ue_history_info[i];

    cell->xnap_cell_type = XNAP_LastVisitedCell_Item_PR_nG_RAN_Cell;
    cell->last_visited_cell_info = create_byte_array(nr->size, nr->buf);
  }

  return true;
}

static bool decode_xnap_qos_flows_to_be_setup_list(const XNAP_QoSFlowsToBeSetup_List_t *in,
                                                   xnap_pdusession_resources_tobe_setup_item_t *out)
{
  DevAssert(in != NULL);
  DevAssert(out != NULL);

  out->num_qos = in->list.count;
  DevAssert(out->num_qos > 0);

  out->qos_list = calloc_or_fail(out->num_qos, sizeof(*out->qos_list));

  for (int q = 0; q < out->num_qos; q++) {
    XNAP_QoSFlowsToBeSetup_Item_t *qos = in->list.array[q];
    xnap_qos_flow_tobe_setup_item_t *qo = &out->qos_list[q];

    qo->qfi = qos->qfi;

    const XNAP_QoSFlowLevelQoSParameters_t *params = &qos->qosFlowLevelQoSParameters;

    qo->qos_params.arp.priority_level = params->allocationAndRetentionPrio.priorityLevel;
    qo->qos_params.arp.pre_emp_capability = params->allocationAndRetentionPrio.pre_emption_capability;
    qo->qos_params.arp.pre_emp_vulnerability = params->allocationAndRetentionPrio.pre_emption_vulnerability;

    const XNAP_QoSCharacteristics_t *qchar = &params->qos_characteristics;

    if (qchar->present == XNAP_QoSCharacteristics_PR_non_dynamic) {
      qo->qos_params.qos_type = NON_DYNAMIC;
      qo->qos_params.nondyn.fiveQI = qchar->choice.non_dynamic->fiveQI;
    } else {
      const XNAP_Dynamic5QIDescriptor_t *dyn = qchar->choice.dynamic;

      qo->qos_params.qos_type = DYNAMIC;
      qo->qos_params.dyn.prio = dyn->priorityLevelQoS;
      qo->qos_params.dyn.pdb = dyn->packetDelayBudget;
      qo->qos_params.dyn.per.scalar = dyn->packetErrorRate.pER_Scalar;
      qo->qos_params.dyn.per.exponent = dyn->packetErrorRate.pER_Exponent;
    }
  }

  return true;
}

/**
 * @brief XnAP Handover Request encoding
 */
XNAP_XnAP_PDU_t *encode_xnap_handover_request(xnap_handover_req_t *req)
{
  XNAP_XnAP_PDU_t *pdu = calloc_or_fail(1, sizeof(*pdu));

  /* Message type */
  pdu->present = XNAP_XnAP_PDU_PR_initiatingMessage;
  asn1cCalloc(pdu->choice.initiatingMessage, initMsg);
  initMsg->procedureCode = XNAP_ProcedureCode_id_handoverPreparation;
  initMsg->criticality = XNAP_Criticality_reject;
  initMsg->value.present = XNAP_InitiatingMessage__value_PR_HandoverRequest;

  XNAP_HandoverRequest_t *out = &initMsg->value.choice.HandoverRequest;

  /* Source NG-RAN node UE XnAP ID (M) */
  asn1cSequenceAdd(out->protocolIEs.list, XNAP_HandoverRequest_IEs_t, ie1);
  ie1->id = XNAP_ProtocolIE_ID_id_sourceNG_RANnodeUEXnAPID;
  ie1->criticality = XNAP_Criticality_reject;
  ie1->value.present = XNAP_HandoverRequest_IEs__value_PR_NG_RANnodeUEXnAPID;
  ie1->value.choice.NG_RANnodeUEXnAPID = req->s_ng_node_ue_xnap_id;

  /* Cause (M) */
  asn1cSequenceAdd(out->protocolIEs.list, XNAP_HandoverRequest_IEs_t, ie2);
  ie2->id = XNAP_ProtocolIE_ID_id_Cause;
  ie2->criticality = XNAP_Criticality_reject;
  ie2->value.present = XNAP_HandoverRequest_IEs__value_PR_Cause;
  xnap_gNB_set_cause(&ie2->value.choice.Cause, &req->cause);

  /* Target Cell Global ID (M) */
  asn1cSequenceAdd(out->protocolIEs.list, XNAP_HandoverRequest_IEs_t, ie3);
  ie3->id = XNAP_ProtocolIE_ID_id_targetCellGlobalID;
  ie3->criticality = XNAP_Criticality_reject;
  ie3->value.present = XNAP_HandoverRequest_IEs__value_PR_Target_CGI;
  ie3->value.choice.Target_CGI = xnap_encode_target_cgi(&req->target_cgi);

  /* GUAMI (M) */
  asn1cSequenceAdd(out->protocolIEs.list, XNAP_HandoverRequest_IEs_t, ie4);
  ie4->id = XNAP_ProtocolIE_ID_id_GUAMI;
  ie4->criticality = XNAP_Criticality_reject;
  ie4->value.present = XNAP_HandoverRequest_IEs__value_PR_GUAMI;
  MCC_MNC_TO_PLMNID(req->guami.plmn.mcc, req->guami.plmn.mnc, req->guami.plmn.mnc_digit_length, &ie4->value.choice.GUAMI.plmn_ID);
  AMF_REGION_TO_BIT_STRING(req->guami.amf_region_id, &ie4->value.choice.GUAMI.amf_region_id);
  AMF_SETID_TO_BIT_STRING(req->guami.amf_set_id, &ie4->value.choice.GUAMI.amf_set_id);
  AMF_POINTER_TO_BIT_STRING(req->guami.amf_pointer, &ie4->value.choice.GUAMI.amf_pointer);

  /* UE Context Information (M) */
  asn1cSequenceAdd(out->protocolIEs.list, XNAP_HandoverRequest_IEs_t, ie5);
  ie5->id = XNAP_ProtocolIE_ID_id_UEContextInfoHORequest;
  ie5->criticality = XNAP_Criticality_reject;
  ie5->value.present = XNAP_HandoverRequest_IEs__value_PR_UEContextInfoHORequest;
  XNAP_UEContextInfoHORequest_t *ctx = &ie5->value.choice.UEContextInfoHORequest;

  /* NG-C UE associated Signalling reference */
  asn_uint642INTEGER(&ctx->ng_c_UE_reference, req->ue_context.ngc_ue_sig_ref);

  /* Signalling TNL association address at source NG-C side */
  ctx->cp_TNL_info_source.present = XNAP_CPTransportLayerInformation_PR_endpointIPAddress;
  TRANSPORT_LAYER_ADDRESS_IPv4_TO_BIT_STRING(*(long *)req->ue_context.cp_tnl_ip_source.buffer,
                                             &ctx->cp_TNL_info_source.choice.endpointIPAddress);

  /* UE Security Capabilities */
  const xnap_security_capabilities_t *s_cap = &req->ue_context.security_capabilities;
  ctx->ueSecurityCapabilities = xnap_encode_security_capabilities(s_cap);

  /* AS Security Information */
  KGNB_STAR_TO_BIT_STRING(req->ue_context.as_security_key_ranstar, &ctx->securityInformation.key_NG_RAN_Star);
  ctx->securityInformation.ncc = req->ue_context.as_security_ncc;

  /* RRC Context (Handover Preparation Information) */
  OCTET_STRING_fromBuf(&ctx->rrc_Context, (const char *)req->ue_context.rrc_context.buf, req->ue_context.rrc_context.len);

  /* UE Aggregate Maximum Bit Rate */
  UEAGMAXBITRTD_TO_ASN_PRIMITIVES(req->ue_context.ue_ambr.br_dl, &ctx->ue_AMBR.dl_UE_AMBR);
  UEAGMAXBITRTU_TO_ASN_PRIMITIVES(req->ue_context.ue_ambr.br_ul, &ctx->ue_AMBR.ul_UE_AMBR);

  /* PDU Session Resources To Be Setup List */
  for (int i = 0; i < req->ue_context.num_pdu; i++) {
    const xnap_pdusession_resources_tobe_setup_item_t *pdu = &req->ue_context.pdusession_resources_tobe_setup_list[i];
    asn1cSequenceAdd(ctx->pduSessionResourcesToBeSetup_List.list, XNAP_PDUSessionResourcesToBeSetup_Item_t, pduItem);

    /* PDU Session ID */
    pduItem->pduSessionId = pdu->pdusession_id;

    /* S-NSSAI */
    pduItem->s_NSSAI = xnap_encode_snssai(pdu->nssai);

    /* UL NG-U TNL Information */
    pduItem->uL_NG_U_TNLatUPF.present = XNAP_UPTransportLayerInformation_PR_gtpTunnel;
    pduItem->uL_NG_U_TNLatUPF = xnap_encode_ul_ngu_tnl_info(&pdu->n3_incoming);

    /* PDU Session Type */
    pduItem->pduSessionType = pdu->pdu_session_type;

    /* QoS Flows To Be Setup */
    for (int j = 0; j < pdu->num_qos; j++) {
      const xnap_qos_flow_tobe_setup_item_t *qos = &pdu->qos_list[j];
      asn1cSequenceAdd(pduItem->qosFlowsToBeSetup_List.list, XNAP_QoSFlowsToBeSetup_Item_t, qosItem);

      qosItem->qfi = qos->qfi;

      XNAP_QoSCharacteristics_t *qosChar = &qosItem->qosFlowLevelQoSParameters.qos_characteristics;
      if (qos->qos_params.qos_type == NON_DYNAMIC) {
        qosChar->present = XNAP_QoSCharacteristics_PR_non_dynamic;
        asn1cCalloc(qosChar->choice.non_dynamic, nonDyn);
        nonDyn->fiveQI = qos->qos_params.nondyn.fiveQI;
      } else {
        qosChar->present = XNAP_QoSCharacteristics_PR_dynamic;
        asn1cCalloc(qosChar->choice.dynamic, dyn);
        dyn->priorityLevelQoS = qos->qos_params.dyn.prio;
        dyn->packetDelayBudget = qos->qos_params.dyn.pdb;
        dyn->packetErrorRate.pER_Scalar = qos->qos_params.dyn.per.scalar;
        dyn->packetErrorRate.pER_Exponent = qos->qos_params.dyn.per.exponent;
      }

      XNAP_AllocationandRetentionPriority_t *arp = &qosItem->qosFlowLevelQoSParameters.allocationAndRetentionPrio;
      arp->priorityLevel = qos->qos_params.arp.priority_level;
      arp->pre_emption_capability = qos->qos_params.arp.pre_emp_capability;
      arp->pre_emption_vulnerability = qos->qos_params.arp.pre_emp_vulnerability;
    }
  }

  /* UE History Information (M) */
  asn1cSequenceAdd(out->protocolIEs.list, XNAP_HandoverRequest_IEs_t, ie6);
  ie6->id = XNAP_ProtocolIE_ID_id_UEHistoryInformation;
  ie6->criticality = XNAP_Criticality_ignore;
  ie6->value.present = XNAP_HandoverRequest_IEs__value_PR_UEHistoryInformation;

  for (int i = 0; i < req->num_last_visited_cells; i++) {
    asn1cSequenceAdd(ie6->value.choice.UEHistoryInformation.list, XNAP_LastVisitedCell_Item_t, lastVisitedCellItem);
    if (req->ue_history_info[i].xnap_cell_type == XNAP_LastVisitedCell_Item_PR_nG_RAN_Cell) {
      lastVisitedCellItem->present = XNAP_LastVisitedCell_Item_PR_nG_RAN_Cell;
      XNAP_LastVisitedNGRANCellInformation_t *nrInfo = &lastVisitedCellItem->choice.nG_RAN_Cell;
      OCTET_STRING_fromBuf(nrInfo,
                           (const char *)req->ue_history_info[i].last_visited_cell_info.buf,
                           req->ue_history_info[i].last_visited_cell_info.len);
    }
  }

  return pdu;
}

/**
 * @brief XnAP Handover Request decoding
 */
bool decode_xnap_handover_request(xnap_handover_req_t *out, const XNAP_XnAP_PDU_t *pdu)
{
  /* Check message type */
  _EQ_CHECK_INT(pdu->present, XNAP_XnAP_PDU_PR_initiatingMessage);
  AssertError(pdu->choice.initiatingMessage != NULL, return false, "initiatingMessage is NULL");
  _EQ_CHECK_LONG(pdu->choice.initiatingMessage->procedureCode, XNAP_ProcedureCode_id_handoverPreparation);
  _EQ_CHECK_INT(pdu->choice.initiatingMessage->value.present, XNAP_InitiatingMessage__value_PR_HandoverRequest);

  XNAP_HandoverRequest_t *in = &pdu->choice.initiatingMessage->value.choice.HandoverRequest;
  XNAP_HandoverRequest_IEs_t *ie;

  /* Check presence of mandatory IEs */
  XNAP_LIB_FIND_IE(XNAP_HandoverRequest_IEs_t, ie, &in->protocolIEs.list, XNAP_ProtocolIE_ID_id_sourceNG_RANnodeUEXnAPID, true);
  XNAP_LIB_FIND_IE(XNAP_HandoverRequest_IEs_t, ie, &in->protocolIEs.list, XNAP_ProtocolIE_ID_id_Cause, true);
  XNAP_LIB_FIND_IE(XNAP_HandoverRequest_IEs_t, ie, &in->protocolIEs.list, XNAP_ProtocolIE_ID_id_targetCellGlobalID, true);
  XNAP_LIB_FIND_IE(XNAP_HandoverRequest_IEs_t, ie, &in->protocolIEs.list, XNAP_ProtocolIE_ID_id_GUAMI, true);
  XNAP_LIB_FIND_IE(XNAP_HandoverRequest_IEs_t, ie, &in->protocolIEs.list, XNAP_ProtocolIE_ID_id_UEContextInfoHORequest, true);
  XNAP_LIB_FIND_IE(XNAP_HandoverRequest_IEs_t, ie, &in->protocolIEs.list, XNAP_ProtocolIE_ID_id_UEHistoryInformation, true);

  /* Loop over all IEs */
  for (int i = 0; i < in->protocolIEs.list.count; i++) {
    DevAssert(in->protocolIEs.list.array[i]);
    ie = in->protocolIEs.list.array[i];

    switch (ie->id) {
      case XNAP_ProtocolIE_ID_id_sourceNG_RANnodeUEXnAPID: {
        _EQ_CHECK_INT(ie->value.present, XNAP_HandoverRequest_IEs__value_PR_NG_RANnodeUEXnAPID);
        out->s_ng_node_ue_xnap_id = ie->value.choice.NG_RANnodeUEXnAPID;
      } break;

      case XNAP_ProtocolIE_ID_id_Cause: {
        _EQ_CHECK_INT(ie->value.present, XNAP_HandoverRequest_IEs__value_PR_Cause);
        out->cause = decode_xnap_cause(&ie->value.choice.Cause);
      } break;

      case XNAP_ProtocolIE_ID_id_targetCellGlobalID: {
        _EQ_CHECK_INT(ie->value.present, XNAP_HandoverRequest_IEs__value_PR_Target_CGI);
        if (!xnap_decode_target_cgi(&ie->value.choice.Target_CGI, &out->target_cgi))
          return false;
      } break;

      case XNAP_ProtocolIE_ID_id_GUAMI: {
        _EQ_CHECK_INT(ie->value.present, XNAP_HandoverRequest_IEs__value_PR_GUAMI);
        const XNAP_GUAMI_t *guami = &ie->value.choice.GUAMI;
        plmn_id_t *plmn = &out->guami.plmn;
        PLMNID_TO_MCC_MNC(&guami->plmn_ID, plmn->mcc, plmn->mnc, plmn->mnc_digit_length);
        out->guami.amf_region_id = BIT_STRING_to_uint8(&guami->amf_region_id);
        out->guami.amf_set_id = BIT_STRING_to_uint16(&guami->amf_set_id);
        out->guami.amf_pointer = BIT_STRING_to_uint8(&guami->amf_pointer);
      } break;

      case XNAP_ProtocolIE_ID_id_UEContextInfoHORequest: {
        _EQ_CHECK_INT(ie->value.present, XNAP_HandoverRequest_IEs__value_PR_UEContextInfoHORequest);
        XNAP_UEContextInfoHORequest_t *ctx = &ie->value.choice.UEContextInfoHORequest;

        asn_INTEGER2uint64(&ctx->ng_c_UE_reference, &out->ue_context.ngc_ue_sig_ref);

        BIT_STRING_TO_TRANSPORT_LAYER_ADDRESS_IPv4(&ctx->cp_TNL_info_source.choice.endpointIPAddress,
                                                   *(long *)out->ue_context.cp_tnl_ip_source.buffer);
        out->ue_context.cp_tnl_ip_source.length = 4;

        asn_INTEGER2ulong(&ctx->ue_AMBR.ul_UE_AMBR, &out->ue_context.ue_ambr.br_ul);
        asn_INTEGER2ulong(&ctx->ue_AMBR.dl_UE_AMBR, &out->ue_context.ue_ambr.br_dl);

        xnap_security_capabilities_t *cap = &out->ue_context.security_capabilities;
        if (!decode_xnap_ue_security_capabilities(&ctx->ueSecurityCapabilities, cap))
          return false;

        memcpy(out->ue_context.as_security_key_ranstar, ctx->securityInformation.key_NG_RAN_Star.buf, 32);
        out->ue_context.as_security_ncc = ctx->securityInformation.ncc;

        out->ue_context.rrc_context = create_byte_array(ctx->rrc_Context.size, ctx->rrc_Context.buf);

        out->ue_context.num_pdu = ctx->pduSessionResourcesToBeSetup_List.list.count;
        out->ue_context.pdusession_resources_tobe_setup_list =
            calloc_or_fail(out->ue_context.num_pdu, sizeof(*out->ue_context.pdusession_resources_tobe_setup_list));

        for (int p = 0; p < out->ue_context.num_pdu; p++) {
          XNAP_PDUSessionResourcesToBeSetup_Item_t *pdu = ctx->pduSessionResourcesToBeSetup_List.list.array[p];
          xnap_pdusession_resources_tobe_setup_item_t *dst = &out->ue_context.pdusession_resources_tobe_setup_list[p];

          dst->pdusession_id = pdu->pduSessionId;

          dst->nssai = calloc_or_fail(1, sizeof(*dst->nssai));
          if (!decode_xnap_snssai(&pdu->s_NSSAI, dst->nssai))
            return false;

          if (!decode_xnap_ul_ngu_tnl_info(&pdu->uL_NG_U_TNLatUPF, &dst->n3_incoming))
            return false;

          dst->pdu_session_type = pdu->pduSessionType;

          if (!decode_xnap_qos_flows_to_be_setup_list(&pdu->qosFlowsToBeSetup_List, dst))
            return false;
        }
      } break;

      case XNAP_ProtocolIE_ID_id_UEHistoryInformation:
        if (!decode_xnap_ue_history_information(&ie->value.choice.UEHistoryInformation, out))
          return false;
        break;

      default:
        AssertError(0, return false, "Unknown XnAP IE id %ld\n", ie->id);
        break;
    }
  }
  return true;
}

static bool eq_xnap_qos_flow_param(const xnap_qos_flow_param_t *a, const xnap_qos_flow_param_t *b)
{
  _EQ_CHECK_INT(a->qos_type, b->qos_type);
  if (a->qos_type == NON_DYNAMIC) {
    _EQ_CHECK_INT(a->nondyn.fiveQI, b->nondyn.fiveQI);
  } else {
    DevAssert(a->qos_type == DYNAMIC);
    DevAssert(b->qos_type == DYNAMIC);
    const xnap_dynamic_5qi_t *adyn = &a->dyn, *bdyn = &b->dyn;
    _EQ_CHECK_INT(adyn->prio, bdyn->prio);
    _EQ_CHECK_INT(adyn->pdb, bdyn->pdb);
    _EQ_CHECK_INT(adyn->per.scalar, bdyn->per.scalar);
    _EQ_CHECK_INT(adyn->per.exponent, bdyn->per.exponent);
  }
  _EQ_CHECK_INT(a->arp.priority_level, b->arp.priority_level);
  _EQ_CHECK_INT(a->arp.pre_emp_capability, b->arp.pre_emp_capability);
  _EQ_CHECK_INT(a->arp.pre_emp_vulnerability, b->arp.pre_emp_vulnerability);
  return true;
}

static bool eq_xnap_qos_tobe_setup_item(const xnap_qos_flow_tobe_setup_item_t *a, const xnap_qos_flow_tobe_setup_item_t *b)
{
  _EQ_CHECK_INT(a->qfi, b->qfi);
  return eq_xnap_qos_flow_param(&a->qos_params, &b->qos_params);
}

static bool eq_xnap_pdusession_resources_tobe_setup_item(const xnap_pdusession_resources_tobe_setup_item_t *a,
                                                         const xnap_pdusession_resources_tobe_setup_item_t *b)
{
  _EQ_CHECK_INT(a->pdusession_id, b->pdusession_id);

  if (!eq_xnap_snssai(a->nssai, b->nssai))
    return false;

  if (!eq_gtpu_tunnel(&a->n3_incoming, &b->n3_incoming))
    return false;

  _EQ_CHECK_INT(a->pdu_session_type, b->pdu_session_type);
  _EQ_CHECK_INT(a->num_qos, b->num_qos);

  for (int i = 0; i < a->num_qos; i++) {
    if (!eq_xnap_qos_tobe_setup_item(&a->qos_list[i], &b->qos_list[i]))
      return false;
  }

  return true;
}

static bool eq_xnap_security_capabilities(const xnap_security_capabilities_t *a, const xnap_security_capabilities_t *b)
{
  _EQ_CHECK_INT(a->nRencryption_algorithms, b->nRencryption_algorithms);
  _EQ_CHECK_INT(a->nRintegrity_algorithms, b->nRintegrity_algorithms);
  _EQ_CHECK_INT(a->eUTRAencryption_algorithms, b->eUTRAencryption_algorithms);
  _EQ_CHECK_INT(a->eUTRAintegrity_algorithms, b->eUTRAintegrity_algorithms);
  return true;
}

static bool eq_xnap_ue_context_info(const xnap_ue_context_info_t *a, const xnap_ue_context_info_t *b)
{
  _EQ_CHECK_LONG(a->ngc_ue_sig_ref, b->ngc_ue_sig_ref);

  if (!eq_transport_layer_addr(&a->cp_tnl_ip_source, &b->cp_tnl_ip_source))
    return false;

  if (!eq_xnap_security_capabilities(&a->security_capabilities, &b->security_capabilities))
    return false;

  if (memcmp(a->as_security_key_ranstar, b->as_security_key_ranstar, 32) != 0) {
    PRINT_ERROR("XnAP Equality failed: as_security_key_ranstar differs\n");
    return false;
  }

  _EQ_CHECK_LONG(a->as_security_ncc, b->as_security_ncc);
  _EQ_CHECK_LONG(a->ue_ambr.br_ul, b->ue_ambr.br_ul);
  _EQ_CHECK_LONG(a->ue_ambr.br_dl, b->ue_ambr.br_dl);

  if (!eq_byte_array(&a->rrc_context, &b->rrc_context))
    return false;

  _EQ_CHECK_INT(a->num_pdu, b->num_pdu);

  for (int i = 0; i < a->num_pdu; i++) {
    if (!eq_xnap_pdusession_resources_tobe_setup_item(&a->pdusession_resources_tobe_setup_list[i],
                                                      &b->pdusession_resources_tobe_setup_list[i]))
      return false;
  }

  return true;
}

static bool eq_xnap_ue_history_info(const ue_history_info_t *a, const ue_history_info_t *b)
{
  _EQ_CHECK_INT(a->xnap_cell_type, b->xnap_cell_type);

  if (!eq_byte_array(&a->last_visited_cell_info, &b->last_visited_cell_info))
    return false;

  return true;
}

/**
 * @brief XnAP Handover Request Equality Funciton
 */
bool eq_xnap_handover_request(const xnap_handover_req_t *a, const xnap_handover_req_t *b)
{
  _EQ_CHECK_UINT32(a->s_ng_node_ue_xnap_id, b->s_ng_node_ue_xnap_id);

  if (!eq_xnap_cause(&a->cause, &b->cause))
    return false;

  if (!eq_xnap_ngran_cgi(&a->target_cgi, &b->target_cgi))
    return false;

  if (!eq_nr_guami(&a->guami, &b->guami))
    return false;

  if (!eq_xnap_ue_context_info(&a->ue_context, &b->ue_context))
    return false;

  _EQ_CHECK_INT(a->num_last_visited_cells, b->num_last_visited_cells);

  for (int i = 0; i < a->num_last_visited_cells; i++) {
    if (!eq_xnap_ue_history_info(&a->ue_history_info[i], &b->ue_history_info[i]))
      return false;
  }

  return true;
}

static void free_xnap_qos_flow_param(xnap_qos_flow_param_t *p)
{
  // nothing to free
  UNUSED(p);
}

/**
 * @brief Free QoS flows to be setup list
 */
static void free_xnap_qos_tobe_setup_list(xnap_qos_flow_tobe_setup_item_t *qos_list, uint8_t num_qos)
{
  DevAssert(qos_list && num_qos > 0);

  for (int i = 0; i < num_qos; i++) {
    free_xnap_qos_flow_param(&qos_list[i].qos_params);
  }
  free(qos_list);
}

/**
 * @brief Free PDU session resources to be setup list
 */
static void free_xnap_pdusession_resources_tobe_setup_list(xnap_pdusession_resources_tobe_setup_item_t *pdu_list, uint8_t num_pdu)
{
  DevAssert(pdu_list && num_pdu > 0);

  for (int i = 0; i < num_pdu; i++) {
    free(pdu_list[i].nssai);
    free_xnap_qos_tobe_setup_list(pdu_list[i].qos_list, pdu_list[i].num_qos);
  }
  free(pdu_list);
}

/**
 * @brief Free UE context information
 */
static void free_xnap_ue_context_info(const xnap_ue_context_info_t *ue_context)
{
  DevAssert(ue_context);

  // Free RRC context buffer
  free_byte_array(ue_context->rrc_context);

  // Free PDU session resources
  free_xnap_pdusession_resources_tobe_setup_list(ue_context->pdusession_resources_tobe_setup_list, ue_context->num_pdu);
}

/**
 * @brief Free UE history information list
 */
static void free_xnap_ue_history_info_list(ue_history_info_t *history_list, uint8_t num_cells)
{
  DevAssert(history_list && num_cells > 0);

  for (int i = 0; i < num_cells; i++) {
    free_byte_array(history_list[i].last_visited_cell_info);
  }

  free(history_list);
}

/**
 * @brief XnAP Handover Request memory management
 */
void free_xnap_handover_request(const xnap_handover_req_t *msg)
{
  DevAssert(msg != NULL);

  // Free UE context information
  free_xnap_ue_context_info(&msg->ue_context);

  // Free UE history information
  free_xnap_ue_history_info_list(msg->ue_history_info, msg->num_last_visited_cells);
}
