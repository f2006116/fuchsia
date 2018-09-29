// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "peridot/lib/firebase_auth/firebase_auth_impl.h"

#include <utility>

#include <lib/backoff/exponential_backoff.h>
#include <lib/callback/cancellable_helper.h>
#include <lib/fit/function.h>
#include <lib/fsl/vmo/file.h>
#include <lib/fxl/functional/closure.h>
#include <lib/fxl/functional/make_copyable.h>

namespace firebase_auth {
namespace {

constexpr char kConfigBinProtoPath[] =
    "/pkg/data/firebase_auth_cobalt_config.pb";
constexpr int32_t kCobaltAuthFailureMetricId = 3;
constexpr int32_t kCobaltAuthV2FailureMetricId = 4;

// Returns true if the authentication failure may be transient.
bool IsRetriableError(fuchsia::modular::auth::Status status) {
  switch (status) {
    case fuchsia::modular::auth::Status::OK:  // This should never happen.
    case fuchsia::modular::auth::Status::BAD_REQUEST:
    case fuchsia::modular::auth::Status::OAUTH_SERVER_ERROR:
    case fuchsia::modular::auth::Status::USER_CANCELLED:
      return false;
    case fuchsia::modular::auth::Status::BAD_RESPONSE:
    case fuchsia::modular::auth::Status::NETWORK_ERROR:
    case fuchsia::modular::auth::Status::INTERNAL_ERROR:
      return true;
  }
  // In case of unexpected status, retry just in case.
  return true;
}

// Returns true if the authentication failure may be transient.
bool IsRetriableErrorV2(fuchsia::auth::Status status) {
  switch (status) {
    case fuchsia::auth::Status::OK:  // This should never happen.
    case fuchsia::auth::Status::AUTH_PROVIDER_SERVER_ERROR:
    case fuchsia::auth::Status::AUTH_PROVIDER_SERVICE_UNAVAILABLE:
    case fuchsia::auth::Status::INVALID_AUTH_CONTEXT:
    case fuchsia::auth::Status::INVALID_REQUEST:
    case fuchsia::auth::Status::USER_NOT_FOUND:
    case fuchsia::auth::Status::USER_CANCELLED:
    case fuchsia::auth::Status::REAUTH_REQUIRED:
      return false;
    case fuchsia::auth::Status::UNKNOWN_ERROR:
    case fuchsia::auth::Status::NETWORK_ERROR:
    case fuchsia::auth::Status::INTERNAL_ERROR:
    case fuchsia::auth::Status::IO_ERROR:
      return true;
  }
  // In case of unexpected status, retry just in case.
  return true;
}
}  // namespace

FirebaseAuthImpl::FirebaseAuthImpl(
    Config config, async_dispatcher_t* dispatcher, rng::Random* random,
    fuchsia::modular::auth::TokenProviderPtr token_provider,
    fuchsia::auth::TokenManagerPtr token_manager,
    component::StartupContext* startup_context)
    : config_(std::move(config)),
      token_provider_(std::move(token_provider)),
      token_manager_(std::move(token_manager)),
      backoff_(std::make_unique<backoff::ExponentialBackoff>(
          random->NewBitGenerator<uint64_t>())),
      max_retries_(config_.max_retries),
      cobalt_client_name_(config_.cobalt_client_name),
      task_runner_(dispatcher) {
  if (startup_context) {
    cobalt_logger_ = cobalt::NewCobaltLogger(dispatcher, startup_context,
                                             kConfigBinProtoPath);
  } else {
    cobalt_logger_ = nullptr;
  }
}

FirebaseAuthImpl::FirebaseAuthImpl(
    Config config, async_dispatcher_t* dispatcher,
    fuchsia::modular::auth::TokenProviderPtr token_provider,
    fuchsia::auth::TokenManagerPtr token_manager,
    std::unique_ptr<backoff::Backoff> backoff,
    std::unique_ptr<cobalt::CobaltLogger> cobalt_logger)
    : config_(std::move(config)),
      token_provider_(std::move(token_provider)),
      token_manager_(std::move(token_manager)),
      backoff_(std::move(backoff)),
      max_retries_(config_.max_retries),
      cobalt_client_name_(config_.cobalt_client_name),
      cobalt_logger_(std::move(cobalt_logger)),
      task_runner_(dispatcher) {}

void FirebaseAuthImpl::set_error_handler(fit::closure on_error) {
  token_provider_.set_error_handler(
      [on_error = std::move(on_error)](zx_status_t status) { on_error(); });
}

fxl::RefPtr<callback::Cancellable> FirebaseAuthImpl::GetFirebaseToken(
    fit::function<void(AuthStatus, std::string)> callback) {
  if (config_.api_key.empty()) {
    FXL_LOG(WARNING) << "No Firebase API key provided. Connection to Firebase "
                        "may be unauthenticated.";
  }
  auto cancellable = callback::CancellableImpl::Create([] {});
  if (token_manager_.is_bound()) {
    GetTokenV2(max_retries_,
               [callback = cancellable->WrapCallback(std::move(callback))](
                   auto status, auto token) {
                 callback(status, token ? token->id_token : "");
               });
  } else {
    GetToken(max_retries_, [callback = cancellable->WrapCallback(
                                std::move(callback))](auto status, auto token) {
      callback(status, token ? token->id_token : "");
    });
  }
  return cancellable;
}

fxl::RefPtr<callback::Cancellable> FirebaseAuthImpl::GetFirebaseUserId(
    fit::function<void(AuthStatus, std::string)> callback) {
  auto cancellable = callback::CancellableImpl::Create([] {});
  if (token_manager_.is_bound()) {
    GetTokenV2(max_retries_,
               [callback = cancellable->WrapCallback(std::move(callback))](
                   auto status, auto token) {
                 callback(status, token ? token->local_id : "");
               });
  } else {
    GetToken(max_retries_, [callback = cancellable->WrapCallback(
                                std::move(callback))](auto status, auto token) {
      callback(status, token ? token->local_id : "");
    });
  }
  return cancellable;
}

void FirebaseAuthImpl::GetToken(
    int max_retries,
    fit::function<void(AuthStatus, fuchsia::modular::auth::FirebaseTokenPtr)>
        callback) {
  token_provider_->GetFirebaseAuthToken(
      config_.api_key,
      fxl::MakeCopyable([this, max_retries, callback = std::move(callback)](
                            fuchsia::modular::auth::FirebaseTokenPtr token,
                            fuchsia::modular::auth::AuthErr error) mutable {
        if (!token || error.status != fuchsia::modular::auth::Status::OK) {
          if (!token && error.status == fuchsia::modular::auth::Status::OK) {
            FXL_LOG(ERROR)
                << "null Firebase token returned from token provider with no "
                << "error reported. This should never happen. Retrying.";
            error.status = fuchsia::modular::auth::Status::BAD_RESPONSE;
          } else {
            FXL_LOG(ERROR)
                << "Error retrieving the Firebase token from token provider: "
                << fidl::ToUnderlying(error.status) << ", '" << error.message
                << "', retrying.";
          }

          if (max_retries > 0 && IsRetriableError(error.status)) {
            task_runner_.PostDelayedTask(
                [this, max_retries, callback = std::move(callback)]() mutable {
                  GetToken(max_retries - 1, std::move(callback));
                },
                backoff_->GetNext());
            return;
          }
        }

        backoff_->Reset();
        if (error.status == fuchsia::modular::auth::Status::OK) {
          callback(AuthStatus::OK, std::move(token));
        } else {
          ReportError(kCobaltAuthFailureMetricId,
                      static_cast<uint32_t>(error.status));
          callback(AuthStatus::ERROR, std::move(token));
        }
      }));
}

void FirebaseAuthImpl::GetTokenV2(
    int max_retries,
    fit::function<void(AuthStatus, fuchsia::auth::FirebaseTokenPtr)> callback) {
  fuchsia::auth::AppConfig oauth_config;
  oauth_config.auth_provider_type = "google";

  token_manager_->GetFirebaseToken(
      std::move(oauth_config), "", /*user_profile_id*/
      "",                          /*audience*/
      config_.api_key,
      fxl::MakeCopyable([this, max_retries, callback = std::move(callback)](
                            fuchsia::auth::Status status,
                            fuchsia::auth::FirebaseTokenPtr token) mutable {
        if (!token || status != fuchsia::auth::Status::OK) {
          if (!token && status == fuchsia::auth::Status::OK) {
            FXL_LOG(ERROR)
                << "null Firebase token returned from token provider with no "
                << "error reported. This should never happen. Retrying.";
            status = fuchsia::auth::Status::UNKNOWN_ERROR;
          } else {
            FXL_LOG(ERROR)
                << "Error retrieving the Firebase token from token provider: "
                << fidl::ToUnderlying(status) << "', retrying.";
          }

          if (max_retries > 0 && IsRetriableErrorV2(status)) {
            task_runner_.PostDelayedTask(
                [this, max_retries, callback = std::move(callback)]() mutable {
                  GetTokenV2(max_retries - 1, std::move(callback));
                },
                backoff_->GetNext());
            return;
          }
        }

        backoff_->Reset();
        if (status == fuchsia::auth::Status::OK) {
          callback(AuthStatus::OK, std::move(token));
        } else {
          ReportError(kCobaltAuthV2FailureMetricId,
                      static_cast<uint32_t>(status));
          callback(AuthStatus::ERROR, std::move(token));
        }
      }));
}

void FirebaseAuthImpl::ReportError(int32_t metric_id, uint32_t status) {
  if (cobalt_client_name_.empty() || cobalt_logger_ == nullptr) {
    return;
  }

  cobalt_logger_->LogEventCount(metric_id, status, cobalt_client_name_,
                                zx::duration(0), 1);
}
}  // namespace firebase_auth
