//
// Copyright 2019 ZetaSQL Authors
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
//

#ifndef ZETASQL_PUBLIC_TYPES_TYPE_FACTORY_H_
#define ZETASQL_PUBLIC_TYPES_TYPE_FACTORY_H_

#include "zetasql/public/types/array_type.h"
#include "zetasql/public/types/enum_type.h"
#include "zetasql/public/types/proto_type.h"
#include "zetasql/public/types/simple_type.h"
#include "zetasql/public/types/struct_type.h"

ABSL_DECLARE_FLAG(int32_t, zetasql_type_factory_nesting_depth_limit);

namespace zetasql {

// A TypeFactory creates and owns Type objects.
// Created Type objects live until the TypeFactory is destroyed.
// The TypeFactory may return the same Type object from multiple calls that
// request equivalent types.
//
// When a compound Type (array or struct) is constructed referring to a Type
// from a separate TypeFactory, the constructed type may refer to the Type from
// the separate TypeFactory, so that TypeFactory must outlive this one.
//
// This class is thread-safe.
class TypeFactory {
 public:
  TypeFactory();
#ifndef SWIG
  TypeFactory(const TypeFactory&) = delete;
  TypeFactory& operator=(const TypeFactory&) = delete;
#endif  // SWIG
  ~TypeFactory();

  // Helpers to get simple scalar types directly.
  const Type* get_int32();
  const Type* get_int64();
  const Type* get_uint32();
  const Type* get_uint64();
  const Type* get_string();
  const Type* get_bytes();
  const Type* get_bool();
  const Type* get_float();
  const Type* get_double();
  const Type* get_date();
  const Type* get_timestamp();
  const Type* get_time();
  const Type* get_datetime();
  const Type* get_geography();
  const Type* get_numeric();
  const Type* get_bignumeric();

  // Return a Type object for a simple type.  This works for all
  // non-parameterized scalar types.  Enums, arrays, structs and protos must
  // use the parameterized constructors.
  const Type* MakeSimpleType(TypeKind kind);

  // Make an array type.
  // Arrays of arrays are not supported and will fail with an error.
  zetasql_base::Status MakeArrayType(const Type* element_type,
                             const ArrayType** result);
  zetasql_base::Status MakeArrayType(const Type* element_type,
                             const Type** result);

  // Make a struct type.
  // The field names must be valid.
  zetasql_base::Status MakeStructType(absl::Span<const StructType::StructField> fields,
                              const StructType** result);
  zetasql_base::Status MakeStructType(absl::Span<const StructType::StructField> fields,
                              const Type** result);
  zetasql_base::Status MakeStructTypeFromVector(
      std::vector<StructType::StructField> fields, const StructType** result);
  zetasql_base::Status MakeStructTypeFromVector(
      std::vector<StructType::StructField> fields, const Type** result);

  // Make a proto type.
  // The <descriptor> must outlive this TypeFactory.
  //
  // This always constructs a ProtoType, even for protos that are
  // annotated with zetasql.is_struct or zetasql.is_wrapper,
  // which normally indicate the proto should be interpreted as
  // a different type.  Use MakeUnwrappedTypeFromProto instead
  // to get the unwrapped type.
  zetasql_base::Status MakeProtoType(const google::protobuf::Descriptor* descriptor,
                             const ProtoType** result);
  zetasql_base::Status MakeProtoType(const google::protobuf::Descriptor* descriptor,
                             const Type** result);

  // Make a zetasql type from a proto, honoring zetasql.is_struct and
  // zetasql.is_wrapper annotations.
  // These annotations allow creating a proto representation of any zetasql
  // type, including structs and arrays, with nullability.
  // Such protos can be created with methods in convert_type_to_proto.h.
  // This method converts protos back to the represented zetasql type.
  zetasql_base::Status MakeUnwrappedTypeFromProto(const google::protobuf::Descriptor* message,
                                          const Type** result_type) {
    return MakeUnwrappedTypeFromProto(message, /*use_obsolete_timestamp=*/false,
                                      result_type);
  }
  // DEPRECATED: Callers should remove their dependencies on obsolete types and
  // move to the method above.
  zetasql_base::Status MakeUnwrappedTypeFromProto(const google::protobuf::Descriptor* message,
                                          bool use_obsolete_timestamp,
                                          const Type** result_type);

