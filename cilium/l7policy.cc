#include "cilium/l7policy.h"

#include <string>

#include "cilium/api/l7policy.pb.validate.h"
#include "cilium/network_policy.h"
#include "cilium/socket_option.h"
#include "source/common/buffer/buffer_impl.h"
#include "source/common/common/enum_to_int.h"
#include "source/common/config/utility.h"
#include "source/common/network/upstream_server_name.h"
#include "source/common/network/upstream_subject_alt_names.h"
#include "source/common/http/header_map_impl.h"
#include "source/common/http/utility.h"
#include "envoy/registry/registry.h"
#include "envoy/singleton/manager.h"

namespace Envoy {
namespace Cilium {

class ConfigFactory
    : public Server::Configuration::NamedHttpFilterConfigFactory {
 public:
  Http::FilterFactoryCb createFilterFactoryFromProto(
      const Protobuf::Message& proto_config, const std::string&,
      Server::Configuration::FactoryContext& context) override {
    auto config = std::make_shared<Cilium::Config>(
        MessageUtil::downcastAndValidate<const ::cilium::L7Policy&>(
            proto_config, context.messageValidationVisitor()),
        context);
    return [config](
               Http::FilterChainFactoryCallbacks& callbacks) mutable -> void {
      callbacks.addStreamFilter(std::make_shared<Cilium::AccessFilter>(config));
    };
  }

  ProtobufTypes::MessagePtr createEmptyConfigProto() override {
    return std::make_unique<::cilium::L7Policy>();
  }

  std::string name() const override { return "cilium.l7policy"; }
};

/**
 * Static registration for this filter. @see RegisterFactory.
 */
REGISTER_FACTORY(ConfigFactory, Server::Configuration::NamedHttpFilterConfigFactory);

Config::Config(const std::string& access_log_path,
               const std::string& denied_403_body,
               Server::Configuration::FactoryContext& context)
    : time_source_(context.timeSource()),
      stats_{ALL_CILIUM_STATS(POOL_COUNTER_PREFIX(context.scope(), "cilium"))},
      denied_403_body_(denied_403_body),
      access_log_(nullptr) {
  if (access_log_path.length()) {
    access_log_ = AccessLog::Open(access_log_path);
    if (!access_log_) {
      ENVOY_LOG(warn, "Cilium filter can not open access log socket {}",
                access_log_path);
    }
  }
  if (denied_403_body_.length() == 0) {
    denied_403_body_ = "Access denied";
  }
  size_t len = denied_403_body_.length();
  if (len < 2 || denied_403_body_[len - 2] != '\r' ||
      denied_403_body_[len - 1] != '\n') {
    denied_403_body_.append("\r\n");
  }
}

Config::Config(const ::cilium::L7Policy& config,
               Server::Configuration::FactoryContext& context)
    : Config(config.access_log_path(), config.denied_403_body(), context) {
  if (config.policy_name() != "") {
    throw EnvoyException(fmt::format(
        "cilium.l7policy: 'policy_name' is no longer supported: \'{}\'",
        config.DebugString()));
  }
  if (config.has_is_ingress()) {
    ENVOY_LOG(warn,
              "cilium.l7policy: 'is_ingress' config option is deprecated and "
              "is ignored: \'{}\'",
              config.DebugString());
  }
}

Config::~Config() {
  if (access_log_) {
    access_log_->Close();
  }
}

void Config::Log(AccessLog::Entry& entry, ::cilium::EntryType type) {
  if (access_log_) {
    access_log_->Log(entry, type);
  }
}

void AccessFilter::onDestroy() {}

Http::FilterHeadersStatus AccessFilter::decodeHeaders(
    Http::RequestHeaderMap& headers, bool) {
  headers.remove(Http::Headers::get().EnvoyOriginalDstHost);
  const auto& conn = callbacks_->connection();

  if (!conn) {
    ENVOY_LOG(warn, "cilium.l7policy: No connection");
    // Return a 403 response
    callbacks_->sendLocalReply(Http::Code::Forbidden, config_->denied_403_body_,
			       nullptr, absl::nullopt, absl::string_view());
    return Http::FilterHeadersStatus::StopIteration;
  }

  const Network::Socket::OptionsSharedPtr socketOptions = conn->socketOptions();
  callbacks_->addUpstreamCallback([this, socketOptions](Http::RequestHeaderMap& headers,
							StreamInfo::StreamInfo& stream_info) -> bool {
    const auto option = Cilium::GetSocketOption(socketOptions);
    if (!option) {
      ENVOY_LOG(warn, "cilium.l7policy: Cilium Socket Option not found");
      return false;
    }
    std::string policy_name = option->pod_ip_;
    bool ingress = option->ingress_;
    uint32_t destination_identity = 0;
    uint32_t destination_port = option->port_;
    const Network::Address::InstanceConstSharedPtr& dst_address = stream_info.upstreamInfo()->upstreamHost()->address();

    if (nullptr == dst_address) {
      ENVOY_LOG(warn, "cilium.l7policy: No destination address");
      return false;
    }
    if (!ingress) {
      const auto dip = dst_address->ip();
      if (!dip) {
	ENVOY_LOG_MISC(warn, "cilium.l7policy: Non-IP destination address: {}", dst_address->asString());
	return false;
      }
      destination_port = dip->port();
      destination_identity = option->resolvePolicyId(dip);
    }

    // Fill in the log entry
    log_entry_.InitFromRequest(policy_name, option->ingress_, option->identity_,
			       callbacks_->streamInfo().downstreamAddressProvider().remoteAddress(),
			       destination_identity, dst_address, stream_info, headers);

    const auto& policy = option->getPolicy();
    if (policy) {
      allowed_ = policy->Allowed(ingress, destination_port,
				 ingress ? option->identity_ : destination_identity,
				 headers, log_entry_);
      ENVOY_LOG(debug,
		"cilium.l7policy: {} ({}->{}) policy lookup for endpoint {} for port {}: {}",
		ingress ? "ingress" : "egress", option->identity_, destination_identity,
		policy_name, destination_port, allowed_ ? "ALLOW" : "DENY");
      if (allowed_) {
	// Log as a forwarded request
	config_->Log(log_entry_, ::cilium::EntryType::Request);
      }
    } else {
      ENVOY_LOG(debug,
		"cilium.l7policy: No {} policy found for pod {}, defaulting to DENY", ingress ? "ingress" : "egress", option->pod_ip_);
    }

    return allowed_;
  });

  return Http::FilterHeadersStatus::Continue;
}

Http::FilterHeadersStatus AccessFilter::encodeHeaders(
    Http::ResponseHeaderMap& headers, bool) {
  log_entry_.UpdateFromResponse(headers, config_->time_source_);
  auto logType = ::cilium::EntryType::Response;
  if (!allowed_) {
    logType = ::cilium::EntryType::Denied;
    config_->stats_.access_denied_.inc();
  }
  config_->Log(log_entry_, logType);
  return Http::FilterHeadersStatus::Continue;
}

}  // namespace Cilium
}  // namespace Envoy
