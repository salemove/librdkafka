/*
 * librdkafka - Apache Kafka C library
 *
 * Copyright (c) 2023, Confluent Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "rd.h"
#include "rdkafka_int.h"
#include "rdkafka_telemetry_encode.h"
#include "nanopb/pb.h"
#include "nanopb/pb_encode.h"
#include "nanopb/pb_decode.h"
#include "opentelemetry/metrics.pb.h"

#define RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR 1000000

typedef struct {
        opentelemetry_proto_metrics_v1_Metric **metrics;
        size_t count;
} rd_kafka_telemetry_metrics_repeated_t;

typedef struct {
        opentelemetry_proto_common_v1_KeyValue **key_values;
        size_t count;
} rd_kafka_telemetry_key_values_repeated_t;


static rd_kafka_telemetry_metric_value_t
calculate_connection_creation_total(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t total;
        rd_kafka_broker_t *rkb;

        total.intValue = 0;
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                if (!rk->rk_telemetry.delta_temporality)
                        total.intValue += rkb->rkb_c.connects.val;
                else
                        total.intValue += rkb->rkb_c.connects.val -
                                          rkb->rkb_c_historic.connects;
        }

        return total;
}

static rd_kafka_telemetry_metric_value_t
calculate_connection_creation_rate(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t total;
        rd_kafka_broker_t *rkb;
        rd_ts_t ts_last = 0;

        total.doubleValue = 0;
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                ts_last = rkb->rkb_c_historic.ts_last;
                total.doubleValue +=
                    rkb->rkb_c.connects.val - rkb->rkb_c_historic.connects;
        }
        int seconds = (rd_uclock() * 1000 - ts_last) / 1e9;
        if (seconds > 0)
                total.doubleValue /= seconds;
        return total;
}

static rd_kafka_telemetry_metric_value_t
calculate_broker_avg_rtt(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t avg_rtt;
        double avg_value = 0;

        int64_t current_cnt  = broker->rkb_avg_rtt.ra_v.cnt;
        int64_t historic_cnt = broker->rkb_c_historic.rkb_avg_rtt.ra_v.cnt;

        if (current_cnt > historic_cnt) {
                int64_t current_sum = broker->rkb_avg_rtt.ra_v.sum;
                int64_t historic_sum =
                    broker->rkb_c_historic.rkb_avg_rtt.ra_v.sum;
                int64_t cnt_diff = current_cnt - historic_cnt;
                int64_t sum_diff = current_sum - historic_sum;

                avg_value =
                    sum_diff / (cnt_diff * RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR);
        }

        avg_rtt.doubleValue = avg_value;
        return avg_rtt;
}

static rd_kafka_telemetry_metric_value_t
calculate_broker_max_rtt(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t max_rtt;

        max_rtt.intValue = broker->rkb_avg_rtt.ra_v.maxv_interval /
                           RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR;
        return max_rtt;
}

static rd_kafka_telemetry_metric_value_t
calculate_throttle_avg(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t avg_throttle;
        int64_t sum_value = 0, broker_count = rk->rk_broker_cnt.val;
        rd_kafka_broker_t *rkb;

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                int64_t current_cnt = rkb->rkb_avg_throttle.ra_v.cnt;
                int64_t historic_cnt =
                    rkb->rkb_c_historic.rkb_avg_throttle.ra_v.cnt;

                if (current_cnt > historic_cnt) {
                        int64_t current_sum = rkb->rkb_avg_throttle.ra_v.sum;
                        int64_t historic_sum =
                            rkb->rkb_c_historic.rkb_avg_throttle.ra_v.sum;
                        int64_t cnt_diff = current_cnt - historic_cnt;
                        int64_t sum_diff = current_sum - historic_sum;

                        sum_value +=
                            sum_diff /
                            (cnt_diff * RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR);
                }
        }
        avg_throttle.intValue = sum_value / broker_count;
        return avg_throttle;
}


static rd_kafka_telemetry_metric_value_t
calculate_throttle_max(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t max_throttle;
        rd_kafka_broker_t *rkb;

        max_throttle.intValue = 0;
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                max_throttle.intValue =
                    RD_MAX(max_throttle.intValue,
                           rkb->rkb_avg_throttle.ra_v.maxv_interval);
        }
        max_throttle.intValue /= RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR;
        return max_throttle;
}

static rd_kafka_telemetry_metric_value_t
calculate_queue_time_avg(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t avg_queue_time;
        int64_t sum_value = 0, broker_count = rk->rk_broker_cnt.val;
        rd_kafka_broker_t *rkb;

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                int64_t current_cnt = rkb->rkb_avg_outbuf_latency.ra_v.cnt;
                int64_t historic_cnt =
                    rkb->rkb_c_historic.rkb_avg_outbuf_latency.ra_v.cnt;

                if (current_cnt > historic_cnt) {
                        int64_t current_sum =
                            rkb->rkb_avg_outbuf_latency.ra_v.sum;
                        int64_t historic_sum =
                            rkb->rkb_c_historic.rkb_avg_outbuf_latency.ra_v.sum;
                        int64_t cnt_diff = current_cnt - historic_cnt;
                        int64_t sum_diff = current_sum - historic_sum;

                        sum_value +=
                            sum_diff /
                            (cnt_diff * RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR);
                }
        }
        avg_queue_time.intValue = sum_value / broker_count;
        return avg_queue_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_queue_time_max(rd_kafka_t *rk, rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t max_queue_time;
        rd_kafka_broker_t *rkb;

        max_queue_time.intValue = 0;
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                max_queue_time.intValue =
                    RD_MAX(max_queue_time.intValue,
                           rkb->rkb_avg_outbuf_latency.ra_v.maxv_interval);
        }
        max_queue_time.intValue /= RDKAFKA_TELEMETRY_NS_TO_MS_FACTOR;
        return max_queue_time;
}

static rd_kafka_telemetry_metric_value_t
calculate_consumer_assigned_partitions(rd_kafka_t *rk,
                                       rd_kafka_broker_t *broker) {
        rd_kafka_telemetry_metric_value_t assigned_partitions;
        rd_kafka_broker_t *rkb;
        rd_kafka_toppar_t *rktp;
        int32_t total_assigned_partitions = 0;

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                total_assigned_partitions += rkb->rkb_toppar_cnt;
                total_assigned_partitions -=
                    rkb->rkb_c_historic.assigned_partitions;
        }
        assigned_partitions.intValue = total_assigned_partitions;
        return assigned_partitions;
}


static void reset_historical_metrics(rd_kafka_t *rk) {
        rd_kafka_broker_t *rkb;
        rd_kafka_toppar_t *rktp;

        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                rkb->rkb_c_historic.assigned_partitions = rkb->rkb_toppar_cnt;
                rkb->rkb_c_historic.connects = rkb->rkb_c.connects.val;
                rkb->rkb_c_historic.ts_last  = rd_uclock() * 1000;
                rkb->rkb_c_historic.connects = rkb->rkb_c.connects.val;
                /* Only ra_v is being used to keep track of the metrics */
                rkb->rkb_c_historic.rkb_avg_rtt.ra_v = rkb->rkb_avg_rtt.ra_v;
                rd_atomic32_set(&rkb->rkb_avg_rtt.ra_v.maxv_reset, 1);
                rkb->rkb_c_historic.rkb_avg_throttle.ra_v =
                    rkb->rkb_avg_throttle.ra_v;
                rd_atomic32_set(&rkb->rkb_avg_throttle.ra_v.maxv_reset, 1);
                rkb->rkb_c_historic.rkb_avg_outbuf_latency.ra_v =
                    rkb->rkb_avg_outbuf_latency.ra_v;
                rd_atomic32_set(&rkb->rkb_avg_outbuf_latency.ra_v.maxv_reset,
                                1);
        }
}

