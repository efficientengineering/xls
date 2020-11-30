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

#include "xls/noc/config/virtual_channel_config_proto_builder.h"

namespace xls::noc {

VirtualChannelConfigProtoBuilder& VirtualChannelConfigProtoBuilder::WithName(
    absl::string_view name) {
  proto_->set_name(name);
  return *this;
}

VirtualChannelConfigProtoBuilder&
VirtualChannelConfigProtoBuilder::WithFlitBitWidth(int64 flit_bit_width) {
  proto_->set_flit_bit_width(flit_bit_width);
  return *this;
}

VirtualChannelConfigProtoBuilder& VirtualChannelConfigProtoBuilder::WithDepth(
    int64 depth) {
  proto_->set_depth(depth);
  return *this;
}

}  // namespace xls::noc
