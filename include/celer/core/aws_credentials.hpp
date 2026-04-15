#pragma once
/// celer::aws — Explicit AWS credential types for S3 backend.
///
/// Two credential modes:
///   1. AccessKey  — static IAM access key ID + secret access key
///   2. RoleArn    — assume IAM role via STS (access key used as seed creds)
///
/// Chain variant holds both, resolved at client construction time.
/// If neither is set, falls back to AWS SDK default credential provider chain
/// (env vars → ~/.aws/credentials → instance profile → OIDC).
///
/// Thread safety: credential objects are value types — copy freely.
/// The resolved credential provider is thread-safe (AWS SDK guarantee).

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

namespace celer::aws {

// ════════════════════════════════════════════════════════════════════
// AccessKey — Static IAM credentials (long-lived or temporary)
// ════════════════════════════════════════════════════════════════════

struct AccessKey {
    std::string access_key_id;       ///< AWS_ACCESS_KEY_ID
    std::string secret_access_key;   ///< AWS_SECRET_ACCESS_KEY
    std::string session_token;       ///< AWS_SESSION_TOKEN (empty for long-lived keys)

    [[nodiscard]] auto valid() const noexcept -> bool {
        return !access_key_id.empty() && !secret_access_key.empty();
    }
};

// ════════════════════════════════════════════════════════════════════
// RoleArn — Assume role via STS (requires seed credentials)
// ════════════════════════════════════════════════════════════════════

struct RoleArn {
    std::string role_arn;                ///< arn:aws:iam::ACCOUNT:role/ROLE_NAME
    std::string session_name = "celer";  ///< STS session identifier
    std::string external_id;             ///< External ID for cross-account (empty = not required)
    uint32_t    duration_seconds = 3600; ///< Token lifetime (900–43200, default 1h)

    /// Optional seed credentials for the STS call itself.
    /// If empty, STS call uses the SDK default chain.
    std::optional<AccessKey> seed_credentials;

    [[nodiscard]] auto valid() const noexcept -> bool {
        return !role_arn.empty();
    }
};

// ════════════════════════════════════════════════════════════════════
// AwsCredentials — Sum type: AccessKey | RoleArn | DefaultChain
// ════════════════════════════════════════════════════════════════════

/// Sentinel: use the AWS SDK default credential provider chain.
struct DefaultChain {};

/// Discriminated union of credential strategies.
/// Default-constructed = DefaultChain (zero-config).
using AwsCredentials = std::variant<DefaultChain, AccessKey, RoleArn>;

/// Convenience factories — avoids exposing variant syntax to callers.
[[nodiscard]] inline auto from_access_key(std::string key_id,
                                           std::string secret,
                                           std::string token = {}) -> AwsCredentials {
    return AccessKey{std::move(key_id), std::move(secret), std::move(token)};
}

[[nodiscard]] inline auto from_role_arn(std::string arn,
                                         std::string session = "celer",
                                         std::string external_id = {},
                                         uint32_t duration = 3600) -> AwsCredentials {
    return RoleArn{std::move(arn), std::move(session), std::move(external_id), duration, std::nullopt};
}

[[nodiscard]] inline auto from_role_arn_with_key(std::string arn,
                                                  AccessKey seed,
                                                  std::string session = "celer",
                                                  std::string external_id = {},
                                                  uint32_t duration = 3600) -> AwsCredentials {
    return RoleArn{std::move(arn), std::move(session), std::move(external_id),
                   duration, std::move(seed)};
}

/// Resolve credentials from environment variables.
/// Checks AWS_ACCESS_KEY_ID / AWS_SECRET_ACCESS_KEY / AWS_SESSION_TOKEN
/// and AWS_ROLE_ARN / AWS_ROLE_SESSION_NAME.
/// Returns DefaultChain if nothing is set.
[[nodiscard]] auto from_env() -> AwsCredentials;

} // namespace celer::aws