static const rd_kafka_telemetry_metric_value_calculator_t
    PRODUCER_METRIC_VALUE_CALCULATORS[RD_KAFKA_TELEMETRY_PRODUCER_METRIC__CNT] =
        {
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_RATE] =
                &calculate_connection_creation_rate,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_CONNECTION_CREATION_TOTAL] =
                &calculate_connection_creation_total,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_AVG] =
                &calculate_broker_avg_rtt,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_MAX] =
                &calculate_broker_max_rtt,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_AVG] =
                &calculate_throttle_avg,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_PRODUCE_THROTTLE_TIME_MAX] =
                &calculate_throttle_max,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_AVG] =
                &calculate_queue_time_avg,
            [RD_KAFKA_TELEMETRY_METRIC_PRODUCER_RECORD_QUEUE_TIME_MAX] =
                &calculate_queue_time_max,
};

static const rd_kafka_telemetry_metric_value_calculator_t
    CONSUMER_METRIC_VALUE_CALCULATORS[RD_KAFKA_TELEMETRY_CONSUMER_METRIC__CNT] = {
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_RATE] =
            &calculate_connection_creation_rate,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_CONNECTION_CREATION_TOTAL] =
            &calculate_connection_creation_total,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_AVG] =
            &calculate_broker_avg_rtt,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_MAX] =
            &calculate_broker_max_rtt,
        [RD_KAFKA_TELEMETRY_METRIC_CONSUMER_COORDINATOR_ASSIGNED_PARTITIONS] =
            &calculate_consumer_assigned_partitions,
};

