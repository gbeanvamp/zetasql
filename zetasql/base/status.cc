//
// Copyright 2018 ZetaSQL Authors
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

#include "zetasql/base/status.h"

#include "absl/strings/str_cat.h"
#include "absl/types/optional.h"
#include "zetasql/base/map_util.h"

namespace zetasql_base {

std::string StatusCodeToString(StatusCode e) {
  if (e == StatusCode::kOk) {
    return "OK";
  } else if (e == StatusCode::kInvalidArgument) {
    return "kInvalidArgument";
  } else if (e == StatusCode::kCancelled) {
    return "kCancelled";
  } else if (e == StatusCode::kUnknown) {
    return "kUnknown";
  } else {
    return absl::StrCat(e);
  }
}

std::ostream& operator<<(std::ostream& os, StatusCode code) {
  return os << StatusCodeToString(code);
}

Status::Status(StatusCode code, absl::string_view message)
    : code_(code), message_(code == StatusCode::kOk ? "" : message) {}

std::string Status::ToString() const {
  return ok() ? "OK" : absl::StrCat(StatusCodeToString(code()), ": ", message_);
}

void Status::SetPayload(absl::string_view type_url, const StatusCord& payload) {
  if (!ok()) {
    InsertOrUpdate(&payload_, std::string(type_url), payload);
  }
}

absl::optional<StatusCord> Status::GetPayload(
    absl::string_view type_url) const {
  auto it = payload_.find(std::string(type_url));
  if (it == payload_.end()) return absl::nullopt;
  return it->second;
}

void Status::ErasePayload(absl::string_view type_url) {
  payload_.erase(std::string(type_url));
}

std::ostream& operator<<(std::ostream& os, const Status& x) {
  return os << x.ToString();
}

Status AbortedError(absl::string_view message) {
  return Status(ABORTED, message);
}

Status AlreadyExistsError(absl::string_view message) {
  return Status(ALREADY_EXISTS, message);
}

Status CancelledError(absl::string_view message) {
  return Status(CANCELLED, message);
}

Status DataLossError(absl::string_view message) {
  return Status(DATA_LOSS, message);
}

Status DeadlineExceededError(absl::string_view message) {
  return Status(DEADLINE_EXCEEDED, message);
}

Status FailedPreconditionError(absl::string_view message) {
  return Status(FAILED_PRECONDITION, message);
}

Status InternalError(absl::string_view message) {
  return Status(INTERNAL, message);
}

Status InvalidArgumentError(absl::string_view message) {
  return Status(INVALID_ARGUMENT, message);
}

Status NotFoundError(absl::string_view message) {
  return Status(NOT_FOUND, message);
}

Status OutOfRangeError(absl::string_view message) {
  return Status(OUT_OF_RANGE, message);
}

Status PermissionDeniedError(absl::string_view message) {
  return Status(PERMISSION_DENIED, message);
}

Status ResourceExhaustedError(absl::string_view message) {
  return Status(RESOURCE_EXHAUSTED, message);
}

Status UnauthenticatedError(absl::string_view message) {
  return Status(UNAUTHENTICATED, message);
}

Status UnavailableError(absl::string_view message) {
  return Status(UNAVAILABLE, message);
}

Status UnimplementedError(absl::string_view message) {
  return Status(UNIMPLEMENTED, message);
}

Status UnknownError(absl::string_view message) {
  return Status(UNKNOWN, message);
}

bool IsAborted(const Status& status) {
  return status.code() == ABORTED;
}

bool IsAlreadyExists(const Status& status) {
  return status.code() == ALREADY_EXISTS;
}

bool IsCancelled(const Status& status) {
  return status.code() == CANCELLED;
}

bool IsDataLoss(const Status& status) {
  return status.code() == DATA_LOSS;
}

bool IsDeadlineExceeded(const Status& status) {
  return status.code() == DEADLINE_EXCEEDED;
}

bool IsFailedPrecondition(const Status& status) {
  return status.code() == FAILED_PRECONDITION;
}

bool IsInternal(const Status& status) {
  return status.code() == INTERNAL;
}

bool IsInvalidArgument(const Status& status) {
  return status.code() == INVALID_ARGUMENT;
}

bool IsNotFound(const Status& status) {
  return status.code() == NOT_FOUND;
}

bool IsOutOfRange(const Status& status) {
  return status.code() == OUT_OF_RANGE;
}

bool IsPermissionDenied(const Status& status) {
  return status.code() == PERMISSION_DENIED;
}

bool IsResourceExhausted(const Status& status) {
  return status.code() == RESOURCE_EXHAUSTED;
}

bool IsUnauthenticated(const Status& status) {
  return status.code() == UNAUTHENTICATED;
}

bool IsUnavailable(const Status& status) {
  return status.code() == UNAVAILABLE;
}

bool IsUnimplemented(const Status& status) {
  return status.code() == UNIMPLEMENTED;
}

bool IsUnknown(const Status& status) {
  return status.code() == UNKNOWN;
}

}  // namespace zetasql_base
