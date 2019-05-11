/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

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

#include <memory>

#include "tensorflow/lite/c/builtin_op_data.h"
#include "tensorflow/lite/c/c_api_internal.h"
#include "tensorflow/lite/kernels/internal/optimized/optimized_ops.h"
#include "tensorflow/lite/kernels/internal/quantization_util.h"
#include "tensorflow/lite/kernels/internal/reference/reference_ops.h"
#include "tensorflow/lite/kernels/internal/tensor.h"
#include "tensorflow/lite/kernels/kernel_util.h"
#include "tensorflow/lite/kernels/op_macros.h"

namespace tflite {
namespace ops {
namespace builtin {
namespace mirror_pad {
namespace {

// Simple class that represents a mirror padded tensor - which is the output
// from the Op.
struct PaddedTensor {
  // If not null that means this is a scalar value.
  // Note: This is not owned by default. It will point to the value
  // in the input tensor.
  const void* value = nullptr;
  // If this tensor is not one value, then this vector will have
  // all the tensors that belongs to this tensor.
  // Pointers are not owned.
  std::vector<PaddedTensor*> values;
  // Pointers to PaddedTensors that are padded on the left of the current
  // tensor.
  std::vector<PaddedTensor*> left_pad_ptrs;
  // Pointers to PaddedTensors that are padded on the right of the current
  // tensor.
  std::vector<PaddedTensor*> right_pad_ptrs;