static const char *get_client_rack(const rd_kafka_t *rk) {
        return rk->rk_conf.client_rack &&
                       RD_KAFKAP_STR_LEN(rk->rk_conf.client_rack)
                   ? (const char *)rk->rk_conf.client_rack->str
                   : NULL;
}

static const char *get_group_id(const rd_kafka_t *rk) {
        return rk->rk_conf.group_id_str ? (const char *)rk->rk_conf.group_id_str
                                        : NULL;
}

static const char *get_group_instance_id(const rd_kafka_t *rk) {
        return rk->rk_conf.group_instance_id
                   ? (const char *)rk->rk_conf.group_instance_id
                   : NULL;
}

static const char *get_member_id(const rd_kafka_t *rk) {
        return rk->rk_cgrp->rkcg_member_id &&
                       rk->rk_cgrp->rkcg_member_id->len > 0
                   ? (const char *)rk->rk_cgrp->rkcg_member_id->str
                   : NULL;
}

static const char *get_transactional_id(const rd_kafka_t *rk) {
        return rk->rk_conf.eos.transactional_id
                   ? (const char *)rk->rk_conf.eos.transactional_id
                   : NULL;
}

static const rd_kafka_telemetry_attribute_config_t producer_attributes[] = {
    {"client_rack", get_client_rack},
    {"transactional_id", get_transactional_id},
};

static const rd_kafka_telemetry_attribute_config_t consumer_attributes[] = {
    {"client_rack", get_client_rack},
    {"group_id", get_group_id},
    {"group_instance_id", get_group_instance_id},
    {"member_id", get_member_id},
};

static int
count_attributes(rd_kafka_t *rk,
                 const rd_kafka_telemetry_attribute_config_t *configs,
                 int config_count) {
        int count = 0, i;
        for (i = 0; i < config_count; ++i) {
                if (configs[i].getValue(rk)) {
                        count++;
                }
        }
        return count;
}

static void set_attributes(rd_kafka_t *rk,
                           rd_kafka_telemetry_resource_attribute_t **attributes,
                           const rd_kafka_telemetry_attribute_config_t *configs,
                           int config_count) {
        int attr_idx = 0, i;
        for (i = 0; i < config_count; ++i) {
                const char *value = configs[i].getValue(rk);
                if (value) {
                        attributes[attr_idx]->name  = configs[i].name;
                        attributes[attr_idx]->value = value;
                        attr_idx++;
                }
        }
}

