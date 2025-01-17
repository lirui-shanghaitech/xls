// Copyright 2020 The XLS Authors
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

#include "xls/noc/config/common_network_config_builder_options_proto_builder.h"

namespace xls::noc {

FlowControlOptionsProtoBuilder&
FlowControlOptionsProtoBuilder::EnablePeekFlowControl() {
  proto_->mutable_peek();
  return *this;
}

FlowControlOptionsProtoBuilder&
FlowControlOptionsProtoBuilder::EnableTokenCreditBasedFlowControl() {
  proto_->mutable_token_credit_based();
  return *this;
}

FlowControlOptionsProtoBuilder&
FlowControlOptionsProtoBuilder::EnableTotalCreditBasedFlowControl(
    int64_t credit_bit_width) {
  proto_->mutable_total_credit_based()->set_credit_bit_width(credit_bit_width);
  return *this;
}

}  // namespace xls::noc