  // Returns mutable pointer to the tensor identified by 'indices'.
  PaddedTensor* GetMutable(const std::vector<int>& indices) {
    auto* result = this;
    for (int i = 0; i < indices.size(); ++i) {
      if (indices[i] >= result->values.size()) {
        return nullptr;
      }
      result = result->values[indices[i]];
      if (result == nullptr) break;
    }
    return result;
  }
};

// Wrapper for all intermediate data used by the op.
struct OpData {
  // Holds intermediate data structure of the padded tensor.
  std::vector<PaddedTensor> pad_tensor_buffer;
  // Total number of intermediate elements in the pad_tensor_buffer.
  int num_elements;
};

// Util method to initialize the memory of the padded tensor.
void InitializeTensorMemory(const TfLiteIntArray* const dims, int dims_size,
                            std::vector<PaddedTensor>* padded_tensor_buffer) {
  int dimension_index = 0;
  int element_index = 0;
  // We hold 2 vectors with values for nodes in current level, and
  // nodes in the next level, and swap while moving on dimensions of the tensor.
  std::vector<PaddedTensor*> current_nodes, next_level;
  current_nodes.push_back(&(*padded_tensor_buffer)[element_index]);
  element_index++;
  int next_level_size = 1;
  while (!current_nodes.empty() && dimension_index < dims_size) {
    next_level_size *= dims->data[dimension_index];
    next_level.resize(next_level_size);
    // Index of elements in next level.
    int index = 0;
    for (auto* padded_tensor : current_nodes) {
      padded_tensor->values.resize(dims->data[dimension_index]);
      for (int i = 0; i < dims->data[dimension_index]; ++i) {
        padded_tensor->values[i] = &(*padded_tensor_buffer)[element_index];
        next_level[index++] = padded_tensor->values[i];
        element_index++;
      }
    }
    std::swap(current_nodes, next_level);
    dimension_index++;
  }
}

// Returns pointer to the value at the specified index in 'data'.
inline const void* GetValuePointerAtIndex(const void* data, int index,
                                          const TfLiteType data_type) {
  switch (data_type) {
    case kTfLiteFloat32:
      return static_cast<const float*>(data) + index;
    case kTfLiteInt32:
      return static_cast<const int32_t*>(data) + index;
    case kTfLiteUInt8:
      return static_cast<const uint8_t*>(data) + index;
    case kTfLiteInt64:
      return static_cast<const int64_t*>(data) + index;
    case kTfLiteBool:
      return static_cast<const bool*>(data) + index;
    case kTfLiteInt16:
      return static_cast<const int16_t*>(data) + index;
    case kTfLiteInt8:
      return static_cast<const int8_t*>(data) + index;
    // Unsupported types ?
    default:
      return nullptr;
  }
  return nullptr;
}

// Fills the 'padded_tensor' with data from 'input_tensor'.
TfLiteStatus InitFromInputTensor(const TfLiteTensor* input_tensor,
                                 PaddedTensor* padded_tensor) {
  const auto* dims = input_tensor->dims;
  const auto data_type = input_tensor->type;
  const void* data = static_cast<const void*>(input_tensor->data.raw_const);
  // Either invalid input or unsupported type.+
  if (data == nullptr) {
    return kTfLiteError;
  }
  // Index of current processing tensor.
  std::vector<int> tensor_index(dims->size, 0);
  int flat_index = 0;
  const int num_elements = NumElements(input_tensor);
  auto* tensor = padded_tensor->GetMutable(tensor_index);
  while (flat_index < num_elements) {
    if (tensor == nullptr) {
      return kTfLiteError;
    }
    tensor->value = GetValuePointerAtIndex(data, flat_index, data_type);
    ++tensor;
    ++flat_index;
  }

  return kTfLiteOk;
}

template <typename T>
inline void GetPadding(const T* data, int offset, int64_t* left_pad,
                       int64_t* right_pad) {
  *left_pad = static_cast<int64_t>(*(data + offset * 2));
  *right_pad = static_cast<int64_t>(*(data + offset * 2 + 1));
}

inline TfLiteStatus GetPadding(const TfLiteTensor* padding_matrix,
                               int dimension, int64_t* left_pad,
                               int64_t* right_pad) {
  switch (padding_matrix->type) {
    case kTfLiteInt32:
      GetPadding(padding_matrix->data.i32, dimension, left_pad, right_pad);
      break;
    case kTfLiteInt64:
      GetPadding(padding_matrix->data.i64, dimension, left_pad, right_pad);
      break;
    default:
      return kTfLiteError;
  }
  return kTfLiteOk;
}

TfLiteStatus ValidateTensor(const TfLiteTensor* padding_matrix, int offset,
                            int dimension_index, PaddedTensor* padded_tensor,
                            TfLiteContext* context) {
  if (dimension_index >= padding_matrix->dims->data[0]) {
    return kTfLiteOk;
  }

  int64_t left_pad = 0, right_pad = 0;
  TF_LITE_ENSURE_STATUS(
      GetPadding(padding_matrix, dimension_index, &left_pad, &right_pad));
  // If we are not going to include border we must have enough values
  // to use.
  if (left_pad + offset > padded_tensor->values.size()) {
    context->ReportError(
        context, "Not enough values for Mirror Pad, required %d, available %d.",
        left_pad + offset, padded_tensor->values.size());
    return kTfLiteError;
  }
  if (right_pad + offset > padded_tensor->values.size()) {
    context->ReportError(
        context, "Not enough values for Mirror Pad, required %d, available %d.",
        right_pad + offset, padded_tensor->values.size());
    return kTfLiteError;
  }
  if (!padded_tensor->values.empty()) {
    ValidateTensor(padding_matrix, offset, dimension_index + 1,
                   padded_tensor->values[0], context);
  }
  return kTfLiteOk;
}

// Fills 'padded_tensor' with the padding information based on
// 'padding_matrix'.
// 'dimension_index' represents which dimension the function is operating on.
TfLiteStatus PadTensor(const TfLiteTensor* padding_matrix, int offset,
                       int dimension_index, PaddedTensor* padded_tensor,
                       TfLiteContext* context) {
  if (dimension_index >= padding_matrix->dims->data[0]) return kTfLiteOk;

  int64_t left_pad = 0, right_pad = 0;
  TF_LITE_ENSURE_STATUS(
      GetPadding(padding_matrix, dimension_index, &left_pad, &right_pad));

  padded_tensor->left_pad_ptrs.clear();
  for (int i = left_pad + offset - 1; i >= offset && left_pad > 0;
       --i, --left_pad) {
    padded_tensor->left_pad_ptrs.push_back(padded_tensor->values[i]);
  }
  padded_tensor->right_pad_ptrs.clear();
  for (int i = padded_tensor->values.size() - (1 + offset);
       i >= 0 && right_pad > 0; --i, --right_pad) {
    padded_tensor->right_pad_ptrs.push_back(padded_tensor->values[i]);
  }

  for (auto& tensor : padded_tensor->values) {
    TF_LITE_ENSURE_STATUS(PadTensor(padding_matrix, offset, dimension_index + 1,
                                    tensor, context));
  }
  return kTfLiteOk;
}

// Fills 'output_data' with data from 'padded_tensor'.
// The function does this recursively by setting left padding first then
// original data, followed by the right padding.
template <typename T>
int FillOutput(const PaddedTensor* padded_tensor, T* output_data,
               int index_in_output) {
  if (padded_tensor == nullptr || output_data == nullptr) {
    return -1;
  }
  if (padded_tensor->value != nullptr) {
    output_data[index_in_output] = *static_cast<const T*>(padded_tensor->value);
    return index_in_output + 1;
  }
  for (const auto* tensor : padded_tensor->left_pad_ptrs) {
    index_in_output = FillOutput(tensor, output_data, index_in_output);
  }
  for (const auto& tensor : padded_tensor->values) {
    index_in_output = FillOutput(tensor, output_data, index_in_output);
  }
  for (const auto* tensor : padded_tensor->right_pad_ptrs) {
    index_in_output = FillOutput(tensor, output_data, index_in_output);
  }
  return index_in_output;
}

// Returns the shape of the final output after padding.
std::unique_ptr<TfLiteIntArray, void (*)(TfLiteIntArray*)> GetPaddedOutputShape(
    const TfLiteTensor* input, const TfLiteTensor* padding_matrix) {
  const int input_dims = NumDimensions(input);
  std::unique_ptr<TfLiteIntArray, void (*)(TfLiteIntArray*)> shape(
      TfLiteIntArrayCreate(input_dims), TfLiteIntArrayFree);

  int64_t left_pad = 0, right_pad = 0;
  for (int i = 0; i < input_dims; ++i) {
    GetPadding(padding_matrix, i, &left_pad, &right_pad);
    shape->data[i] = SizeOfDimension(input, i) + left_pad + right_pad;
  }
  return shape;
}

}  // namespace

TfLiteStatus Eval(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input_tensor = GetInput(context, node, 0);
  const TfLiteTensor* padding_matrix = GetInput(context, node, 1);
  auto* params =
      reinterpret_cast<TfLiteMirrorPaddingParams*>(node->builtin_data);
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);