static int
resource_attributes(rd_kafka_t *rk,
                    rd_kafka_telemetry_resource_attribute_t **attributes) {
        int count = 0;
        const rd_kafka_telemetry_attribute_config_t *configs;
        int config_count;

        if (rk->rk_type == RD_KAFKA_PRODUCER) {
                configs      = producer_attributes;
                config_count = sizeof(producer_attributes) /
                               sizeof(producer_attributes[0]);
        } else if (rk->rk_type == RD_KAFKA_CONSUMER) {
                configs      = consumer_attributes;
                config_count = sizeof(consumer_attributes) /
                               sizeof(consumer_attributes[0]);
        } else {
                *attributes = NULL;
                return 0;
        }

        count = count_attributes(rk, configs, config_count);

        if (count == 0) {
                *attributes = NULL;
                return 0;
        }

        *attributes =
            rd_malloc(sizeof(rd_kafka_telemetry_resource_attribute_t) * count);

        set_attributes(rk, attributes, configs, config_count);

        return count;
}

static bool
encode_string(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
        if (!pb_encode_tag_for_field(stream, field))
                return false;
        return pb_encode_string(stream, (uint8_t *)(*arg), strlen(*arg));
}

// TODO: Update to handle multiple data points.
static bool encode_number_data_point(pb_ostream_t *stream,
                                     const pb_field_t *field,
                                     void *const *arg) {
        opentelemetry_proto_metrics_v1_NumberDataPoint *data_point =
            (opentelemetry_proto_metrics_v1_NumberDataPoint *)*arg;
        if (!pb_encode_tag_for_field(stream, field))
                return false;

        return pb_encode_submessage(
            stream, opentelemetry_proto_metrics_v1_NumberDataPoint_fields,
            data_point);
}

static bool
encode_metric(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
        rd_kafka_telemetry_metrics_repeated_t *metricArr =
            (rd_kafka_telemetry_metrics_repeated_t *)*arg;
        size_t i;

        for (i = 0; i < metricArr->count; i++) {

                opentelemetry_proto_metrics_v1_Metric *metric =
                    metricArr->metrics[i];
                if (!pb_encode_tag_for_field(stream, field))
                        return false;

                if (!pb_encode_submessage(
                        stream, opentelemetry_proto_metrics_v1_Metric_fields,
                        metric))
                        return false;
        }
        return true;
}

static bool encode_scope_metrics(pb_ostream_t *stream,
                                 const pb_field_t *field,
                                 void *const *arg) {
        opentelemetry_proto_metrics_v1_ScopeMetrics *scope_metrics =
            (opentelemetry_proto_metrics_v1_ScopeMetrics *)*arg;
        if (!pb_encode_tag_for_field(stream, field))
                return false;

        return pb_encode_submessage(
            stream, opentelemetry_proto_metrics_v1_ScopeMetrics_fields,
            scope_metrics);
}

static bool encode_resource_metrics(pb_ostream_t *stream,
                                    const pb_field_t *field,
                                    void *const *arg) {
        opentelemetry_proto_metrics_v1_ResourceMetrics *resource_metrics =
            (opentelemetry_proto_metrics_v1_ResourceMetrics *)*arg;
        if (!pb_encode_tag_for_field(stream, field))
                return false;

        return pb_encode_submessage(
            stream, opentelemetry_proto_metrics_v1_ResourceMetrics_fields,
            resource_metrics);
}

static bool encode_key_value(pb_ostream_t *stream,
                             const pb_field_t *field,
                             void *const *arg) {
        if (!pb_encode_tag_for_field(stream, field))
                return false;
        opentelemetry_proto_common_v1_KeyValue *key_value =
            (opentelemetry_proto_common_v1_KeyValue *)*arg;
        return pb_encode_submessage(
            stream, opentelemetry_proto_common_v1_KeyValue_fields, key_value);
}