  // Like the method above, but starting from a zetasql::Type.
  // If the Type is not a proto, it will be returned unchanged.
  zetasql_base::Status UnwrapTypeIfAnnotatedProto(const Type* input_type,
                                          const Type** result_type) {
    return UnwrapTypeIfAnnotatedProto(
        input_type, /*use_obsolete_timestamp=*/false, result_type);
  }
  // DEPRECATED: Callers should remove their dependencies on obsolete types and
  // move to the method above.
  zetasql_base::Status UnwrapTypeIfAnnotatedProto(const Type* input_type,
                                          bool use_obsolete_timestamp,
                                          const Type** result_type);

  // Make an enum type from a protocol buffer EnumDescriptor.
  // The <enum_descriptor> must outlive this TypeFactory.
  zetasql_base::Status MakeEnumType(const google::protobuf::EnumDescriptor* enum_descriptor,
                            const EnumType** result);
  zetasql_base::Status MakeEnumType(const google::protobuf::EnumDescriptor* enum_descriptor,
                            const Type** result);

  // Get the Type for a proto field.
  // If <ignore_annotations> is false, this looks at format annotations on the
  // field and possibly its parent message to help select the Type. If
  // <ignore_annotations> is true, annotations on the field are not considered
  // and the returned type is that of which ZetaSQL sees before applying any
  // annotations or automatic conversions. This function always ignores (does
  // not unwrap) is_struct and is_wrapper annotations.
  zetasql_base::Status GetProtoFieldType(bool ignore_annotations,
                                 const google::protobuf::FieldDescriptor* field_descr,
                                 const Type** type);

  // Get the Type for a proto field.
  // This is the same as the above signature with ignore_annotations = false.
  //
  // NOTE: There is a similar method GetProtoFieldTypeAndDefault in proto_util.h
  // that also extracts the default value.
  zetasql_base::Status GetProtoFieldType(const google::protobuf::FieldDescriptor* field_descr,
                                 const Type** type) {
    return GetProtoFieldType(/*ignore_annotations=*/false, field_descr, type);
  }
  // DEPRECATED: Callers should remove their dependencies on obsolete types and
  // move to the method above.
  zetasql_base::Status GetProtoFieldType(const google::protobuf::FieldDescriptor* field_descr,
                                 bool use_obsolete_timestamp,
                                 const Type** type);

  // Makes a ZetaSQL Type from a self-contained ZetaSQL TypeProto.  The
  // <type_proto> FileDescriptorSets are loaded into the pool.  The <pool>
  // must outlive the TypeFactory.  Will return an error if the
  // FileDescriptorSets cannot be deserialized into a single DescriptorPool,
  // i.e. if type_proto.file_descriptor_set_size() > 1.  For serialized types
  // spanning multiple pools, see
  // DeserializeFromSelfContainedProtoWithDistinctFiles below.
  zetasql_base::Status DeserializeFromSelfContainedProto(
      const TypeProto& type_proto,
      google::protobuf::DescriptorPool* pool,
      const Type** type);

  // Similar to the above, but supports types referencing multiple
  // DescriptorPools.  The provided pools must match the number of
  // FileDescriptorSets stored in <type_proto>.  Each FileDescriptorSet from
  // <type_proto> is loaded into the DescriptorPool corresponding to its index.
  zetasql_base::Status DeserializeFromSelfContainedProtoWithDistinctFiles(
      const TypeProto& type_proto,
      const std::vector<google::protobuf::DescriptorPool*>& pools,
      const Type** type);

