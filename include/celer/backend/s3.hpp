#pragma once
/// celer S3 backend — satisfies StorageBackend concept via HTTP/REST.
///
/// Architecture:
///   - One S3 "database" per scope (key prefix: <scope>/<table>/<key>)
///   - Native streaming: stream_get/stream_put use chunked transfer
///   - stream_scan uses paginated ListObjectsV2 → Stream<KVPair>
///   - Non-streaming methods (get/put/del) materialize for backward compat
///
/// Requires: AWS SDK for C++ (aws-sdk-cpp) or a minimal HTTP client.
/// Detection: compile-time via __has_include(<aws/s3/S3Client.h>)

#include <string>
#include <string_view>

#include "celer/backend/concept.hpp"
#include "celer/core/aws_credentials.hpp"
#include "celer/core/result.hpp"
#include "celer/core/stream.hpp"

#if !defined(CELER_FORCE_NO_S3) && __has_include(<aws/s3/S3Client.h>)
#  define CELER_HAS_S3 1
#  include <aws/s3/S3Client.h>
#else
#  define CELER_HAS_S3 0
#endif

namespace celer {

namespace backends::s3 {

/// Version metadata for a single S3 object version.
struct VersionInfo {
    std::string version_id;
    std::string last_modified;   ///< ISO 8601
    std::size_t size{0};
    bool is_latest{false};
    bool is_delete_marker{false};
};

/// Result of a versioned put — carries the assigned version ID.
struct PutResult {
    std::string version_id;  ///< Empty if versioning disabled on the bucket
};

/// S3-specific configuration.
struct Config {
    std::string bucket;                       ///< S3 bucket name
    std::string prefix;                       ///< Key prefix (e.g., "celer/prod/")
    std::string region       = "us-east-1";   ///< AWS region
    std::string endpoint_url;                 ///< Override for MinIO/LocalStack (empty = AWS)
    std::size_t chunk_size   = 5 * 1024 * 1024; ///< Multipart chunk size (5MB default, S3 minimum)
    int         max_retries  = 3;
    int         timeout_ms   = 30000;
    bool        use_path_style = false;       ///< Path-style addressing (for MinIO)
    bool        versioning_enabled = false;   ///< Opt-in: enable version-aware methods
    aws::AwsCredentials credentials;          ///< AccessKey | RoleArn | DefaultChain
};

/// Returns a BackendFactory that routes (scope, table) to S3 key prefixes:
///   <config.prefix><scope>/<table>/<key>
[[nodiscard]] auto factory(Config cfg) -> BackendFactory;

} // namespace backends::s3

#if CELER_HAS_S3

/// S3Backend — satisfies StorageBackend concept.
/// Native streaming for get/put/scan; S3 is inherently chunked.
class S3Backend {
public:
    S3Backend() = default;

    S3Backend(std::shared_ptr<Aws::S3::S3Client> client,
              std::string bucket,
              std::string key_prefix,
              std::size_t chunk_size,
              bool versioning_enabled = false) noexcept
        : client_(std::move(client))
        , bucket_(std::move(bucket))
        , key_prefix_(std::move(key_prefix))
        , chunk_size_(chunk_size)
        , versioning_enabled_(versioning_enabled) {}

    S3Backend(S3Backend&&) noexcept            = default;
    auto operator=(S3Backend&&) noexcept -> S3Backend& = default;
    S3Backend(const S3Backend&)                = delete;
    auto operator=(const S3Backend&) -> S3Backend& = delete;
    ~S3Backend() { close(); }

    [[nodiscard]] static auto name() noexcept -> std::string_view { return "s3"; }

    // ── Core StorageBackend methods (materializing) ──
    [[nodiscard]] auto get(std::string_view key) -> Result<std::optional<std::string>>;
    [[nodiscard]] auto put(std::string_view key, std::string_view value) -> VoidResult;
    [[nodiscard]] auto del(std::string_view key) -> VoidResult;
    [[nodiscard]] auto prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>>;
    [[nodiscard]] auto batch(std::span<const BatchOp> ops) -> VoidResult;
    [[nodiscard]] auto compact() -> VoidResult;   // no-op: S3 is append-only
    [[nodiscard]] auto foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx) -> VoidResult;

    // ── Streaming extensions (RFC-002) — native S3 implementation ──
    [[nodiscard]] auto stream_get(std::string_view key) -> Result<StreamHandle<char>>;
    [[nodiscard]] auto stream_put(std::string_view key, StreamHandle<char> stream) -> VoidResult;
    [[nodiscard]] auto stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>>;

    // ── Versioned object extensions (RFC-002 §16) — S3-specific, not part of StorageBackend concept ──
    [[nodiscard]] auto get_versioned(std::string_view key, std::string_view version_id)
        -> Result<std::optional<std::string>>;
    [[nodiscard]] auto stream_get_versioned(std::string_view key, std::string_view version_id)
        -> Result<StreamHandle<char>>;
    [[nodiscard]] auto put_versioned(std::string_view key, std::string_view value)
        -> Result<backends::s3::PutResult>;
    [[nodiscard]] auto list_versions(std::string_view key)
        -> Result<StreamHandle<backends::s3::VersionInfo>>;

    auto close() -> VoidResult;

private:
    std::shared_ptr<Aws::S3::S3Client> client_;
    std::string bucket_;
    std::string key_prefix_;
    std::size_t chunk_size_{5 * 1024 * 1024};
    bool versioning_enabled_{false};

    /// Resolve a logical key to the full S3 object key.
    [[nodiscard]] auto resolve_key(std::string_view key) const -> std::string {
        return key_prefix_ + std::string(key);
    }
};

#endif // CELER_HAS_S3

} // namespace celer