static bool encode_key_values(pb_ostream_t *stream,
                              const pb_field_t *field,
                              void *const *arg) {
        rd_kafka_telemetry_key_values_repeated_t *kv_arr =
            (rd_kafka_telemetry_key_values_repeated_t *)*arg;
        size_t i;

        for (i = 0; i < kv_arr->count; i++) {

                opentelemetry_proto_common_v1_KeyValue *kv =
                    kv_arr->key_values[i];
                if (!pb_encode_tag_for_field(stream, field))
                        return false;

                if (!pb_encode_submessage(
                        stream, opentelemetry_proto_common_v1_KeyValue_fields,
                        kv))
                        return false;
        }
        return true;
}

static bool is_per_broker_metric(rd_kafka_t *rk, int metric_idx) {
        if (rk->rk_type == RD_KAFKA_PRODUCER &&
            (metric_idx ==
                 RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_AVG ||
             metric_idx ==
                 RD_KAFKA_TELEMETRY_METRIC_PRODUCER_NODE_REQUEST_LATENCY_MAX)) {
                return true;
        }
        if (rk->rk_type == RD_KAFKA_CONSUMER &&
            (metric_idx ==
                 RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_AVG ||
             metric_idx ==
                 RD_KAFKA_TELEMETRY_METRIC_CONSUMER_NODE_REQUEST_LATENCY_MAX)) {
                return true;
        }
        return false;
}

static void free_metrics(
    opentelemetry_proto_metrics_v1_Metric **metrics,
    char **metric_names,
    opentelemetry_proto_metrics_v1_NumberDataPoint **data_points,
    opentelemetry_proto_common_v1_KeyValue *datapoint_attributes_key_values,
    size_t count) {
        size_t i;
        for (i = 0; i < count; i++) {
                rd_free(data_points[i]);
                rd_free(metric_names[i]);
                rd_free(metrics[i]);
        }
        rd_free(data_points);
        rd_free(metric_names);
        rd_free(metrics);
        rd_free(datapoint_attributes_key_values);
}

static void free_resource_attributes(
    opentelemetry_proto_common_v1_KeyValue **resource_attributes_key_values,
    rd_kafka_telemetry_resource_attribute_t *resource_attributes_struct,
    size_t count) {
        size_t i;
        if (count == 0)
                return;
        for (i = 0; i < count; i++)
                rd_free(resource_attributes_key_values[i]);
        rd_free(resource_attributes_struct);
        rd_free(resource_attributes_key_values);
}

