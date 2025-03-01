/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/service/gpu/nccl_all_to_all_thunk.h"

#include <cstdint>
#include <cstdlib>
#include <optional>
#include <utility>
#include <vector>

#include "absl/status/status.h"
#include "absl/strings/str_format.h"
#include "absl/strings/substitute.h"
#include "mlir/IR/Value.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/collective_ops_utils.h"
#include "xla/service/gpu/ir_emission_utils.h"
#include "xla/service/gpu/nccl_collective_thunk.h"
#include "xla/shape.h"
#include "xla/shape_util.h"
#include "tsl/platform/errors.h"

#if XLA_ENABLE_XCCL
#include "xla/stream_executor/gpu/gpu_stream.h"
#endif

namespace xla {
namespace gpu {

using mlir::lmhlo_gpu::AllToAllStartOp;

namespace {

NcclAllToAllConfig GetNcclAllToAllConfig(AllToAllStartOp op) {
  NcclAllToAllConfig config;
  // FIXME(b/180174349): LMHLO AllToAll incorrectly has use_global_device_ids
  // attribute and it should be removed.
  config.config = GetNcclCollectiveConfigForMlir(op, std::nullopt);
  config.has_split_dimension = op.getSplitDimension().has_value();
  return config;
}

NcclAllToAllConfig GetNcclAllToAllConfig(const HloAllToAllInstruction* instr) {
  NcclAllToAllConfig config;
  // FIXME(b/180174349): LMHLO AllToAll incorrectly has use_global_device_ids
  // attribute and it should be removed.
  config.config = GetNcclCollectiveConfig(instr, std::nullopt);
  config.has_split_dimension = instr->split_dimension().has_value();
  return config;
}

}  // namespace

NcclAllToAllStartThunk::NcclAllToAllStartThunk(
    ThunkInfo thunk_info, AllToAllStartOp op,
    std::vector<NcclCollectiveThunk::Buffer> buffers)
    : NcclCollectiveThunk(Thunk::kNcclAllToAllStart, thunk_info,
                          op.getIsSync()),
      config_(GetNcclAllToAllConfig(op)),
      buffers_(std::move(buffers)) {
  CHECK_EQ(config_.config.operand_count, buffers_.size());
}

NcclAllToAllStartThunk::NcclAllToAllStartThunk(
    ThunkInfo thunk_info, const HloAllToAllInstruction* instr,
    std::vector<NcclCollectiveThunk::Buffer> buffers)
    : NcclCollectiveThunk(Thunk::kNcclAllToAllStart, thunk_info,
                          IsSyncCollective(instr)),
      config_(GetNcclAllToAllConfig(instr)),
      buffers_(std::move(buffers)) {
  CHECK_EQ(config_.config.operand_count, buffers_.size());
}

/*static*/ absl::Status NcclAllToAllStartThunk::CheckImplementable(
    AllToAllStartOp op, int64_t replica_count, int64_t partition_count) {
  auto status = [&]() -> absl::Status {
    TF_RETURN_IF_ERROR(NcclCollectiveThunk::CheckImplementable());
    std::optional<uint64_t> split_dim = op.getSplitDimension();
    for (mlir::Value operand : op.getInputs()) {
      TF_RETURN_IF_ERROR(IsValidOperand(operand, Thunk::kNcclAllToAll));
      Shape shape = GetShape(operand);
      if (split_dim &&
          !ShapeUtil::IsEffectivelyMostMajorDimension(shape, *split_dim)) {
        return absl::UnimplementedError(absl::Substitute(
            "all-to-all split dim $0 is not the most major in input shape $1",
            *split_dim, shape.ToString(/*print_layout=*/true)));
      }
    }
    return absl::OkStatus();
  };
  return AddOpDescription<NcclAllToAllStartThunk>(status(), op, replica_count,
                                                  partition_count);
}

/*static*/ absl::Status NcclAllToAllStartThunk::CheckImplementable(
    const HloAllToAllInstruction* instr, int64_t replica_count,
    int64_t partition_count) {
  auto status = [&instr]() -> absl::Status {
    TF_RETURN_IF_ERROR(NcclCollectiveThunk::CheckImplementable());
    std::optional<uint64_t> split_dim = instr->split_dimension();
    for (HloInstruction* operand : instr->operands()) {
      Shape shape = operand->shape();
      TF_RETURN_IF_ERROR(IsValidOperand(shape, Thunk::kNcclAllToAll));
      if (split_dim &&
          !ShapeUtil::IsEffectivelyMostMajorDimension(shape, *split_dim)) {
        return absl::UnimplementedError(absl::Substitute(
            "all-to-all split dim $0 is not the most major in input shape $1",
            *split_dim, shape.ToString(/*print_layout=*/true)));
      }
    }
    return absl::OkStatus();
  };
  return AddOpDescription<NcclAllToAllStartThunk>(
      status(), instr, replica_count, partition_count);
}

/*static*/ CollectiveOpGroupMode NcclAllToAllStartThunk::GetGroupMode(
    AllToAllStartOp op) {
  return GetNcclAllToAllConfig(op).config.group_mode;
}
/*static*/ CollectiveOpGroupMode NcclAllToAllStartThunk::GetGroupMode(
    const HloAllToAllInstruction* instr) {
  return GetNcclAllToAllConfig(instr).config.group_mode;
}

absl::Status NcclAllToAllStartThunk::RunNcclCollective(
    const ExecuteParams& params, se::Stream& stream, ncclComm_t comm) {
  TF_ASSIGN_OR_RETURN(
      std::vector<DeviceBufferPair> device_buffers,
      ConvertToDeviceBuffers(params, buffers_,
                             config_.config.operand_element_type));
  return xla::gpu::RunAllToAll(config_.has_split_dimension, device_buffers,
                               stream, comm);
}

absl::Status RunAllToAll(bool has_split_dimension,
                         std::vector<DeviceBufferPair>& buffers,
                         se::Stream& stream, ncclComm_t comm) {
#if XLA_ENABLE_XCCL
  int device_ordinal = stream.parent()->device_ordinal();
  VLOG(3) << "Performing all-to-all from device ordinal: " << device_ordinal;

  se::gpu::GpuStreamHandle gpu_stream = se::gpu::AsGpuStreamValue(&stream);

  int num_participants;
  XLA_NCCL_RETURN_IF_ERROR(ncclCommCount(comm, &num_participants));

  XLA_NCCL_RETURN_IF_ERROR(ncclGroupStart());
  // AllToAll can operate in two modes. Either it specifies a split dimension,
  // in which case inputs are split and outputs concatenated in that dimension
  // (here, we only support dimension 0), or it takes a list of inputs
  // and produces a tuple of outputs.
  if (has_split_dimension) {
    for (size_t i = 0; i < buffers.size(); ++i) {
      DeviceBufferPair& buffer = buffers[i];
      const uint8_t* send_buffer =
          static_cast<uint8_t*>(buffer.source_buffer.opaque());
      uint8_t* recv_buffer =
          static_cast<uint8_t*>(buffer.destination_buffer.opaque());

      TF_ASSIGN_OR_RETURN(auto dtype_and_multiplier,
                          ToNcclDataTypeAndCountMultiplier(
                              buffer.element_type, Thunk::kNcclAllToAll));
      auto [dtype, multiplier] = dtype_and_multiplier;
      int64_t element_count = buffer.element_count;

      TF_RET_CHECK(element_count % num_participants == 0)
          << "Buffer was not an exact multiple of the number of participants.";
      size_t chunk_elements = element_count / num_participants;
      size_t chunk_bytes = chunk_elements * ShapeUtil::ByteSizeOfPrimitiveType(
                                                buffer.element_type);

      for (int rank = 0; rank < num_participants; ++rank) {
        VLOG(3) << absl::StreamFormat(
            "Calling ncclSend(sendbuff=%p, count=%d, peer=%d "
            "comm=%p, stream=%p)",
            send_buffer + rank * chunk_bytes, chunk_elements * multiplier, rank,
            static_cast<const void*>(comm), gpu_stream);
        XLA_NCCL_RETURN_IF_ERROR(ncclSend(send_buffer + rank * chunk_bytes,
                                          chunk_elements * multiplier, dtype,
                                          rank, comm, gpu_stream));

        VLOG(3) << absl::StreamFormat(
            "Calling ncclRecv(recvbuff=%p, count=%d, peer=%d "
            "comm=%p, stream=%p)",
            recv_buffer + rank * chunk_bytes, chunk_elements * multiplier, rank,
            static_cast<const void*>(comm), gpu_stream);

        XLA_NCCL_RETURN_IF_ERROR(ncclRecv(recv_buffer + rank * chunk_bytes,
                                          chunk_elements * multiplier, dtype,
                                          rank, comm, gpu_stream));
      }
    }
  } else {
    TF_RET_CHECK(buffers.size() == num_participants)
        << "Number of inputs didn't match the number of participants.";

    for (size_t i = 0; i < buffers.size(); ++i) {
      DeviceBufferPair& buffer = buffers[i];
      const uint8_t* send_buffer =
          static_cast<uint8_t*>(buffer.source_buffer.opaque());
      uint8_t* recv_buffer =
          static_cast<uint8_t*>(buffer.destination_buffer.opaque());

      TF_ASSIGN_OR_RETURN(auto dtype_and_multiplier,
                          ToNcclDataTypeAndCountMultiplier(
                              buffer.element_type, Thunk::kNcclAllToAll));
      auto [dtype, multiplier] = dtype_and_multiplier;
      int64_t element_count = buffer.element_count * multiplier;

      VLOG(3) << absl::StreamFormat(
          "Calling ncclSend(sendbuff=%p, count=%d, peer=%d "
          "comm=%p, stream=%p)",
          send_buffer, element_count, i, static_cast<const void*>(comm),
          gpu_stream);

      XLA_NCCL_RETURN_IF_ERROR(ncclSend(send_buffer, element_count, dtype,
                                        /*rank=*/i, comm, gpu_stream));

      VLOG(3) << absl::StreamFormat(
          "Calling ncclRecv(recvbuff=%p, count=%d, peer=%d "
          "comm=%p, stream=%p)",
          recv_buffer, element_count, i, static_cast<const void*>(comm),
          gpu_stream);

      XLA_NCCL_RETURN_IF_ERROR(ncclRecv(recv_buffer, element_count, dtype,
                                        /*rank=*/i, comm, gpu_stream));
    }
  }
  XLA_NCCL_RETURN_IF_ERROR(ncclGroupEnd());

  VLOG(3) << "Done performing all-to-all for ordinal: " << device_ordinal;
  return absl::OkStatus();
#else   // XLA_ENABLE_XCCL
  return Unimplemented(
      "NCCL support is not available: this binary was not built with a CUDA "
      "compiler, which is necessary to build the NCCL source library.");
#endif  // XLA_ENABLE_XCCL
}

}  // namespace gpu
}  // namespace xla
