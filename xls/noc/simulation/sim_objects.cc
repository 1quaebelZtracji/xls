// Copyright 2021 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/noc/simulation/sim_objects.h"

#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "xls/common/integral_types.h"
#include "xls/common/status/ret_check.h"
#include "xls/noc/config/network_config.pb.h"
#include "xls/noc/simulation/common.h"
#include "xls/noc/simulation/network_graph.h"
#include "xls/noc/simulation/parameters.h"

namespace xls {
namespace noc {
namespace {

// Implements an simple pipeline between two connections.
//
// Template parameters are used to switch between the different types of
// phits we support -- either data (TimedDataPhit) or
// metadata (TimedMetadataPhit).
template <typename DataTimePhitT,
          typename DataPhitT = decltype(DataTimePhitT::phit)>
class SimplePipelineImpl {
 public:
  SimplePipelineImpl(int64 stage_count, DataTimePhitT& from_channel,
                     DataTimePhitT& to_channel, std::queue<DataPhitT>& state)
      : stage_count_(stage_count),
        from_(from_channel),
        to_(to_channel),
        state_(state) {}

  bool TryPropagation(NocSimulator& simulator);

 private:
  int64 stage_count_;
  DataTimePhitT& from_;
  DataTimePhitT& to_;
  std::queue<DataPhitT>& state_;
};

template <typename DataTimePhitT, typename DataPhitT>
bool SimplePipelineImpl<DataTimePhitT, DataPhitT>::TryPropagation(
    NocSimulator& simulator) {
  int64 current_cycle = simulator.GetCurrentCycle();

  if (from_.cycle == current_cycle) {
    if (to_.cycle == current_cycle) {
      return true;
    }

    state_.push(from_.phit);

    XLS_LOG(INFO) << absl::StreamFormat("... link received data %x valid %d",
                                        from_.phit.data, from_.phit.valid);

    if (state_.size() > stage_count_) {
      to_.phit = state_.front();
      to_.cycle = current_cycle;
      state_.pop();
    } else {
      to_.phit.valid = false;
      to_.phit.data = 0;
      to_.cycle = current_cycle;
    }

    XLS_LOG(INFO) << absl::StreamFormat(
        "... link sending data %x valid %d connection", to_.phit.data,
        to_.phit.valid);
  }

  return false;
}

}  // namespace

absl::Status NocSimulator::CreateSimulationObjects(NetworkId network) {
  Network& network_obj = mgr_->GetNetwork(network);

  // Create connection simulation objects.
  for (int64 i = 0; i < network_obj.GetConnectionCount(); ++i) {
    ConnectionId id = network_obj.GetConnectionIdByIndex(i);
    XLS_RETURN_IF_ERROR(CreateConnection(id));
  }

  // Create component simulation objects.
  for (int64 i = 0; i < network_obj.GetNetworkComponentCount(); ++i) {
    NetworkComponentId id = network_obj.GetNetworkComponentIdByIndex(i);
    XLS_RETURN_IF_ERROR(CreateNetworkComponent(id));
  }

  return absl::OkStatus();
}

absl::Status NocSimulator::CreateConnection(ConnectionId connection) {
  // Find number of vc's.
  Connection& connection_obj = mgr_->GetConnection(connection);
  XLS_ASSIGN_OR_RETURN(PortParam from_port_param,
                       params_->GetPortParam(connection_obj.src()));
  int64 vc_count = from_port_param.VirtualChannelCount();

  // Construct new connection object.
  SimConnectionState& new_connection = NewConnection(connection);

  new_connection.id = connection_obj.id();
  new_connection.forward_channels.cycle = cycle_;
  new_connection.forward_channels.phit.valid = false;
  new_connection.forward_channels.phit.destination_index = 0;
  new_connection.forward_channels.phit.vc = 0;
  new_connection.forward_channels.phit.data = 0;

  if (vc_count == 0) {
    vc_count = 1;
  }

  new_connection.reverse_channels.resize(vc_count);
  for (int64 i = 0; i < vc_count; ++i) {
    TimedMetadataPhit& phit = new_connection.reverse_channels[i];
    phit.cycle = cycle_;
    phit.phit.valid = false;
    phit.phit.data = 0;
  }

  return absl::OkStatus();
}

absl::Status NocSimulator::CreateNetworkComponent(NetworkComponentId nc_id) {
  NetworkComponent& network_component = mgr_->GetNetworkComponent(nc_id);

  switch (network_component.kind()) {
    case NetworkComponentKind::kNISrc:
      return CreateNetworkInterfaceSrc(nc_id);
    case NetworkComponentKind::kNISink:
      return CreateNetworkInterfaceSink(nc_id);
    case NetworkComponentKind::kLink:
      return CreateLink(nc_id);
    case NetworkComponentKind::kRouter:
      return CreateRouter(nc_id);
    case NetworkComponentKind::kNone:
      break;
  }

  return absl::InternalError(absl::StrFormat(
      "Unsupported network component kind %d", network_component.kind()));
}

absl::Status NocSimulator::CreateNetworkInterfaceSrc(NetworkComponentId nc_id) {
  int64 index = network_interface_sources_.size();

  XLS_ASSIGN_OR_RETURN(SimNetworkInterfaceSrc sim_obj,
                       SimNetworkInterfaceSrc::Create(nc_id, *this));
  network_interface_sources_.push_back(std::move(sim_obj));
  src_index_map_.insert({nc_id, index});

  return absl::OkStatus();
}

absl::Status NocSimulator::CreateNetworkInterfaceSink(
    NetworkComponentId nc_id) {
  int64 index = network_interface_sinks_.size();

  XLS_ASSIGN_OR_RETURN(SimNetworkInterfaceSink sim_obj,
                       SimNetworkInterfaceSink::Create(nc_id, *this));
  network_interface_sinks_.push_back(std::move(sim_obj));
  sink_index_map_.insert({nc_id, index});

  return absl::OkStatus();
}

absl::Status NocSimulator::CreateLink(NetworkComponentId nc_id) {
  XLS_ASSIGN_OR_RETURN(SimLink sim_obj, SimLink::Create(nc_id, *this));
  links_.push_back(std::move(sim_obj));
  return absl::OkStatus();
}

absl::Status NocSimulator::CreateRouter(NetworkComponentId nc_id) {
  XLS_ASSIGN_OR_RETURN(SimInputBufferedVCRouter sim_obj,
                       SimInputBufferedVCRouter::Create(nc_id, *this));
  routers_.push_back(std::move(sim_obj));
  return absl::OkStatus();
}

void NocSimulator::Dump() {
  Network& network_obj = mgr_->GetNetwork(network_);
  // Create connection simulation objects
  for (int64 i = 0; i < network_obj.GetConnectionCount(); ++i) {
    ConnectionId id = network_obj.GetConnectionIdByIndex(i);
    int64 index = GetConnectionIndex(id);
    SimConnectionState& connection = GetSimConnectionByIndex(index);

    XLS_LOG(INFO) << absl::StreamFormat(
        "Simul Connection id %x data %x cycle %d", id.AsUInt64(),
        connection.forward_channels.phit.data,
        connection.forward_channels.cycle);
  }

  // Create connection simulation objects
  for (int64 i = 0; i < network_obj.GetNetworkComponentCount(); ++i) {
    auto id = network_obj.GetNetworkComponentIdByIndex(i);

    XLS_LOG(INFO) << absl::StreamFormat("Simul Component id %x", id.AsUInt64());
    mgr_->GetNetworkComponent(id).Dump();
  }
}

absl::Status NocSimulator::RunCycle(int64 max_ticks) {
  ++cycle_;
  XLS_LOG(INFO) << "";
  XLS_LOG(INFO) << absl::StreamFormat("*** Simul Cycle %d", cycle_);

  bool converged = false;
  int64 nticks = 0;
  while (!converged) {
    XLS_LOG(INFO) << absl::StreamFormat("Tick %d", nticks);
    converged = Tick();
    ++nticks;
  }

  for (int64 i = 0; i < connections_.size(); ++i) {
    XLS_LOG(INFO) << absl::StreamFormat("  Connection %d (%x)", i,
                                        connections_[i].id.AsUInt64());

    XLS_LOG(INFO) << absl::StreamFormat(
        "    FWD cycle %d data %x vc %d dest %d valid %d",
        connections_[i].forward_channels.cycle,
        connections_[i].forward_channels.phit.data,
        connections_[i].forward_channels.phit.vc,
        connections_[i].forward_channels.phit.destination_index,
        connections_[i].forward_channels.phit.valid);

    for (int64 vc = 0; vc < connections_[i].reverse_channels.size(); ++vc) {
      XLS_LOG(INFO) << absl::StreamFormat(
          "    REV %d cycle %d data %x valid %d", vc,
          connections_[i].reverse_channels[vc].cycle,
          connections_[i].reverse_channels[vc].phit.data,
          connections_[i].reverse_channels[vc].phit.valid);
    }

    if (nticks >= max_ticks) {
      return absl::InternalError(absl::StrFormat(
          "Simulator unable to converge after %d ticks for cycle %d", nticks,
          cycle_));
    }
  }

  return absl::OkStatus();
}

bool NocSimulator::Tick() {
  // Goes through each simulator object and run atick.
  // Converges when everyone returns True -- that determines new cycle

  bool converged = true;

  for (SimNetworkInterfaceSrc& nc : network_interface_sources_) {
    NetworkComponentId id = nc.GetId();
    bool this_converged = nc.Tick(*this);
    converged &= this_converged;
    XLS_LOG(INFO) << absl::StreamFormat(" NC %x Converged %d", id.AsUInt64(),
                                        this_converged);
  }

  for (SimLink& nc : links_) {
    NetworkComponentId id = nc.GetId();
    bool this_converged = nc.Tick(*this);
    converged &= this_converged;
    XLS_LOG(INFO) << absl::StreamFormat(" NC %x Converged %d", id.AsUInt64(),
                                        this_converged);
  }

  for (SimInputBufferedVCRouter& nc : routers_) {
    NetworkComponentId id = nc.GetId();
    bool this_converged = nc.Tick(*this);
    converged &= this_converged;
    XLS_LOG(INFO) << absl::StreamFormat(" NC %x Converged %d", id.AsUInt64(),
                                        this_converged);
  }

  for (SimNetworkInterfaceSink& nc : network_interface_sinks_) {
    NetworkComponentId id = nc.GetId();
    bool this_converged = nc.Tick(*this);
    converged &= this_converged;
    XLS_LOG(INFO) << absl::StreamFormat(" NC %x Converged %d", id.AsUInt64(),
                                        this_converged);
  }

  return converged;
}

bool SimNetworkComponentBase::Tick(NocSimulator& simulator) {
  int64 cycle = simulator.GetCurrentCycle();

  bool converged = true;
  if (forward_propagated_cycle_ != cycle) {
    if (TryForwardPropagation(simulator)) {
      forward_propagated_cycle_ = cycle;
    } else {
      converged = false;
    }
  }
  if (reverse_propagated_cycle_ != cycle) {
    if (TryReversePropagation(simulator)) {
      reverse_propagated_cycle_ = cycle;
    } else {
      converged = false;
    }
  }
  return converged;
}

absl::Status SimLink::InitializeImpl(NocSimulator& simulator) {
  XLS_ASSIGN_OR_RETURN(
      NetworkComponentParam nc_param,
      simulator.GetNocParameters()->GetNetworkComponentParam(id_));
  LinkParam& param = absl::get<LinkParam>(nc_param);

  forward_pipeline_stages_ = param.GetSourceToSinkPipelineStages();
  reverse_pipeline_stages_ = param.GetSinkToSourcePipelineStages();
  phit_width_ = param.GetPhitDataBitWidth();

  NetworkManager* network_manager = simulator.GetNetworkManager();

  PortId src_port =
      network_manager->GetNetworkComponent(id_).GetPortIdByIndex(0);
  PortId sink_port =
      network_manager->GetNetworkComponent(id_).GetPortIdByIndex(1);
  if (network_manager->GetPort(src_port).direction() ==
      PortDirection::kOutput) {
    // Swap src/sink if the src port actually had index 0.
    PortId tmp = src_port;
    src_port = sink_port;
    sink_port = tmp;
  }

  ConnectionId src_connection = network_manager->GetPort(src_port).connection();
  ConnectionId sink_connection =
      network_manager->GetPort(sink_port).connection();

  src_connection_index_ = simulator.GetConnectionIndex(src_connection);
  sink_connection_index_ = simulator.GetConnectionIndex(sink_connection);

  // Create a reverse pipeline stage for each vc.
  SimConnectionState& sink =
      simulator.GetSimConnectionByIndex(sink_connection_index_);
  reverse_credit_stages_.resize(sink.reverse_channels.size());

  return absl::OkStatus();
}

absl::Status SimNetworkInterfaceSrc::InitializeImpl(NocSimulator& simulator) {
  XLS_ASSIGN_OR_RETURN(
      NetworkComponentParam nc_param,
      simulator.GetNocParameters()->GetNetworkComponentParam(id_));
  NetworkInterfaceSrcParam& param =
      absl::get<NetworkInterfaceSrcParam>(nc_param);

  int64 virtual_channel_count = param.GetPortParam().VirtualChannelCount();
  data_to_send_.resize(virtual_channel_count);
  credit_.resize(virtual_channel_count, 0);
  credit_update_.resize(virtual_channel_count,
                        CreditState{simulator.GetCurrentCycle(), 0});

  NetworkManager* network_manager = simulator.GetNetworkManager();
  PortId sink_port =
      network_manager->GetNetworkComponent(id_).GetPortIdByIndex(0);
  ConnectionId sink_connection =
      network_manager->GetPort(sink_port).connection();

  sink_connection_index_ = simulator.GetConnectionIndex(sink_connection);

  return absl::OkStatus();
}

absl::Status SimNetworkInterfaceSrc::SendPhitAtTime(TimedDataPhit phit) {
  int64 vc_index = phit.phit.vc;

  if (vc_index < data_to_send_.size()) {
    data_to_send_[vc_index].push(phit);
    return absl::OkStatus();
  } else {
    return absl::OutOfRangeError(
        absl::StrFormat("Unable to send phit to vc index %d, max %d", vc_index,
                        data_to_send_.size()));
  }
}

absl::Status SimNetworkInterfaceSink::InitializeImpl(NocSimulator& simulator) {
  XLS_ASSIGN_OR_RETURN(
      NetworkComponentParam nc_param,
      simulator.GetNocParameters()->GetNetworkComponentParam(id_));
  NetworkInterfaceSinkParam& param =
      absl::get<NetworkInterfaceSinkParam>(nc_param);

  PortParam port_param = param.GetPortParam();
  std::vector<VirtualChannelParam> vc_params = port_param.GetVirtualChannels();
  int64 virtual_channel_count = port_param.VirtualChannelCount();

  input_buffers_.resize(virtual_channel_count);

  for (int64 vc = 0; vc < virtual_channel_count; ++vc) {
    input_buffers_[vc].max_queue_size = vc_params[vc].GetDepth();
  }

  NetworkManager* network_manager = simulator.GetNetworkManager();
  PortId src_port =
      network_manager->GetNetworkComponent(id_).GetPortIdByIndex(0);
  ConnectionId src_connection = network_manager->GetPort(src_port).connection();

  src_connection_index_ = simulator.GetConnectionIndex(src_connection);

  return absl::OkStatus();
}

absl::Status SimInputBufferedVCRouter::InitializeImpl(NocSimulator& simulator) {
  NetworkManager* network_manager = simulator.GetNetworkManager();
  NetworkComponent& nc = network_manager->GetNetworkComponent(id_);
  const PortIndexMap& port_indexer =
      simulator.GetRoutingTable()->GetPortIndices();

  // Setup structures associated with the inputs.
  //  - input to SimConnectionState (input_connection_index_start_ and count_)
  //  - input_buffers_
  input_connection_count_ = nc.GetInputPortIds().size();
  input_connection_index_start_ =
      simulator.GetNewConnectionIndicesStore(input_connection_count_);
  absl::Span<int64> input_indices = simulator.GetConnectionIndicesStore(
      input_connection_index_start_, input_connection_count_);

  input_buffers_.resize(input_connection_count_);
  input_credit_to_send_.resize(input_connection_count_);
  max_vc_ = 0;
  for (int64 i = 0; i < input_connection_count_; ++i) {
    XLS_ASSIGN_OR_RETURN(
        PortId port_id,
        port_indexer.GetPortByIndex(nc.id(), PortDirection::kInput, i));
    Port& port = network_manager->GetPort(port_id);
    input_indices[i] = simulator.GetConnectionIndex(port.connection());

    XLS_ASSIGN_OR_RETURN(PortParam port_param,
                         simulator.GetNocParameters()->GetPortParam(port_id));
    std::vector<VirtualChannelParam> vc_params =
        port_param.GetVirtualChannels();

    input_buffers_[i].resize(port_param.VirtualChannelCount());
    for (int64 vc = 0; vc < port_param.VirtualChannelCount(); ++vc) {
      input_buffers_[i][vc].max_queue_size = vc_params[vc].GetDepth();
    }
    input_credit_to_send_[i].resize(port_param.VirtualChannelCount());
    if (max_vc_ < port_param.VirtualChannelCount()) {
      max_vc_ = port_param.VirtualChannelCount();
    }
  }

  // Setup structures associated with the outputs.
  //  - output to SimConnectionState (output_connection_index_start_ and count_)
  //  - credits associated with the outputs
  output_connection_count_ = nc.GetOutputPortIds().size();
  output_connection_index_start_ =
      simulator.GetNewConnectionIndicesStore(output_connection_count_);
  absl::Span<int64> output_indices = simulator.GetConnectionIndicesStore(
      output_connection_index_start_, output_connection_count_);
  credit_.resize(output_connection_count_);
  credit_update_.resize(output_connection_count_);
  for (int64 i = 0; i < output_connection_count_; ++i) {
    XLS_ASSIGN_OR_RETURN(
        PortId port_id,
        port_indexer.GetPortByIndex(nc.id(), PortDirection::kOutput, i));
    Port& port = network_manager->GetPort(port_id);
    output_indices[i] = simulator.GetConnectionIndex(port.connection());

    XLS_ASSIGN_OR_RETURN(PortParam port_param,
                         simulator.GetNocParameters()->GetPortParam(port_id));
    credit_[i].resize(port_param.VirtualChannelCount(), 0);
    credit_update_[i].resize(port_param.VirtualChannelCount(),
                             CreditState{simulator.GetCurrentCycle(), 0});
  }

  internal_propagated_cycle_ = simulator.GetCurrentCycle();

  return absl::OkStatus();
}

bool SimLink::TryForwardPropagation(NocSimulator& simulator) {
  SimConnectionState& src =
      simulator.GetSimConnectionByIndex(src_connection_index_);
  SimConnectionState& sink =
      simulator.GetSimConnectionByIndex(sink_connection_index_);

  bool did_propagate = SimplePipelineImpl<TimedDataPhit>(
                           forward_pipeline_stages_, src.forward_channels,
                           sink.forward_channels, forward_data_stages_)
                           .TryPropagation(simulator);

  if (did_propagate) {
    XLS_LOG(INFO) << absl::StreamFormat(
        "Forward propagated from connection %x to %x", src.id.AsUInt64(),
        sink.id.AsUInt64());
    forward_propagated_cycle_ = simulator.GetCurrentCycle();
  }

  return did_propagate;
}

bool SimLink::TryReversePropagation(NocSimulator& simulator) {
  SimConnectionState& src =
      simulator.GetSimConnectionByIndex(src_connection_index_);
  SimConnectionState& sink =
      simulator.GetSimConnectionByIndex(sink_connection_index_);

  int64 vc_count = sink.reverse_channels.size();
  int64 num_propagated = 0;
  for (int64 vc = 0; vc < vc_count; ++vc) {
    if (SimplePipelineImpl<TimedMetadataPhit>(
            reverse_pipeline_stages_, sink.reverse_channels.at(vc),
            src.reverse_channels.at(vc), reverse_credit_stages_.at(vc))
            .TryPropagation(simulator)) {
      ++num_propagated;
      XLS_LOG(INFO) << absl::StreamFormat(
          "Reverse propagated from connection %x to %x", sink.id.AsUInt64(),
          src.id.AsUInt64());
    }
  }

  if (num_propagated == vc_count) {
    reverse_propagated_cycle_ = simulator.GetCurrentCycle();
    return true;
  } else {
    return false;
  }
}

bool SimNetworkInterfaceSrc::TryForwardPropagation(NocSimulator& simulator) {
  int64 current_cycle = simulator.GetCurrentCycle();
  SimConnectionState& sink =
      simulator.GetSimConnectionByIndex(sink_connection_index_);

  // Update credits.
  // No need to check for cycle here, because forward propagation
  // always succeeds and occurs before reverse propagation.
  // Sequence of operations is
  //  1. Credits are updated based off of prior cycle's received update
  //  2. Phits are sent va forward propagation.
  //  3. Reverse propagation updates the credit_update (for next cycle).
  for (int64 vc = 0; vc < credit_.size(); ++vc) {
    if (credit_update_[vc].credit > 0) {
      credit_[vc] += credit_update_[vc].credit;
      XLS_LOG(INFO) << absl::StrFormat(
          "... ni-src vc %d added credits %d, now %d", vc,
          credit_update_[vc].credit, credit_[vc]);
    }
  }

  // Send data.
  bool did_send_phit = false;

  for (int64 vc = 0; vc < data_to_send_.size(); ++vc) {
    std::queue<TimedDataPhit>& send_queue = data_to_send_[vc];
    if (!send_queue.empty() && send_queue.front().cycle <= current_cycle) {
      if (credit_[vc] > 0) {
        sink.forward_channels.phit = send_queue.front().phit;
        sink.forward_channels.phit.vc = vc;
        sink.forward_channels.phit.valid = true;
        sink.forward_channels.cycle = current_cycle;

        --credit_[vc];

        send_queue.pop();
        did_send_phit = true;

        XLS_LOG(INFO) << absl::StreamFormat(
            "... ni-src sending data %x vc %d credit now %d",
            sink.forward_channels.phit.data, vc, credit_[vc]);
        break;
      } else {
        XLS_LOG(INFO) << absl::StreamFormat(
            "... ni-src unable to send data %x vc %d credit %d",
            sink.forward_channels.phit.data, vc, credit_[vc]);
      }
    }
  }

  if (!did_send_phit) {
    sink.forward_channels.phit.valid = false;
    sink.forward_channels.phit.data = 0;
    sink.forward_channels.phit.vc = 0;
    sink.forward_channels.phit.destination_index = 0;
    sink.forward_channels.cycle = current_cycle;
  }

  forward_propagated_cycle_ = current_cycle;

  return true;
}

bool SimNetworkInterfaceSrc::TryReversePropagation(NocSimulator& simulator) {
  int64 current_cycle = simulator.GetCurrentCycle();
  SimConnectionState& sink =
      simulator.GetSimConnectionByIndex(sink_connection_index_);

  int64 vc_count = credit_update_.size();
  int64 num_propagated = 0;
  XLS_LOG(INFO) << absl::StreamFormat("... ni-src vc %d", vc_count);
  for (int64 vc = 0; vc < vc_count; ++vc) {
    TimedMetadataPhit possible_credit = sink.reverse_channels[vc];
    if (possible_credit.cycle == current_cycle) {
      if (credit_update_[vc].cycle != current_cycle) {
        credit_update_[vc].cycle = current_cycle;
        credit_update_[vc].credit =
            possible_credit.phit.valid ? possible_credit.phit.data : 0;

        XLS_LOG(INFO) << absl::StreamFormat(
            "... ni-src received credit %d vc %d via connection %x",
            credit_update_[vc].credit, vc, sink.id.AsUInt64());
      }

      XLS_LOG(INFO) << absl::StreamFormat(
          "... ni-src credit update cycle %x vc %d", credit_update_[vc].cycle,
          vc);

      ++num_propagated;
    }
  }

  if (num_propagated == vc_count) {
    XLS_LOG(INFO) << absl::StreamFormat(
        "... ni-src %x connected to %x finished reverse propagation",
        GetId().AsUInt64(), sink.id.AsUInt64());

    reverse_propagated_cycle_ = current_cycle;
    return true;
  } else {
    return false;
  }
}

absl::StatusOr<SimInputBufferedVCRouter::PortIndexAndVCIndex>
SimInputBufferedVCRouter::GetDestinationPortIndexAndVcIndex(
    NocSimulator& simulator, PortIndexAndVCIndex input,
    int64 destination_index) {
  DistributedRoutingTable* routes = simulator.GetRoutingTable();

  XLS_ASSIGN_OR_RETURN(PortId input_port,
                       routes->GetPortIndices().GetPortByIndex(
                           GetId(), PortDirection::kInput, input.port_index));

  PortAndVCIndex port_from{input_port, input.vc_index};

  XLS_ASSIGN_OR_RETURN(
      PortAndVCIndex port_to,
      routes->GetRouterOutputPortByIndex(port_from, destination_index));

  XLS_ASSIGN_OR_RETURN(int64 output_port_index,
                       routes->GetPortIndices().GetPortIndex(
                           port_to.port_id_, PortDirection::kOutput));

  return PortIndexAndVCIndex{output_port_index, port_to.vc_index_};
}

bool SimInputBufferedVCRouter::TryForwardPropagation(NocSimulator& simulator) {
  // TODO(tedhong): 2020-02-16 Factor out with strategy pattern.

  int64 current_cycle = simulator.GetCurrentCycle();
  absl::Span<int64> input_connection_index =
      simulator.GetConnectionIndicesStore(input_connection_index_start_,
                                          input_connection_count_);
  absl::Span<int64> output_connection_index =
      simulator.GetConnectionIndicesStore(output_connection_index_start_,
                                          output_connection_count_);

  // Update credits (for output ports)
  if (internal_propagated_cycle_ != current_cycle) {
    for (int64 i = 0; i < credit_update_.size(); ++i) {
      for (int64 vc = 0; vc < credit_update_[i].size(); ++vc) {
        if (credit_update_[i][vc].credit > 0) {
          credit_[i][vc] += credit_update_[i][vc].credit;
          XLS_LOG(INFO) << absl::StrFormat(
              "... router %x output port %d vc %d added credits %d, now %d",
              GetId().AsUInt64(), i, vc, credit_update_[i][vc].credit,
              credit_[i][vc]);
        } else {
          XLS_LOG(INFO) << absl::StrFormat(
              "... router %x output port %d vc %d did not add credits %d, now "
              "%d",
              GetId().AsUInt64(), i, vc, credit_update_[i][vc].credit,
              credit_[i][vc]);
        }
      }
    }

    internal_propagated_cycle_ = current_cycle;
  }

  // See if we can propagate forward.
  bool can_propagate_forward = true;
  for (int64 i = 0; i < input_connection_count_; ++i) {
    SimConnectionState& input =
        simulator.GetSimConnectionByIndex(input_connection_index[i]);

    if (input.forward_channels.cycle != current_cycle) {
      can_propagate_forward = false;
      break;
    }
  }

  if (!can_propagate_forward) {
    return false;
  }

  // Reset credits to send on reverse channel to 0.
  for (int64 i = 0; i < input_connection_count_; ++i) {
    for (int64 vc = 0; vc < input_credit_to_send_[i].size(); ++vc) {
      input_credit_to_send_[i][vc] = 0;
    }
  }

  // This router supports bypass so an phit arriving at the
  // input can be routed to the output immediately.
  for (int64 i = 0; i < input_connection_count_; ++i) {
    SimConnectionState& input =
        simulator.GetSimConnectionByIndex(input_connection_index[i]);

    if (input.forward_channels.phit.valid) {
      int64 vc = input.forward_channels.phit.vc;
      input_buffers_[i][vc].queue.push(input.forward_channels.phit);

      XLS_LOG(INFO) << absl::StrFormat(
          "... router %x from %x received data %x port %d vc %d",
          GetId().AsUInt64(), input.id.AsUInt64(),
          input.forward_channels.phit.data, i, vc);
    }
  }

  // Use fixed priority to route to output ports.
  // Priority goes to the port with the least vc and the least port index.
  for (int64 vc = 0; vc < max_vc_; ++vc) {
    for (int64 i = 0; i < input_buffers_.size(); ++i) {
      if (vc >= input_buffers_[i].size()) {
        continue;
      }

      // See if we have a flit to route and can route it.
      if (input_buffers_[i][vc].queue.empty()) {
        continue;
      }

      DataPhit phit = input_buffers_[i][vc].queue.front();
      int64 destination_index = phit.destination_index;

      PortIndexAndVCIndex input{i, vc};
      absl::StatusOr<PortIndexAndVCIndex> output_status =
          GetDestinationPortIndexAndVcIndex(simulator, input,
                                            destination_index);
      XLS_CHECK_OK(output_status.status());
      PortIndexAndVCIndex output = output_status.value();

      // Now see if we have sufficient credits.
      if (credit_.at(output.port_index).at(output.vc_index) <= 0) {
        XLS_LOG(INFO) << absl::StreamFormat(
            "... router unable to send data %x vc %d credit now %d"
            " from port index %d to port index %d.",
            phit.data, phit.vc,
            credit_.at(output.port_index).at(output.vc_index), i,
            output.port_index);
        continue;
      }

      // Check that no other port has already used the output port
      // (since this is a router without output buffers.
      SimConnectionState& output_state = simulator.GetSimConnectionByIndex(
          output_connection_index.at(output.port_index));
      if (output_state.forward_channels.cycle == current_cycle) {
        continue;
      }

      // Now send the flit along.
      output_state.forward_channels.phit = phit;
      output_state.forward_channels.phit.valid = true;
      output_state.forward_channels.phit.vc = output.vc_index;
      output_state.forward_channels.cycle = current_cycle;

      // Update credit on output.
      --credit_.at(output.port_index).at(output.vc_index);

      // Update credit to send back to input.
      ++input_credit_to_send_[i][vc];
      input_buffers_[i][vc].queue.pop();

      XLS_LOG(INFO) << absl::StreamFormat(
          "... router sending data %x vc %d credit now %d"
          " from port index %d to port index %d on %x.",
          output_state.forward_channels.phit.data,
          output_state.forward_channels.phit.vc,
          credit_.at(output.port_index).at(output.vc_index), i,
          output.port_index, output_state.id.AsUInt64());
    }
  }

  // Now put bubbles in output ports that couldn't send data.
  for (int64 i = 0; i < output_connection_index.size(); ++i) {
    SimConnectionState& output =
        simulator.GetSimConnectionByIndex(output_connection_index[i]);
    if (output.forward_channels.cycle != current_cycle) {
      output.forward_channels.cycle = current_cycle;
      output.forward_channels.phit.valid = false;
      output.forward_channels.phit.data = 0;
      output.forward_channels.phit.vc = 0;
      output.forward_channels.phit.destination_index = 0;
    }
  }

  forward_propagated_cycle_ = current_cycle;

  return true;
}

bool SimInputBufferedVCRouter::TryReversePropagation(NocSimulator& simulator) {
  int64 current_cycle = simulator.GetCurrentCycle();

  // Reverse propagation occurs only after forward propagation.
  if (forward_propagated_cycle_ != current_cycle) {
    return false;
  }

  absl::Span<int64> input_connection_index =
      simulator.GetConnectionIndicesStore(input_connection_index_start_,
                                          input_connection_count_);

  // Send credit upstream.
  for (int64 i = 0; i < input_connection_count_; ++i) {
    SimConnectionState& input =
        simulator.GetSimConnectionByIndex(input_connection_index[i]);

    for (int64 vc = 0; vc < input.reverse_channels.size(); ++vc) {
      input.reverse_channels[vc].phit.valid = true;

      // Upon reset (cycle-0) a full update of credits is sent.
      if (current_cycle == 0) {
        input.reverse_channels[vc].phit.data =
            input_buffers_[i][vc].max_queue_size;
      } else {
        input.reverse_channels[vc].phit.data = input_credit_to_send_[i][vc];
      }
      input.reverse_channels[vc].cycle = current_cycle;

      XLS_LOG(INFO) << absl::StreamFormat(
          "... router %x sending credit update %d"
          " input port %d vc %d connection %x",
          GetId().AsUInt64(), input.reverse_channels[vc].phit.data, i, vc,
          input.id.AsUInt64());
    }
  }

  // Recieve credit from downstream.
  absl::Span<int64> output_connection_index =
      simulator.GetConnectionIndicesStore(output_connection_index_start_,
                                          output_connection_count_);

  int64 num_propagated = 0;
  int64 possible_propagation = 0;
  for (int64 i = 0; i < credit_update_.size(); ++i) {
    SimConnectionState& output =
        simulator.GetSimConnectionByIndex(output_connection_index.at(i));

    for (int64 vc = 0; vc < credit_update_[i].size(); ++vc) {
      TimedMetadataPhit possible_credit = output.reverse_channels[vc];

      if (possible_credit.cycle == current_cycle) {
        if (credit_update_[i][vc].cycle != current_cycle) {
          credit_update_[i][vc].cycle = current_cycle;
          credit_update_[i][vc].credit =
              possible_credit.phit.valid ? possible_credit.phit.data : 0;

          XLS_LOG(INFO) << absl::StreamFormat(
              "... router received credit %d output port %d vc %d via "
              "connection %x",
              credit_update_[i][vc].credit, i, vc, output.id.AsUInt64());
        }

        ++num_propagated;
      } else {
        XLS_LOG(INFO) << absl::StreamFormat(
            "... router output port %d vc %d waiting for credits via "
            "connection %x",
            i, vc, output.id.AsUInt64());
      }

      ++possible_propagation;
    }
  }

  if (possible_propagation == num_propagated) {
    XLS_LOG(INFO) << absl::StreamFormat(
        "... router %x finished reverse propagation", GetId().AsUInt64());
    return true;
  } else {
    XLS_LOG(INFO) << absl::StreamFormat(
        "... router %x did not finish reverse propagation", GetId().AsUInt64());
    return false;
  }
}

bool SimNetworkInterfaceSink::TryForwardPropagation(NocSimulator& simulator) {
  int64 current_cycle = simulator.GetCurrentCycle();

  SimConnectionState& src =
      simulator.GetSimConnectionByIndex(src_connection_index_);

  if (src.forward_channels.cycle != current_cycle) {
    return false;
  }

  if (src.forward_channels.phit.valid) {
    int64 data = src.forward_channels.phit.data;
    int64 vc = src.forward_channels.phit.vc;

    // TODO(tedhong): 2021-01-31 Support blocking traffic at sink.
    // without blocking, the queue never gets empty so we don't
    // emplace into input_buffers_[vc].queue.
    TimedDataPhit received_phit;
    received_phit.cycle = current_cycle;
    received_phit.phit = src.forward_channels.phit;
    received_traffic_.push_back(received_phit);

    // Send one credit back
    src.reverse_channels[vc].cycle = current_cycle;
    src.reverse_channels[vc].phit.valid = true;
    src.reverse_channels[vc].phit.data = 1;

    XLS_LOG(INFO) << absl::StreamFormat(
        "... sink %x received data %x on vc %d cycle %d, sending 1 credit on "
        "%x",
        GetId().AsUInt64(), data, vc, current_cycle, src.id.AsUInt64());
  }

  // In cycle 0, a full credit update is sent
  if (current_cycle == 0) {
    for (int64 vc = 0; vc < src.reverse_channels.size(); ++vc) {
      src.reverse_channels[vc].cycle = current_cycle;
      src.reverse_channels[vc].phit.valid = true;
      src.reverse_channels[vc].phit.data = input_buffers_[vc].max_queue_size;

      XLS_LOG(INFO) << absl::StreamFormat(
          "... sink %x sending %d credit vc %d on %x", GetId().AsUInt64(),
          input_buffers_[vc].max_queue_size, vc, src.id.AsUInt64());
    }
  } else {
    for (int64 vc = 0; vc < src.reverse_channels.size(); ++vc) {
      if (src.reverse_channels[vc].cycle != current_cycle) {
        src.reverse_channels[vc].cycle = current_cycle;
        src.reverse_channels[vc].phit.valid = false;
        src.reverse_channels[vc].phit.data = 0;
      }
    }
  }

  return true;
}

absl::StatusOr<SimNetworkInterfaceSrc*> NocSimulator::GetSimNetworkInterfaceSrc(
    NetworkComponentId src) {
  auto iter = src_index_map_.find(src);
  if (iter == src_index_map_.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Unable to find sim object for"
                        " network interface src %x",
                        src.AsUInt64()));
  }

  return &network_interface_sources_[iter->second];
}

absl::StatusOr<SimNetworkInterfaceSink*>
NocSimulator::GetSimNetworkInterfaceSink(NetworkComponentId sink) {
  auto iter = sink_index_map_.find(sink);
  if (iter == sink_index_map_.end()) {
    return absl::NotFoundError(
        absl::StrFormat("Unable to find sim object for"
                        " network interface src %x",
                        sink.AsUInt64()));
  }

  return &network_interface_sinks_[iter->second];
}

}  // namespace noc
}  // namespace xls