static void serializeMetricData(
    rd_kafka_t *rk,
    rd_kafka_broker_t *rkb,
    const rd_kafka_telemetry_metric_info_t *info,
    opentelemetry_proto_metrics_v1_Metric **metric,
    opentelemetry_proto_metrics_v1_NumberDataPoint **data_point,
    opentelemetry_proto_common_v1_KeyValue *data_point_attribute,
    rd_kafka_telemetry_metric_value_calculator_t metricValueCalculator,
    char **metric_name,
    bool is_per_broker,
    rd_ts_t now_ns) {
        rd_ts_t ts_last, ts_start;
        size_t metric_name_len;
        if (info->is_int) {
                (*data_point)->which_value =
                    opentelemetry_proto_metrics_v1_NumberDataPoint_as_int_tag;
                (*data_point)->value.as_int =
                    metricValueCalculator(rk, rkb).intValue;
        } else {
                (*data_point)->which_value =
                    opentelemetry_proto_metrics_v1_NumberDataPoint_as_double_tag;
                (*data_point)->value.as_double =
                    metricValueCalculator(rk, rkb).doubleValue;
        }
        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                ts_last  = rkb->rkb_c_historic.ts_last;
                ts_start = rkb->rkb_c_historic.ts_start;
                break;
        }

        (*data_point)->time_unix_nano = now_ns;
        if (info->type == RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE)
                (*data_point)->start_time_unix_nano = ts_last;
        else
                (*data_point)->start_time_unix_nano = ts_start;

        if (is_per_broker) {
                data_point_attribute->key.funcs.encode = &encode_string;
                data_point_attribute->key.arg =
                    RD_KAFKA_TELEMETRY_METRIC_NODE_ID_ATTRIBUTE;
                data_point_attribute->has_value = true;
                data_point_attribute->value.which_value =
                    opentelemetry_proto_common_v1_AnyValue_int_value_tag;

                data_point_attribute->value.value.int_value = rkb->rkb_nodeid;
                (*data_point)->attributes.funcs.encode      = &encode_key_value;
                (*data_point)->attributes.arg = data_point_attribute;
        }


        switch (info->type) {

        case RD_KAFKA_TELEMETRY_METRIC_TYPE_SUM: {
                (*metric)->which_data =
                    opentelemetry_proto_metrics_v1_Metric_sum_tag;
                (*metric)->data.sum.data_points.funcs.encode =
                    &encode_number_data_point;
                (*metric)->data.sum.data_points.arg = *data_point;
                (*metric)->data.sum.aggregation_temporality =
                    rk->rk_telemetry.delta_temporality
                        ? opentelemetry_proto_metrics_v1_AggregationTemporality_AGGREGATION_TEMPORALITY_DELTA
                        : opentelemetry_proto_metrics_v1_AggregationTemporality_AGGREGATION_TEMPORALITY_CUMULATIVE;
                (*metric)->data.sum.is_monotonic = true;
                break;
        }
        case RD_KAFKA_TELEMETRY_METRIC_TYPE_GAUGE: {
                (*metric)->which_data =
                    opentelemetry_proto_metrics_v1_Metric_gauge_tag;
                (*metric)->data.gauge.data_points.funcs.encode =
                    &encode_number_data_point;
                (*metric)->data.gauge.data_points.arg = *data_point;
                break;
        }
        default:
                rd_assert(!"Unknown metric type");
                break;
        }

        (*metric)->description.funcs.encode = &encode_string;
        (*metric)->description.arg          = (void *)info->description;

        metric_name_len =
            strlen(RD_KAFKA_TELEMETRY_METRIC_PREFIX) + strlen(info->name) + 1;
        *metric_name = rd_calloc(1, metric_name_len);
        rd_snprintf(*metric_name, metric_name_len, "%s%s",
                    RD_KAFKA_TELEMETRY_METRIC_PREFIX, info->name);


        (*metric)->name.funcs.encode = &encode_string;
        (*metric)->name.arg          = *metric_name;

        /* Skipping unit as Java client does the same */
        // (*metric)->unit.funcs.encode = &encode_string;
        // (*metric)->unit.arg          = (void *)info->unit;
}

/**
 * @brief Encodes the metrics to opentelemetry_proto_metrics_v1_MetricsData and
 * returns the serialized data. Currently only supports encoding of connection
 * creation total by default
 */