  // Make a ZetaSQL Type from a ZetaSQL TypeProto.  All protos referenced
  // by <type_proto> must already have related descriptors in the <pool>.
  // The <pool> must outlive the TypeFactory.  May only be used with a
  // <type_proto> serialized via Type::SerializeToProtoAndFileDescriptors.
  zetasql_base::Status DeserializeFromProtoUsingExistingPool(
      const TypeProto& type_proto,
      const google::protobuf::DescriptorPool* pool,
      const Type** type);

  // Similar to the above, but expects that all protos and enums referenced by
  // <type_proto> must have related descriptors in the pool corresponding to
  // the ProtoTypeProto or EnumTypeProto's file_descriptor_set_index. May be
  // used with a <type_proto> serialized via
  // Type::SerializeToProtoAndFileDescriptors or
  // Type::SerializeToProtoAndDistinctFileDescriptors.
  zetasql_base::Status DeserializeFromProtoUsingExistingPools(
      const TypeProto& type_proto,
      const std::vector<const google::protobuf::DescriptorPool*>& pools,
      const Type** type);

  // Maximum nesting depth for types supported by this TypeFactory. Any attempt
  // to create a type with a nesting_depth() greater than this will return an
  // error. If a limit is not set, the ZetaSQL analyzer may create types that
  // it cannot destruct. Use kint32max for no limit (the default).
  // The limit value must be >= 0. The default value of this field can be
  // overidden with FLAGS_zetasql_type_factory_nesting_depth_limit.
  int nesting_depth_limit() const ABSL_LOCKS_EXCLUDED(mutex_);
  void set_nesting_depth_limit(int value) ABSL_LOCKS_EXCLUDED(mutex_);

  // Estimate memory size allocated to store TypeFactory's data in bytes
  int64_t GetEstimatedOwnedMemoryBytesSize() const;

 private:
  // Store links to and from TypeFactories that this TypeFactory depends on.
  // This is used as a sanity check to catch incorrect destruction order.
  mutable absl::flat_hash_set<const TypeFactory*> depends_on_factories_
      ABSL_GUARDED_BY(mutex_);
  mutable absl::flat_hash_set<const TypeFactory*> factories_depending_on_this_
      ABSL_GUARDED_BY(mutex_);

  // Add <type> into <owned_types_>.  Templated so it can return the
  // specific subclass of Type.
  template <class TYPE>
  const TYPE* TakeOwnership(const TYPE* type) ABSL_LOCKS_EXCLUDED(mutex_);
  template <class TYPE>
  const TYPE* TakeOwnershipLocked(const TYPE* type)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);
  template <class TYPE>
  const TYPE* TakeOwnershipLocked(const TYPE* type, int64_t type_owned_bytes_size)
      ABSL_EXCLUSIVE_LOCKS_REQUIRED(mutex_);

  // Mark that <other_type>'s factory must outlive <this>.
  void AddDependency(const Type* other_type) ABSL_LOCKS_EXCLUDED(mutex_);

  // Get the Type for a proto field from its corresponding TypeKind. For
  // repeated fields, <kind> must be the base TypeKind for the field (i.e., the
  // TypeKind of the field, ignoring repeatedness), which can be obtained by
  // FieldDescriptorToTypeKindBase().
  zetasql_base::Status GetProtoFieldTypeWithKind(
      const google::protobuf::FieldDescriptor* field_descr, TypeKind kind,
      const Type** type);

  // Implementation of MakeUnwrappedTypeFromProto above that detects invalid use
  // of type annotations with recursive protos by storing all visited message
  // types in 'ancestor_messages'.
  zetasql_base::Status MakeUnwrappedTypeFromProtoImpl(
      const google::protobuf::Descriptor* message, const Type* existing_message_type,
      bool use_obsolete_timestamp, const Type** result_type,
      std::set<const google::protobuf::Descriptor*>* ancestor_messages);

