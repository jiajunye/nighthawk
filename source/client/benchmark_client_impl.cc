#include "source/client/benchmark_client_impl.h"

#include "envoy/event/dispatcher.h"
#include "envoy/thread_local/thread_local.h"

#include "nighthawk/common/statistic.h"
#include "nighthawk/user_defined_output/user_defined_output_plugin.h"

#include "external/envoy/source/common/http/header_map_impl.h"
#include "external/envoy/source/common/http/headers.h"
#include "external/envoy/source/common/http/utility.h"
#include "external/envoy/source/common/network/utility.h"

#include "source/client/stream_decoder.h"

#include "absl/strings/numbers.h"
#include "absl/strings/str_split.h"

using namespace std::chrono_literals;

namespace Nighthawk {
namespace Client {

BenchmarkClientStatistic::BenchmarkClientStatistic(BenchmarkClientStatistic&& statistic) noexcept
    : connect_statistic(std::move(statistic.connect_statistic)),
      response_statistic(std::move(statistic.response_statistic)),
      response_header_size_statistic(std::move(statistic.response_header_size_statistic)),
      response_body_size_statistic(std::move(statistic.response_body_size_statistic)),
      latency_1xx_statistic(std::move(statistic.latency_1xx_statistic)),
      latency_2xx_statistic(std::move(statistic.latency_2xx_statistic)),
      latency_3xx_statistic(std::move(statistic.latency_3xx_statistic)),
      latency_4xx_statistic(std::move(statistic.latency_4xx_statistic)),
      latency_5xx_statistic(std::move(statistic.latency_5xx_statistic)),
      latency_xxx_statistic(std::move(statistic.latency_xxx_statistic)),
      origin_latency_statistic(std::move(statistic.origin_latency_statistic)) {}

BenchmarkClientStatistic::BenchmarkClientStatistic(
    StatisticPtr&& connect_stat, StatisticPtr&& response_stat,
    StatisticPtr&& response_header_size_stat, StatisticPtr&& response_body_size_stat,
    StatisticPtr&& latency_1xx_stat, StatisticPtr&& latency_2xx_stat,
    StatisticPtr&& latency_3xx_stat, StatisticPtr&& latency_4xx_stat,
    StatisticPtr&& latency_5xx_stat, StatisticPtr&& latency_xxx_stat,
    StatisticPtr&& origin_latency_stat)
    : connect_statistic(std::move(connect_stat)), response_statistic(std::move(response_stat)),
      response_header_size_statistic(std::move(response_header_size_stat)),
      response_body_size_statistic(std::move(response_body_size_stat)),
      latency_1xx_statistic(std::move(latency_1xx_stat)),
      latency_2xx_statistic(std::move(latency_2xx_stat)),
      latency_3xx_statistic(std::move(latency_3xx_stat)),
      latency_4xx_statistic(std::move(latency_4xx_stat)),
      latency_5xx_statistic(std::move(latency_5xx_stat)),
      latency_xxx_statistic(std::move(latency_xxx_stat)),
      origin_latency_statistic(std::move(origin_latency_stat)) {}

Envoy::Http::ConnectionPool::Cancellable*
Http1PoolImpl::newStream(Envoy::Http::ResponseDecoder& response_decoder,
                         Envoy::Http::ConnectionPool::Callbacks& callbacks,
                         const Instance::StreamOptions& options) {
  // In prefetch mode we try to keep the amount of connections at the configured limit.
  if (prefetch_connections_) {
    while (host_->cluster().resourceManager(priority_).connections().canCreate()) {
      // We pass in a high prefetch ratio, because we don't want to throttle the prefetched
      // connection amount like Envoy does out of the box.
      ConnPoolImplBase::ConnectionResult result = tryCreateNewConnection(10000.0);
      if (result != ConnectionResult::CreatedNewConnection) {
        break;
      }
    }
  }

  // By default, Envoy re-uses the most recent free connection. Here we pop from the back
  // of ready_clients_, which will pick the oldest one instead. This makes us cycle through
  // all the available connections.
  if (!ready_clients_.empty() && connection_reuse_strategy_ == ConnectionReuseStrategy::LRU) {
    Envoy::Http::HttpAttachContext context({&response_decoder, &callbacks});
    attachStreamToClient(*ready_clients_.back(), context);
    return nullptr;
  }

  // Vanilla Envoy pool behavior.
  return HttpConnPoolImplBase::newStream(response_decoder, callbacks, options);
}

BenchmarkClientHttpImpl::BenchmarkClientHttpImpl(
    Envoy::Api::Api& api, Envoy::Event::Dispatcher& dispatcher, Envoy::Stats::Scope& scope,
    BenchmarkClientStatistic& statistic, Envoy::Http::Protocol protocol,
    Envoy::Upstream::ClusterManagerPtr& cluster_manager, Envoy::Tracing::TracerSharedPtr& tracer,
    absl::string_view cluster_name, RequestGenerator request_generator,
    const bool provide_resource_backpressure, absl::string_view latency_response_header_name,
    std::vector<UserDefinedOutputNamePluginPair> user_defined_output_plugins)
    : api_(api), dispatcher_(dispatcher), scope_(scope.createScope("benchmark.")),
      statistic_(std::move(statistic)), protocol_(protocol),
      benchmark_client_counters_({ALL_BENCHMARK_CLIENT_COUNTERS(POOL_COUNTER(*scope_))}),
      cluster_manager_(cluster_manager), tracer_(tracer), cluster_name_(std::string(cluster_name)),
      request_generator_(std::move(request_generator)),
      provide_resource_backpressure_(provide_resource_backpressure),
      latency_response_header_name_(latency_response_header_name),
      user_defined_output_plugins_(std::move(user_defined_output_plugins)) {
  statistic_.connect_statistic->setId("benchmark_http_client.queue_to_connect");
  statistic_.response_statistic->setId("benchmark_http_client.request_to_response");
  statistic_.response_header_size_statistic->setId("benchmark_http_client.response_header_size");
  statistic_.response_body_size_statistic->setId("benchmark_http_client.response_body_size");
  statistic_.latency_1xx_statistic->setId("benchmark_http_client.latency_1xx");
  statistic_.latency_2xx_statistic->setId("benchmark_http_client.latency_2xx");
  statistic_.latency_3xx_statistic->setId("benchmark_http_client.latency_3xx");
  statistic_.latency_4xx_statistic->setId("benchmark_http_client.latency_4xx");
  statistic_.latency_5xx_statistic->setId("benchmark_http_client.latency_5xx");
  statistic_.latency_xxx_statistic->setId("benchmark_http_client.latency_xxx");
  statistic_.origin_latency_statistic->setId("benchmark_http_client.origin_latency_statistic");
}

void BenchmarkClientHttpImpl::terminate() {
  absl::optional<Envoy::Upstream::HttpPoolData> pool_data = pool();
  if (pool_data.has_value() && pool_data.value().hasActiveConnections()) {
    // We don't report what happens after this call in the output, but latencies may still be
    // reported via callbacks. This may happen after a long time (60s), which HdrHistogram can't
    // track the way we configure it today, as that exceeds the max that it can record.
    // No harm is done, but it does result in log lines warning about it. Avoid that, by
    // disabling latency measurement here.
    setShouldMeasureLatencies(false);
    pool_data.value().addIdleCallback([this]() -> void {
      drain_timer_->disableTimer();
      dispatcher_.exit();
    });
    // Set up a timer with a callback which caps the time we wait for the pool to drain.
    drain_timer_ = dispatcher_.createTimer([this]() -> void {
      ENVOY_LOG(info, "Wait for the connection pool drain timed out, proceeding to hard shutdown.");
      dispatcher_.exit();
    });
    drain_timer_->enableTimer(30s);
    dispatcher_.run(Envoy::Event::Dispatcher::RunType::RunUntilExit);
  }
}

StatisticPtrMap BenchmarkClientHttpImpl::statistics() const {
  StatisticPtrMap statistics;
  statistics[statistic_.connect_statistic->id()] = statistic_.connect_statistic.get();
  statistics[statistic_.response_statistic->id()] = statistic_.response_statistic.get();
  statistics[statistic_.response_header_size_statistic->id()] =
      statistic_.response_header_size_statistic.get();
  statistics[statistic_.response_body_size_statistic->id()] =
      statistic_.response_body_size_statistic.get();
  statistics[statistic_.latency_1xx_statistic->id()] = statistic_.latency_1xx_statistic.get();
  statistics[statistic_.latency_2xx_statistic->id()] = statistic_.latency_2xx_statistic.get();
  statistics[statistic_.latency_3xx_statistic->id()] = statistic_.latency_3xx_statistic.get();
  statistics[statistic_.latency_4xx_statistic->id()] = statistic_.latency_4xx_statistic.get();
  statistics[statistic_.latency_5xx_statistic->id()] = statistic_.latency_5xx_statistic.get();
  statistics[statistic_.latency_xxx_statistic->id()] = statistic_.latency_xxx_statistic.get();
  statistics[statistic_.origin_latency_statistic->id()] = statistic_.origin_latency_statistic.get();
  return statistics;
};

bool BenchmarkClientHttpImpl::tryStartRequest(CompletionCallback caller_completion_callback) {
  absl::optional<Envoy::Upstream::HttpPoolData> pool_data = pool();
  if (!pool_data.has_value()) {
    return false;
  }
  if (provide_resource_backpressure_) {
    uint64_t max_active_requests = 0;
    if (protocol_ == Envoy::Http::Protocol::Http2 || protocol_ == Envoy::Http::Protocol::Http3) {
      max_active_requests = max_active_requests_;
    } else {
      max_active_requests = connection_limit_;
    }
    const uint64_t max_in_flight = max_pending_requests_ + max_active_requests;

    if (requests_initiated_ - requests_completed_ >= max_in_flight) {
      // When we allow client-side queueing, we want to have a sense of time spend waiting on that
      // queue. So we return false here to indicate we couldn't initiate a new request.
      return false;
    }
  }
  auto request = request_generator_();
  // The header generator may not have something for us to send. We'll try next time.
  // TODO(oschaaf): track occurrences of this via a counter & consider setting up a default failure
  // condition for when this happens.
  if (request == nullptr) {
    return false;
  }
  auto* content_length_header = request->header()->ContentLength();
  uint64_t content_length = 0;
  if (content_length_header != nullptr) {
    auto s_content_length = content_length_header->value().getStringView();
    if (!absl::SimpleAtoi(s_content_length, &content_length)) {
      ENVOY_LOG_EVERY_POW_2(error, "Ignoring bad content length of {}", s_content_length);
      content_length = 0;
    }
  }

  auto stream_decoder = new StreamDecoder(
      dispatcher_, api_.timeSource(), *this, std::move(caller_completion_callback),
      *statistic_.connect_statistic, *statistic_.response_statistic,
      *statistic_.response_header_size_statistic, *statistic_.response_body_size_statistic,
      *statistic_.origin_latency_statistic, request->header(), request->body(),
      shouldMeasureLatencies(), content_length, generator_, tracer_, latency_response_header_name_);
  requests_initiated_++;
  pool_data.value().newStream(*stream_decoder, *stream_decoder,
                              {/*can_send_early_data_=*/false,
                               /*can_use_http3_=*/true});
  return true;
}

void BenchmarkClientHttpImpl::onComplete(bool success,
                                         const Envoy::Http::ResponseHeaderMap& headers) {
  requests_completed_++;
  if (!success) {
    benchmark_client_counters_.stream_resets_.inc();
  } else {
    ASSERT(headers.Status());
    const int64_t status = Envoy::Http::Utility::getResponseStatus(headers);

    if (status > 99 && status <= 199) {
      benchmark_client_counters_.http_1xx_.inc();
    } else if (status > 199 && status <= 299) {
      benchmark_client_counters_.http_2xx_.inc();
    } else if (status > 299 && status <= 399) {
      benchmark_client_counters_.http_3xx_.inc();
    } else if (status > 399 && status <= 499) {
      benchmark_client_counters_.http_4xx_.inc();
    } else if (status > 499 && status <= 599) {
      benchmark_client_counters_.http_5xx_.inc();
    } else {
      benchmark_client_counters_.http_xxx_.inc();
    }
  }
  for (UserDefinedOutputNamePluginPair& plugin : user_defined_output_plugins_) {
    absl::Status status = plugin.second->handleResponseHeaders(headers);
    if (!status.ok()) {
      benchmark_client_counters_.user_defined_plugin_handle_headers_failure_.inc();
    }
  }
}

void BenchmarkClientHttpImpl::handleResponseData(const Envoy::Buffer::Instance& response_data) {
  for (UserDefinedOutputNamePluginPair& plugin : user_defined_output_plugins_) {
    absl::Status status = plugin.second->handleResponseData(response_data);
    if (!status.ok()) {
      benchmark_client_counters_.user_defined_plugin_handle_data_failure_.inc();
    }
  }
}

void BenchmarkClientHttpImpl::onPoolFailure(Envoy::Http::ConnectionPool::PoolFailureReason reason) {
  switch (reason) {
  case Envoy::Http::ConnectionPool::PoolFailureReason::Overflow:
    benchmark_client_counters_.pool_overflow_.inc();
    break;
  case Envoy::Http::ConnectionPool::PoolFailureReason::LocalConnectionFailure:
  case Envoy::Http::ConnectionPool::PoolFailureReason::RemoteConnectionFailure:
    benchmark_client_counters_.pool_connection_failure_.inc();
    break;
  case Envoy::Http::ConnectionPool::PoolFailureReason::Timeout:
    break;
  default:
    PANIC("not reached");
  }
}

void BenchmarkClientHttpImpl::exportLatency(const uint32_t response_code,
                                            const uint64_t latency_ns) {
  if (response_code > 99 && response_code <= 199) {
    statistic_.latency_1xx_statistic->addValue(latency_ns);
  } else if (response_code > 199 && response_code <= 299) {
    statistic_.latency_2xx_statistic->addValue(latency_ns);
  } else if (response_code > 299 && response_code <= 399) {
    statistic_.latency_3xx_statistic->addValue(latency_ns);
  } else if (response_code > 399 && response_code <= 499) {
    statistic_.latency_4xx_statistic->addValue(latency_ns);
  } else if (response_code > 499 && response_code <= 599) {
    statistic_.latency_5xx_statistic->addValue(latency_ns);
  } else {
    statistic_.latency_xxx_statistic->addValue(latency_ns);
  }
}

std::vector<nighthawk::client::UserDefinedOutput>
BenchmarkClientHttpImpl::getUserDefinedOutputResults() const {
  std::vector<nighthawk::client::UserDefinedOutput> outputs;
  for (const UserDefinedOutputNamePluginPair& plugin : user_defined_output_plugins_) {
    absl::StatusOr<Envoy::ProtobufWkt::Any> per_worker_output = plugin.second->getPerWorkerOutput();
    nighthawk::client::UserDefinedOutput output_result;
    output_result.set_plugin_name(plugin.first);
    if (!per_worker_output.ok()) {
      ENVOY_LOG(error, "Plugin with class type {} received error status: ", plugin.first,
                per_worker_output.status().message());
      *output_result.mutable_error_message() = per_worker_output.status().ToString();
    } else {
      *output_result.mutable_typed_output() = *per_worker_output;
    }
    outputs.push_back(output_result);
  }
  return outputs;
}

} // namespace Client
} // namespace Nighthawk
