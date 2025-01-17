// Copyright 2023 The XLS Authors
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

#ifndef XLS_SCHEDULING_PROC_CLUMPING_PASS_H_
#define XLS_SCHEDULING_PROC_CLUMPING_PASS_H_

#include "absl/status/statusor.h"
#include "xls/scheduling/scheduling_pass.h"

namespace xls {

// Pass that combines logical pipeline stages together to form physical pipeline
// stages via temporal multiplexing, allowing for generation of II > 1
// pipelines.
class ProcClumpingPass : public SchedulingPass {
 public:
  ProcClumpingPass()
      : SchedulingPass("proc_clumping",
                       "Converts a pipeline with multicycle paths "
                       "into one with single cycle paths using "
                       "temporal multiplexing.") {}
  ~ProcClumpingPass() override = default;

 protected:
  absl::StatusOr<bool> RunInternal(
      SchedulingUnit<>* unit, const SchedulingPassOptions& options,
      SchedulingPassResults* results) const override;
};

}  // namespace xls

#endif  // XLS_SCHEDULING_PROC_CLUMPING_PASS_H_