  // Implementation of UnwrapTypeIfAnnotatedProto above that detects invalid use
  // of type annotations with recursive protos by storing all visited message
  // types in 'ancestor_messages'.
  zetasql_base::Status UnwrapTypeIfAnnotatedProtoImpl(
      const Type* input_type, bool use_obsolete_timestamp,
      const Type** result_type,
      std::set<const google::protobuf::Descriptor*>* ancestor_messages);

  // TODO: Should TypeFactory have a DescriptorPool?
  mutable absl::Mutex mutex_;
  absl::flat_hash_map<const Type*, const ArrayType*> cached_array_types_
      ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<const google::protobuf::Descriptor*, const ProtoType*>
      cached_proto_types_ ABSL_GUARDED_BY(mutex_);
  absl::flat_hash_map<const google::protobuf::EnumDescriptor*, const EnumType*>
      cached_enum_types_ ABSL_GUARDED_BY(mutex_);
  // TODO: Once all Type objects are cached, we can likely eliminate
  // this and treat the maps above as owning the Type objects.
  std::vector<const Type*> owned_types_ ABSL_GUARDED_BY(mutex_);

  int nesting_depth_limit_ ABSL_GUARDED_BY(mutex_);

  // Stores estimation of how much memory was allocated by instances
  // of types owned by this TypeFactory (in bytes)
  int64_t estimated_memory_used_by_types_;
};

namespace types {
// The following functions do *not* create any new types using the static
// factory.
const Type* Int32Type();
const Type* Int64Type();
const Type* Uint32Type();
const Type* Uint64Type();
const Type* BoolType();
const Type* FloatType();
const Type* DoubleType();
const Type* StringType();
const Type* BytesType();
const Type* DateType();
const Type* TimestampType();
const Type* TimeType();
const Type* DatetimeType();
const Type* GeographyType();
const Type* NumericType();
const Type* BigNumericType();
const StructType* EmptyStructType();

// ArrayTypes
const ArrayType* Int32ArrayType();
const ArrayType* Int64ArrayType();
const ArrayType* Uint32ArrayType();
const ArrayType* Uint64ArrayType();
const ArrayType* BoolArrayType();
const ArrayType* FloatArrayType();
const ArrayType* DoubleArrayType();
const ArrayType* StringArrayType();
const ArrayType* BytesArrayType();
const ArrayType* TimestampArrayType();
const ArrayType* DateArrayType();
const ArrayType* DatetimeArrayType();
const ArrayType* TimeArrayType();
const ArrayType* GeographyArrayType();
const ArrayType* NumericArrayType();
const ArrayType* BigNumericArrayType();

// Accessor for the ZetaSQL enum Type (functions::DateTimestampPart)
// that represents date parts in function signatures.  Intended
// to be used primarily within the ZetaSQL library, rather than as a
// part of the public ZetaSQL api.
const EnumType* DatePartEnumType();

// Accessor for the ZetaSQL enum Type (functions::NormalizeMode)
// that represents the normalization mode in NORMALIZE and
// NORMALIZE_AND_CASEFOLD.  Intended to be used primarily within the ZetaSQL
// library, rather than as a part of the public ZetaSQL API.
const EnumType* NormalizeModeEnumType();

// Return a type of 'type_kind' if 'type_kind' is a simple type, otherwise
// returns nullptr. This is similar to TypeFactory::MakeSimpleType, but doesn't
// require TypeFactory.
const Type* TypeFromSimpleTypeKind(TypeKind type_kind);

// Returns an array type with element type of 'type_kind' if 'type_kind' is a
// simple type, otherwise returns nullptr.
const ArrayType* ArrayTypeFromSimpleTypeKind(TypeKind type_kind);
}  // namespace types

}  // namespace zetasql

#endif  // ZETASQL_PUBLIC_TYPES_TYPE_FACTORY_H_