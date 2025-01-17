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

#include "xls/codegen/flattening.h"

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/bits_ops.h"
#include "xls/ir/package.h"

namespace xls {

// Gathers the Bits objects at the leaves of the Value.
static void GatherValueLeaves(const Value& value, std::vector<Bits>* leaves) {
  switch (value.kind()) {
    case ValueKind::kBits:
      leaves->push_back(value.bits());
      break;
    case ValueKind::kTuple:
    case ValueKind::kArray:
      for (const Value& e : value.elements()) {
        GatherValueLeaves(e, leaves);
      }
      break;
    default:
      XLS_LOG(FATAL) << "Invalid value kind: " << value.kind();
  }
}

Bits FlattenValueToBits(const Value& value) {
  std::vector<Bits> leaves;
  GatherValueLeaves(value, &leaves);
  return bits_ops::Concat(leaves);
}

absl::StatusOr<Value> UnflattenBitsToValue(const Bits& bits, const Type* type) {
  if (bits.bit_count() != type->GetFlatBitCount()) {
    return absl::InvalidArgumentError(
        absl::StrFormat("Cannot unflatten input. Has %d bits, expected %d bits",
                        bits.bit_count(), type->GetFlatBitCount()));
  }
  if (type->IsBits()) {
    return Value(bits);
  }
  if (type->IsTuple()) {
    std::vector<Value> elements;
    const TupleType* tuple_type = type->AsTupleOrDie();
    for (int64_t i = 0; i < tuple_type->size(); ++i) {
      Type* element_type = tuple_type->element_type(i);
      XLS_ASSIGN_OR_RETURN(
          Value element, UnflattenBitsToValue(
                             bits.Slice(GetFlatBitIndexOfElement(tuple_type, i),
                                        element_type->GetFlatBitCount()),
                             element_type));
      elements.push_back(element);
    }
    return Value::Tuple(elements);
  }
  if (type->IsArray()) {
    std::vector<Value> elements;
    const ArrayType* array_type = type->AsArrayOrDie();
    for (int64_t i = 0; i < array_type->size(); ++i) {
      XLS_ASSIGN_OR_RETURN(
          Value element,
          UnflattenBitsToValue(
              bits.Slice(GetFlatBitIndexOfElement(array_type, i),
                         array_type->element_type()->GetFlatBitCount()),
              array_type->element_type()));
      elements.push_back(element);
    }
    return Value::Array(elements);
  }
  return absl::InvalidArgumentError(
      absl::StrFormat("Invalid type: %s", type->ToString()));
}

absl::StatusOr<Value> UnflattenBitsToValue(const Bits& bits,
                                           const TypeProto& type_proto) {
  // Create a dummy package for converting  a TypeProto into a Type*.
  Package p("unflatten_dummy");
  XLS_ASSIGN_OR_RETURN(Type * type, p.GetTypeFromProto(type_proto));
  return UnflattenBitsToValue(bits, type);
}

int64_t GetFlatBitIndexOfElement(const TupleType* tuple_type, int64_t index) {
  XLS_CHECK_GE(index, 0);
  XLS_CHECK_LT(index, tuple_type->size());
  int64_t flat_index = 0;
  for (int64_t i = tuple_type->size() - 1; i > index; --i) {
    flat_index += tuple_type->element_type(i)->GetFlatBitCount();
  }
  return flat_index;
}

int64_t GetFlatBitIndexOfElement(const ArrayType* array_type, int64_t index) {
  XLS_CHECK_GE(index, 0);
  XLS_CHECK_LT(index, array_type->size());
  return (array_type->size() - index - 1) *
         array_type->element_type()->GetFlatBitCount();
}

// Recursive helper for Unflatten functions.
verilog::Expression* UnflattenArrayHelper(int64_t flat_index_offset,
                                          verilog::IndexableExpression* input,
                                          ArrayType* array_type,
                                          verilog::VerilogFile* file) {
  std::vector<verilog::Expression*> elements;
  const int64_t element_width = array_type->element_type()->GetFlatBitCount();
  for (int64_t i = 0; i < array_type->size(); ++i) {
    const int64_t element_start =
        flat_index_offset + GetFlatBitIndexOfElement(array_type, i);
    if (array_type->element_type()->IsArray()) {
      elements.push_back(UnflattenArrayHelper(
          element_start, input, array_type->element_type()->AsArrayOrDie(),
          file));
    } else {
      elements.push_back(
          file->Slice(input, element_start + element_width - 1, element_start));
    }
  }
  return file->ArrayAssignmentPattern(elements);
}

verilog::Expression* UnflattenArray(verilog::IndexableExpression* input,
                                    ArrayType* array_type,
                                    verilog::VerilogFile* file) {
  return UnflattenArrayHelper(/*flat_index_offset=*/0, input, array_type, file);
}

verilog::Expression* UnflattenArrayShapedTupleElement(
    verilog::IndexableExpression* input, TupleType* tuple_type,
    int64_t tuple_index, verilog::VerilogFile* file) {
  XLS_CHECK(tuple_type->element_type(tuple_index)->IsArray());
  ArrayType* array_type = tuple_type->element_type(tuple_index)->AsArrayOrDie();
  return UnflattenArrayHelper(
      /*flat_index_offset=*/GetFlatBitIndexOfElement(tuple_type, tuple_index),
      input, array_type, file);
}

verilog::Expression* FlattenArray(verilog::IndexableExpression* input,
                                  ArrayType* array_type,
                                  verilog::VerilogFile* file) {
  std::vector<verilog::Expression*> elements;
  for (int64_t i = 0; i < array_type->size(); ++i) {
    verilog::IndexableExpression* element =
        file->Index(input, i);  // array_type->size() - i - 1);
    if (array_type->element_type()->IsArray()) {
      elements.push_back(FlattenArray(
          element, array_type->element_type()->AsArrayOrDie(), file));
    } else {
      elements.push_back(element);
    }
  }
  return file->Concat(elements);
}

}  // namespace xls