  if (params == nullptr) {
    return kTfLiteError;
  }
  const int input_dims = NumDimensions(input_tensor);

  TfLiteTensor* output_tensor = GetOutput(context, node, 0);
  if (IsDynamicTensor(output_tensor)) {
    auto output_size = GetPaddedOutputShape(input_tensor, padding_matrix);
    if (output_size == nullptr) {
      return kTfLiteError;
    }
    TF_LITE_ENSURE_STATUS(
        context->ResizeTensor(context, output_tensor, output_size.release()));
  }

  PaddedTensor& padded_tensor = op_data->pad_tensor_buffer[0];
  // Initialize memory.
  InitializeTensorMemory(input_tensor->dims, input_dims,
                         &op_data->pad_tensor_buffer);
  // Set the values from the input_tensor.
  TF_LITE_ENSURE_STATUS(InitFromInputTensor(input_tensor, &padded_tensor));
  const int offset =
      params->mode != TfLiteMirrorPaddingMode::kTfLiteMirrorPaddingReflect ? 0
                                                                           : 1;
  // Make sure padding values are sufficient and valid to use.
  TF_LITE_ENSURE_STATUS(
      ValidateTensor(padding_matrix, offset, 0, &padded_tensor, context));
  // Apply padding.
  TF_LITE_ENSURE_STATUS(
      PadTensor(padding_matrix, offset, 0, &padded_tensor, context));

  // Fill the output tensor from the padded tensor.
  TfLiteStatus status = kTfLiteOk;

#define TF_LITE_MIRROR_PAD(type) \
  FillOutput(&padded_tensor, GetTensorData<type>(output_tensor), 0);

  switch (output_tensor->type) {
    case kTfLiteFloat32: {
      TF_LITE_MIRROR_PAD(float);
      break;
    }
    case kTfLiteInt32: {
      TF_LITE_MIRROR_PAD(int32_t);
      break;
    }
    case kTfLiteUInt8: {
      TF_LITE_MIRROR_PAD(uint8_t);
      break;
    }
    case kTfLiteInt64: {
      TF_LITE_MIRROR_PAD(int64_t);
      break;
    }
    default:
      status = kTfLiteError;
      break;
  }
#undef TF_LITE_MIRROR_PAD
  return status;
}

void* Init(TfLiteContext* context, const char* buffer, size_t length) {
  return new OpData();
}

void Free(TfLiteContext* context, void* buffer) {
  delete reinterpret_cast<OpData*>(buffer);
}

TfLiteStatus Prepare(TfLiteContext* context, TfLiteNode* node) {
  const TfLiteTensor* input_tensor = GetInput(context, node, 0);
  const TfLiteTensor* padding_matrix = GetInput(context, node, 1);
  TfLiteTensor* output_tensor = GetOutput(context, node, 0);
  OpData* op_data = reinterpret_cast<OpData*>(node->user_data);

  TF_LITE_ENSURE_EQ(context, NumDimensions(padding_matrix), 2);
  TF_LITE_ENSURE_EQ(context, SizeOfDimension(padding_matrix, 0),
                    NumDimensions(input_tensor));

  // Calculate total number of nodes in the tree structure of a tensor
  // and pre-allocates it.
  int num_elements = NumElements(input_tensor) + 1;
  int extra_nodes = 1;
  for (int i = 0; i < NumDimensions(input_tensor) - 1; ++i) {
    extra_nodes *= input_tensor->dims->data[i];
    num_elements += extra_nodes;
  }
  op_data->pad_tensor_buffer.resize(num_elements);
  op_data->num_elements = num_elements;

  if (!IsConstantTensor(padding_matrix)) {
    SetTensorToDynamic(output_tensor);
    return kTfLiteOk;
  }
  // We have constant padding, so we can infer output size.

  auto output_size = GetPaddedOutputShape(input_tensor, padding_matrix);
  if (output_size == nullptr) {
    return kTfLiteError;
  }
  return context->ResizeTensor(context, output_tensor, output_size.release());
}

}  // namespace mirror_pad
TfLiteRegistration* Register_MIRROR_PAD() {
  static TfLiteRegistration r = {mirror_pad::Init, mirror_pad::Free,
                                 mirror_pad::Prepare, mirror_pad::Eval};
  return &r;
}

}  // namespace builtin
}  // namespace ops
}  // namespace tflite
