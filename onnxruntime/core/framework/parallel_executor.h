// Copyright (c) Microsoft Corporation. All rights reserved.
// Licensed under the MIT License.

#pragma once

#include "core/common/nsyncHelper.h"
#include "core/common/common.h"
#include "core/common/status.h"
#include "core/common/logging/logging.h"
#include "core/framework/iexecutor.h"
#include "core/framework/framework_common.h"
#include "core/framework/ml_value.h"
#include "core/framework/session_state.h"
#include "core/graph/graph_viewer.h"

namespace onnxruntime {

class ExecutionFrame;

class ParallelExecutor : public IExecutor {
 public:
  ParallelExecutor(const bool& terminate_flag = false) : terminate_flag_{terminate_flag}, complete_mutex_{0, 0}, ref_mutex_{0, 0} {}
  ParallelExecutor(const SessionState& session_state, const bool& terminate_flag = false);

  common::Status Execute(const SessionState& session_state,
                         const NameMLValMap& feeds,
                         const std::vector<std::string>& output_names,
                         std::vector<MLValue>& fetches,
                         const logging::Logger& logger) override;

 private:
  ORT_DISALLOW_COPY_ASSIGNMENT_AND_MOVE(ParallelExecutor);

  void RunNodeAsync(size_t p_node_index, const SessionState& session_state, const logging::Logger& logger);
  void RunNodeAsyncInternal(size_t p_node_index, const SessionState& session_state, const logging::Logger& logger);

  void EnqueueNode(size_t p_node_index, const SessionState& session_state, const logging::Logger& logger);

  Status FetchOutput(const MLValueNameIdxMap& name_idx_map,
                     ExecutionFrame& frame,
                     const std::vector<std::string>& output_names,
                     std::vector<MLValue>& fetches,
                     const logging::Logger& logger);

  void FinishNodeRun() {
    //Because we have a mutex here, it's not possible another thread is doing the test("while (out_standings_ > 0)"
    NsyncLockGuard lock(&complete_mutex_);
    --out_standings_;
  }

  std::unique_ptr<ExecutionFrame> root_frame_;
  std::vector<size_t> node_refs_;
  nsync::nsync_mu ref_mutex_;
  int out_standings_;  //protected by complete_mutex_
  nsync::nsync_mu complete_mutex_;

  const bool& terminate_flag_;
};

}  // namespace onnxruntime