void *rd_kafka_telemetry_encode_metrics(rd_kafka_t *rk, size_t *size) {
        size_t message_size;
        uint8_t *buffer;
        pb_ostream_t stream;
        bool status;
        char **metric_names;
        const int *metrics_to_encode = rk->rk_telemetry.matched_metrics;
        const size_t metrics_to_encode_count =
            rk->rk_telemetry.matched_metrics_cnt;
        size_t metric_name_len, total_metrics_count = metrics_to_encode_count;
        size_t i, metric_idx                        = 0;

        for (i = 0; i < metrics_to_encode_count; i++) {
                if (is_per_broker_metric(rk, (int)i)) {
                        total_metrics_count += rk->rk_broker_cnt.val - 1;
                }
        }

        rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                     "Serializing metrics");

        opentelemetry_proto_metrics_v1_MetricsData metrics_data =
            opentelemetry_proto_metrics_v1_MetricsData_init_zero;

        opentelemetry_proto_metrics_v1_ResourceMetrics resource_metrics =
            opentelemetry_proto_metrics_v1_ResourceMetrics_init_zero;

        opentelemetry_proto_metrics_v1_Metric **metrics;
        opentelemetry_proto_common_v1_KeyValue *
            *resource_attributes_key_values = NULL;
        opentelemetry_proto_common_v1_KeyValue
            *datapoint_attributes_key_values = NULL;
        opentelemetry_proto_metrics_v1_NumberDataPoint **data_points;
        rd_kafka_telemetry_metrics_repeated_t metrics_repeated;
        rd_kafka_telemetry_key_values_repeated_t resource_attributes_repeated;
        rd_kafka_telemetry_resource_attribute_t *resource_attributes_struct =
            NULL;
        rd_ts_t now_ns = rd_uclock() * 1000;

        int resource_attributes_count =
            resource_attributes(rk, &resource_attributes_struct);
        rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                     "Resource attributes count: %d",
                     resource_attributes_count);
        if (resource_attributes_count > 0) {
                resource_attributes_key_values =
                    rd_malloc(sizeof(opentelemetry_proto_common_v1_KeyValue *) *
                              resource_attributes_count);
                int ind;
                for (ind = 0; ind < resource_attributes_count; ++ind) {
                        resource_attributes_key_values[ind] = rd_calloc(
                            1, sizeof(opentelemetry_proto_common_v1_KeyValue));
                        resource_attributes_key_values[ind]->key.funcs.encode =
                            &encode_string;
                        resource_attributes_key_values[ind]->key.arg =
                            (void *)resource_attributes_struct[ind].name;

                        resource_attributes_key_values[ind]->has_value = true;
                        resource_attributes_key_values[ind]->value.which_value =
                            opentelemetry_proto_common_v1_AnyValue_string_value_tag;
                        resource_attributes_key_values[ind]
                            ->value.value.string_value.funcs.encode =
                            &encode_string;
                        resource_attributes_key_values[ind]
                            ->value.value.string_value.arg =
                            (void *)resource_attributes_struct[ind].value;
                }
                resource_attributes_repeated.key_values =
                    resource_attributes_key_values;
                resource_attributes_repeated.count = resource_attributes_count;
                resource_metrics.has_resource      = true;
                resource_metrics.resource.attributes.funcs.encode =
                    &encode_key_values;
                resource_metrics.resource.attributes.arg =
                    &resource_attributes_repeated;
        }

        opentelemetry_proto_metrics_v1_ScopeMetrics scopeMetrics =
            opentelemetry_proto_metrics_v1_ScopeMetrics_init_zero;

        opentelemetry_proto_common_v1_InstrumentationScope
            instrumentationScope =
                opentelemetry_proto_common_v1_InstrumentationScope_init_zero;
        instrumentationScope.name.funcs.encode    = &encode_string;
        instrumentationScope.name.arg             = (void *)rd_kafka_name(rk);
        instrumentationScope.version.funcs.encode = &encode_string;
        instrumentationScope.version.arg = (void *)rd_kafka_version_str();

        scopeMetrics.has_scope = true;
        scopeMetrics.scope     = instrumentationScope;

        metrics = rd_malloc(sizeof(opentelemetry_proto_metrics_v1_Metric *) *
                            total_metrics_count);
        data_points =
            rd_malloc(sizeof(opentelemetry_proto_metrics_v1_NumberDataPoint *) *
                      total_metrics_count);
        datapoint_attributes_key_values =
            rd_malloc(sizeof(opentelemetry_proto_common_v1_KeyValue) *
                      total_metrics_count);
        metric_names = rd_malloc(sizeof(char *) * total_metrics_count);
        rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                     "Total metrics to be encoded count: %ld",
                     total_metrics_count);


        for (i = 0; i < metrics_to_encode_count; i++) {

                rd_kafka_telemetry_metric_value_calculator_t
                    metricValueCalculator =
                        (rk->rk_type == RD_KAFKA_PRODUCER)
                            ? PRODUCER_METRIC_VALUE_CALCULATORS
                                  [metrics_to_encode[i]]
                            : CONSUMER_METRIC_VALUE_CALCULATORS
                                  [metrics_to_encode[i]];
                const rd_kafka_telemetry_metric_info_t *info =
                    RD_KAFKA_TELEMETRY_METRIC_INFO(rk);

                if (is_per_broker_metric(rk, (int)metrics_to_encode[i])) {
                        rd_kafka_broker_t *rkb;

                        TAILQ_FOREACH(rkb, &rk->rk_brokers, rkb_link) {
                                metrics[metric_idx] = rd_calloc(
                                    1,
                                    sizeof(
                                        opentelemetry_proto_metrics_v1_Metric));
                                data_points[metric_idx] = rd_calloc(
                                    1,
                                    sizeof(
                                        opentelemetry_proto_metrics_v1_NumberDataPoint));
                                serializeMetricData(
                                    rk, rkb, &info[metrics_to_encode[i]],
                                    &metrics[metric_idx],
                                    &data_points[metric_idx],
                                    &datapoint_attributes_key_values
                                        [metric_idx],
                                    metricValueCalculator,
                                    &metric_names[metric_idx], true, now_ns);
                                metric_idx++;
                        }
                        continue;
                }

                metrics[metric_idx] =
                    rd_calloc(1, sizeof(opentelemetry_proto_metrics_v1_Metric));
                data_points[metric_idx] = rd_calloc(
                    1, sizeof(opentelemetry_proto_metrics_v1_NumberDataPoint));

                serializeMetricData(
                    rk, NULL, &info[metrics_to_encode[i]], &metrics[metric_idx],
                    &data_points[metric_idx],
                    &datapoint_attributes_key_values[metric_idx],
                    metricValueCalculator, &metric_names[metric_idx], false,
                    now_ns);
                metric_idx++;
        }

        /* Send empty metrics blob if no metrics are matched */
        if (total_metrics_count > 0) {
                metrics_repeated.metrics = metrics;
                metrics_repeated.count   = total_metrics_count;

                scopeMetrics.metrics.funcs.encode = &encode_metric;
                scopeMetrics.metrics.arg          = &metrics_repeated;


                resource_metrics.scope_metrics.funcs.encode =
                    &encode_scope_metrics;
                resource_metrics.scope_metrics.arg = &scopeMetrics;

                metrics_data.resource_metrics.funcs.encode =
                    &encode_resource_metrics;
                metrics_data.resource_metrics.arg = &resource_metrics;
        }

        status = pb_get_encoded_size(
            &message_size, opentelemetry_proto_metrics_v1_MetricsData_fields,
            &metrics_data);
        if (!status) {
                rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                             "Failed to get encoded size");
                free_metrics(metrics, metric_names, data_points,
                             datapoint_attributes_key_values,
                             total_metrics_count);
                free_resource_attributes(resource_attributes_key_values,
                                         resource_attributes_struct,
                                         resource_attributes_count);
                return NULL;
        }

        buffer = rd_malloc(message_size);
        if (buffer == NULL) {
                rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                             "Failed to allocate memory for buffer");
                free_metrics(metrics, metric_names, data_points,
                             datapoint_attributes_key_values,
                             total_metrics_count);
                free_resource_attributes(resource_attributes_key_values,
                                         resource_attributes_struct,
                                         resource_attributes_count);

                return NULL;
        }

        stream = pb_ostream_from_buffer(buffer, message_size);
        status = pb_encode(&stream,
                           opentelemetry_proto_metrics_v1_MetricsData_fields,
                           &metrics_data);

        if (!status) {
                rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                             "Encoding failed: %s", PB_GET_ERROR(&stream));
                rd_free(buffer);
                free_metrics(metrics, metric_names, data_points,
                             datapoint_attributes_key_values,
                             total_metrics_count);
                free_resource_attributes(resource_attributes_key_values,
                                         resource_attributes_struct,
                                         resource_attributes_count);

                return NULL;
        }
        free_metrics(metrics, metric_names, data_points,
                     datapoint_attributes_key_values, total_metrics_count);
        free_resource_attributes(resource_attributes_key_values,
                                 resource_attributes_struct,
                                 resource_attributes_count);


        rd_kafka_dbg(rk, TELEMETRY, "RD_KAFKA_TELEMETRY_METRICS_INFO",
                     "Push Telemetry metrics encoded, size: %ld",
                     stream.bytes_written);
        *size = message_size;

        reset_historical_metrics(rk);

        return (void *)buffer;
}