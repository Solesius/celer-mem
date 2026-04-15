#include "celer/backend/s3.hpp"

#include <string>
#include <type_traits>
#include <variant>

#if CELER_HAS_S3
#  include <aws/core/auth/AWSCredentialsProvider.h>
#  include <aws/core/auth/AWSCredentialsProviderChain.h>
#  include <aws/core/client/DefaultRetryStrategy.h>
#  include <aws/s3/model/GetObjectRequest.h>
#  include <aws/s3/model/PutObjectRequest.h>
#  include <aws/s3/model/DeleteObjectRequest.h>
#  include <aws/s3/model/DeleteObjectsRequest.h>
#  include <aws/s3/model/ListObjectsV2Request.h>
#  include <aws/s3/model/ListObjectVersionsRequest.h>
#  include <aws/s3/model/CreateMultipartUploadRequest.h>
#  include <aws/s3/model/UploadPartRequest.h>
#  include <aws/s3/model/CompleteMultipartUploadRequest.h>
#  include <aws/s3/model/AbortMultipartUploadRequest.h>
#  include <aws/sts/STSClient.h>
#  include <aws/sts/model/AssumeRoleRequest.h>
#  include <aws/sts/model/AssumeRoleResult.h>
#endif

namespace celer {

#if CELER_HAS_S3

// ── S3 stream implementation types (Prototype-clonable) ──

namespace {

/// Pull-based stream over S3 GetObject response.
/// Each pull() reads chunk_size bytes from the response stream.
/// Prototype clone captures the current byte offset for independent replay.
struct S3GetStream {
    std::shared_ptr<Aws::S3::S3Client> client;
    std::string bucket;
    std::string key;
    std::size_t chunk_size;
    std::size_t offset{0};
    bool done{false};
    std::string version_id;  // empty = latest version

    auto pull() -> Result<std::optional<Chunk<char>>> {
        if (done) return std::optional<Chunk<char>>{};

        // Issue ranged GET: bytes=offset-(offset+chunk_size-1)
        Aws::S3::Model::GetObjectRequest req;
        req.SetBucket(bucket);
        req.SetKey(key);
        if (!version_id.empty()) {
            req.SetVersionId(version_id);
        }
        auto range = "bytes=" + std::to_string(offset) + "-"
                   + std::to_string(offset + chunk_size - 1);
        req.SetRange(range);

        auto outcome = client->GetObject(req);
        if (!outcome.IsSuccess()) {
            auto& err = outcome.GetError();
            if (err.GetResponseCode() == Aws::Http::HttpResponseCode::REQUESTED_RANGE_NOT_SATISFIABLE
                || err.GetExceptionName() == "NoSuchKey"
                || err.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
                done = true;
                return std::optional<Chunk<char>>{};
            }
            return std::unexpected(Error{"S3StreamGet", err.GetMessage()});
        }

        auto& body = outcome.GetResultWithOwnership().GetBody();
        std::string buf(std::istreambuf_iterator<char>(body), {});
        if (buf.empty()) {
            done = true;
            return std::optional<Chunk<char>>{};
        }

        offset += buf.size();
        if (buf.size() < chunk_size) done = true;

        std::vector<char> chars(buf.begin(), buf.end());
        return std::optional{Chunk<char>::from(std::move(chars))};
    }

    S3GetStream(std::shared_ptr<Aws::S3::S3Client> c,
                std::string b, std::string k, std::size_t cs,
                std::size_t off, bool d, std::string vid = {})
        : client(std::move(c)), bucket(std::move(b)), key(std::move(k))
        , chunk_size(cs), offset(off), done(d), version_id(std::move(vid)) {}
    S3GetStream(const S3GetStream&) = default;
};

/// Pull-based stream over S3 ListObjectsV2 pagination.
/// Each pull() fetches one page of results via continuation token.
/// Prototype clone captures the current token for independent pagination.
struct S3ListStream {
    std::shared_ptr<Aws::S3::S3Client> client;
    std::string bucket;
    std::string prefix;
    std::string continuation_token;
    bool done{false};

    S3ListStream(std::shared_ptr<Aws::S3::S3Client> c,
                 std::string b, std::string p, std::string ct, bool d)
        : client(std::move(c)), bucket(std::move(b)), prefix(std::move(p))
        , continuation_token(std::move(ct)), done(d) {}

    auto pull() -> Result<std::optional<Chunk<KVPair>>> {
        if (done) return std::optional<Chunk<KVPair>>{};

        Aws::S3::Model::ListObjectsV2Request req;
        req.SetBucket(bucket);
        req.SetPrefix(prefix);
        if (!continuation_token.empty()) {
            req.SetContinuationToken(continuation_token);
        }

        auto outcome = client->ListObjectsV2(req);
        if (!outcome.IsSuccess()) {
            return std::unexpected(Error{"S3StreamScan", outcome.GetError().GetMessage()});
        }

        auto& result = outcome.GetResult();
        if (result.GetIsTruncated()) {
            continuation_token = result.GetNextContinuationToken();
        } else {
            done = true;
        }

        std::vector<KVPair> pairs;
        for (const auto& obj : result.GetContents()) {
            // Strip the prefix to get the logical key
            auto full_key = std::string(obj.GetKey());
            auto logical_key = full_key.substr(prefix.size());

            // Fetch each object's value (for small-object stores)
            Aws::S3::Model::GetObjectRequest get_req;
            get_req.SetBucket(bucket);
            get_req.SetKey(obj.GetKey());
            auto get_outcome = client->GetObject(get_req);
            if (!get_outcome.IsSuccess()) {
                return std::unexpected(Error{"S3StreamScan",
                    "failed to get object '" + full_key + "': "
                    + get_outcome.GetError().GetMessage()});
            }
            auto& body = get_outcome.GetResultWithOwnership().GetBody();
            std::string value(std::istreambuf_iterator<char>(body), {});
            pairs.push_back(KVPair{std::move(logical_key), std::move(value)});
        }

        if (pairs.empty()) {
            done = true;
            return std::optional<Chunk<KVPair>>{};
        }
        return std::optional{Chunk<KVPair>::from(std::move(pairs))};
    }

    S3ListStream(const S3ListStream&) = default;
};

} // namespace

// ── Core StorageBackend methods (materializing via streaming) ──

auto S3Backend::get(std::string_view key) -> Result<std::optional<std::string>> {
    auto s = stream_get(key);
    if (!s) return std::unexpected(s.error());
    auto collected = stream::collect_string(*s);
    if (!collected) return std::unexpected(collected.error());
    if (collected->empty()) return std::optional<std::string>{std::nullopt};
    return std::optional<std::string>{std::move(*collected)};
}

auto S3Backend::put(std::string_view key, std::string_view value) -> VoidResult {
    auto full_key = resolve_key(key);

    Aws::S3::Model::PutObjectRequest req;
    req.SetBucket(bucket_);
    req.SetKey(full_key);

    auto body = Aws::MakeShared<Aws::StringStream>("celer");
    body->write(value.data(), static_cast<std::streamsize>(value.size()));
    req.SetBody(body);

    auto outcome = client_->PutObject(req);
    if (!outcome.IsSuccess()) {
        return std::unexpected(Error{"S3Put", outcome.GetError().GetMessage()});
    }
    return {};
}

auto S3Backend::del(std::string_view key) -> VoidResult {
    auto full_key = resolve_key(key);

    Aws::S3::Model::DeleteObjectRequest req;
    req.SetBucket(bucket_);
    req.SetKey(full_key);

    auto outcome = client_->DeleteObject(req);
    if (!outcome.IsSuccess()) {
        return std::unexpected(Error{"S3Del", outcome.GetError().GetMessage()});
    }
    return {};
}

auto S3Backend::prefix_scan(std::string_view prefix) -> Result<std::vector<KVPair>> {
    auto s = stream_scan(prefix);
    if (!s) return std::unexpected(s.error());
    return stream::collect(*s);
}

auto S3Backend::batch(std::span<const BatchOp> ops) -> VoidResult {
    // Partition into puts (sequential) and deletes (batched via DeleteObjects).
    std::vector<std::pair<std::string_view, std::string_view>> puts;
    std::vector<std::string> delete_keys;

    for (const auto& op : ops) {
        if (op.kind == BatchOp::Kind::put) {
            if (!op.value) return std::unexpected(Error{"S3Batch", "put op missing value"});
            puts.emplace_back(op.key, *op.value);
        } else {
            delete_keys.push_back(resolve_key(op.key));
        }
    }

    // Execute puts sequentially (no S3 batch write API)
    for (const auto& [key, value] : puts) {
        auto r = put(key, value);
        if (!r) return r;
    }

    // Batch deletes via DeleteObjects — up to 1000 keys per call
    constexpr std::size_t max_keys_per_call = 1000;
    for (std::size_t i = 0; i < delete_keys.size(); i += max_keys_per_call) {
        auto chunk_end = std::min(i + max_keys_per_call, delete_keys.size());

        Aws::S3::Model::Delete del_batch;
        for (std::size_t j = i; j < chunk_end; ++j) {
            Aws::S3::Model::ObjectIdentifier oid;
            oid.SetKey(delete_keys[j]);
            del_batch.AddObjects(std::move(oid));
        }
        del_batch.SetQuiet(true);  // only report errors

        Aws::S3::Model::DeleteObjectsRequest req;
        req.SetBucket(bucket_);
        req.SetDelete(std::move(del_batch));

        auto outcome = client_->DeleteObjects(req);
        if (!outcome.IsSuccess()) {
            return std::unexpected(Error{"S3BatchDel", outcome.GetError().GetMessage()});
        }

        // Check per-key errors (partial failure)
        const auto& errors = outcome.GetResult().GetErrors();
        if (!errors.empty()) {
            const auto& first = errors[0];
            return std::unexpected(Error{"S3BatchDel",
                std::string(first.GetKey()) + ": " + std::string(first.GetMessage())});
        }
    }

    return {};
}

auto S3Backend::compact() -> VoidResult {
    // S3 is immutable/append-only — compaction is a no-op.
    return {};
}

auto S3Backend::foreach_scan(std::string_view prefix, ScanVisitor visitor, void* user_ctx)
    -> VoidResult {
    auto s = stream_scan(prefix);
    if (!s) return std::unexpected(s.error());
    return stream::drain(*s, [&](const KVPair& kv) {
        visitor(user_ctx, kv.key, kv.value);
    });
}

// ── Streaming extensions — native S3 implementation ──

auto S3Backend::stream_get(std::string_view key) -> Result<StreamHandle<char>> {
    auto full_key = resolve_key(key);
    auto* impl = new S3GetStream{client_, bucket_, full_key, chunk_size_, 0, false};
    return make_stream_handle<char>(impl);
}

auto S3Backend::stream_put(std::string_view key, StreamHandle<char> input) -> VoidResult {
    auto full_key = resolve_key(key);

    // Multipart upload: each chunk from the stream becomes a part.
    Aws::S3::Model::CreateMultipartUploadRequest create_req;
    create_req.SetBucket(bucket_);
    create_req.SetKey(full_key);
    auto create_outcome = client_->CreateMultipartUpload(create_req);
    if (!create_outcome.IsSuccess()) {
        return std::unexpected(Error{"S3StreamPut", create_outcome.GetError().GetMessage()});
    }
    auto upload_id = create_outcome.GetResult().GetUploadId();

    std::vector<Aws::S3::Model::CompletedPart> parts;
    int part_number = 1;

    while (true) {
        auto r = input.pull();
        if (!r) {
            // Abort multipart upload on error
            Aws::S3::Model::AbortMultipartUploadRequest abort_req;
            abort_req.SetBucket(bucket_);
            abort_req.SetKey(full_key);
            abort_req.SetUploadId(upload_id);
            client_->AbortMultipartUpload(abort_req);
            return std::unexpected(r.error());
        }
        if (!r->has_value()) break;

        auto& chunk = r->value();
        auto body = Aws::MakeShared<Aws::StringStream>("celer");
        body->write(chunk.data(), static_cast<std::streamsize>(chunk.size()));

        Aws::S3::Model::UploadPartRequest part_req;
        part_req.SetBucket(bucket_);
        part_req.SetKey(full_key);
        part_req.SetUploadId(upload_id);
        part_req.SetPartNumber(part_number);
        part_req.SetBody(body);

        auto part_outcome = client_->UploadPart(part_req);
        if (!part_outcome.IsSuccess()) {
            Aws::S3::Model::AbortMultipartUploadRequest abort_req;
            abort_req.SetBucket(bucket_);
            abort_req.SetKey(full_key);
            abort_req.SetUploadId(upload_id);
            client_->AbortMultipartUpload(abort_req);
            return std::unexpected(Error{"S3StreamPut", part_outcome.GetError().GetMessage()});
        }

        Aws::S3::Model::CompletedPart completed;
        completed.SetPartNumber(part_number);
        completed.SetETag(part_outcome.GetResult().GetETag());
        parts.push_back(std::move(completed));
        ++part_number;
    }

    // Complete multipart upload
    Aws::S3::Model::CompletedMultipartUpload completed_upload;
    completed_upload.SetParts(std::move(parts));

    Aws::S3::Model::CompleteMultipartUploadRequest complete_req;
    complete_req.SetBucket(bucket_);
    complete_req.SetKey(full_key);
    complete_req.SetUploadId(upload_id);
    complete_req.SetMultipartUpload(completed_upload);

    auto complete_outcome = client_->CompleteMultipartUpload(complete_req);
    if (!complete_outcome.IsSuccess()) {
        return std::unexpected(Error{"S3StreamPut", complete_outcome.GetError().GetMessage()});
    }
    return {};
}

auto S3Backend::stream_scan(std::string_view prefix) -> Result<StreamHandle<KVPair>> {
    auto full_prefix = resolve_key(prefix);
    auto* impl = new S3ListStream{client_, bucket_, full_prefix, "", false};
    return make_stream_handle<KVPair>(impl);
}

// ── Versioned object extensions (RFC-002 §16) ──

namespace {

/// Pull-based stream over S3 ListObjectVersions pagination.
/// Each pull() fetches one page of version metadata.
/// Prototype clone captures markers for independent pagination.
struct S3VersionStreamImpl {
    std::shared_ptr<Aws::S3::S3Client> client;
    std::string bucket;
    std::string prefix;
    std::string key_filter;      // filter to single key if non-empty
    std::string key_marker;
    std::string version_id_marker;
    bool done{false};

    auto pull() -> Result<std::optional<Chunk<backends::s3::VersionInfo>>> {
        if (done) return std::optional<Chunk<backends::s3::VersionInfo>>{};

        Aws::S3::Model::ListObjectVersionsRequest req;
        req.SetBucket(bucket);
        req.SetPrefix(prefix);
        if (!key_marker.empty()) {
            req.SetKeyMarker(key_marker);
        }
        if (!version_id_marker.empty()) {
            req.SetVersionIdMarker(version_id_marker);
        }

        auto outcome = client->ListObjectVersions(req);
        if (!outcome.IsSuccess()) {
            return std::unexpected(Error{"S3ListVersions", outcome.GetError().GetMessage()});
        }

        auto& result = outcome.GetResult();
        if (result.GetIsTruncated()) {
            key_marker = result.GetNextKeyMarker();
            version_id_marker = result.GetNextVersionIdMarker();
        } else {
            done = true;
        }

        std::vector<backends::s3::VersionInfo> versions;
        for (const auto& v : result.GetVersions()) {
            // If filtering to a single key, skip others
            std::string obj_key(v.GetKey());
            if (!key_filter.empty() && obj_key != prefix + key_filter) {
                continue;
            }
            backends::s3::VersionInfo info;
            info.version_id = v.GetVersionId();
            info.last_modified = v.GetLastModified().ToGmtString(Aws::Utils::DateFormat::ISO_8601);
            info.size = static_cast<std::size_t>(v.GetSize());
            info.is_latest = v.GetIsLatest();
            info.is_delete_marker = false;
            versions.push_back(std::move(info));
        }

        // Also include delete markers
        for (const auto& dm : result.GetDeleteMarkers()) {
            std::string obj_key(dm.GetKey());
            if (!key_filter.empty() && obj_key != prefix + key_filter) {
                continue;
            }
            backends::s3::VersionInfo info;
            info.version_id = dm.GetVersionId();
            info.last_modified = dm.GetLastModified().ToGmtString(Aws::Utils::DateFormat::ISO_8601);
            info.size = 0;
            info.is_latest = dm.GetIsLatest();
            info.is_delete_marker = true;
            versions.push_back(std::move(info));
        }

        if (versions.empty()) {
            done = true;
            return std::optional<Chunk<backends::s3::VersionInfo>>{};
        }
        return std::optional{Chunk<backends::s3::VersionInfo>::from(std::move(versions))};
    }

    S3VersionStreamImpl(std::shared_ptr<Aws::S3::S3Client> c,
                        std::string b, std::string p, std::string kf,
                        std::string km, std::string vm, bool d)
        : client(std::move(c)), bucket(std::move(b)), prefix(std::move(p))
        , key_filter(std::move(kf)), key_marker(std::move(km))
        , version_id_marker(std::move(vm)), done(d) {}
    S3VersionStreamImpl(const S3VersionStreamImpl&) = default;
};

} // namespace

auto S3Backend::get_versioned(std::string_view key, std::string_view version_id)
    -> Result<std::optional<std::string>>
{
    auto full_key = resolve_key(key);

    Aws::S3::Model::GetObjectRequest req;
    req.SetBucket(bucket_);
    req.SetKey(full_key);
    req.SetVersionId(std::string(version_id));

    auto outcome = client_->GetObject(req);
    if (!outcome.IsSuccess()) {
        auto& err = outcome.GetError();
        if (err.GetResponseCode() == Aws::Http::HttpResponseCode::NOT_FOUND) {
            return std::optional<std::string>{std::nullopt};
        }
        return std::unexpected(Error{"S3GetVersioned", err.GetMessage()});
    }

    auto& body = outcome.GetResultWithOwnership().GetBody();
    std::string value(std::istreambuf_iterator<char>(body), {});
    return std::optional<std::string>{std::move(value)};
}

auto S3Backend::stream_get_versioned(std::string_view key, std::string_view version_id)
    -> Result<StreamHandle<char>>
{
    auto full_key = resolve_key(key);
    auto* impl = new S3GetStream{
        client_, bucket_, full_key, chunk_size_, 0, false, std::string(version_id)};
    return make_stream_handle<char>(impl);
}

auto S3Backend::put_versioned(std::string_view key, std::string_view value)
    -> Result<backends::s3::PutResult>
{
    auto full_key = resolve_key(key);

    Aws::S3::Model::PutObjectRequest req;
    req.SetBucket(bucket_);
    req.SetKey(full_key);

    auto body = Aws::MakeShared<Aws::StringStream>("celer");
    body->write(value.data(), static_cast<std::streamsize>(value.size()));
    req.SetBody(body);

    auto outcome = client_->PutObject(req);
    if (!outcome.IsSuccess()) {
        return std::unexpected(Error{"S3PutVersioned", outcome.GetError().GetMessage()});
    }

    backends::s3::PutResult result;
    result.version_id = outcome.GetResult().GetVersionId();
    return result;
}

auto S3Backend::list_versions(std::string_view key)
    -> Result<StreamHandle<backends::s3::VersionInfo>>
{
    auto full_prefix = key_prefix_;
    auto* impl = new S3VersionStreamImpl{
        client_, bucket_, full_prefix, std::string(key), "", "", false};
    return make_stream_handle<backends::s3::VersionInfo>(impl);
}

auto S3Backend::close() -> VoidResult {
    client_.reset();
    return {};
}

namespace backends::s3 {

auto factory(Config cfg) -> BackendFactory {
    // Shared S3 client across all tables — thread-safe by AWS SDK contract.
    Aws::Client::ClientConfiguration aws_cfg;
    aws_cfg.region = cfg.region;
    aws_cfg.requestTimeoutMs = cfg.timeout_ms;
    aws_cfg.retryStrategy = Aws::MakeShared<Aws::Client::DefaultRetryStrategy>("celer", cfg.max_retries);

    if (!cfg.endpoint_url.empty()) {
        aws_cfg.endpointOverride = cfg.endpoint_url;
    }

    // Resolve credential provider from Config.
    std::shared_ptr<Aws::S3::S3Client> client;

    std::visit([&](auto&& cred) {
        using Cred = std::decay_t<decltype(cred)>;

        if constexpr (std::is_same_v<Cred, aws::AccessKey>) {
            // Static credentials — no STS, no chain lookup.
            auto provider = Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
                "celer",
                Aws::String(cred.access_key_id.data(), cred.access_key_id.size()),
                Aws::String(cred.secret_access_key.data(), cred.secret_access_key.size()),
                Aws::String(cred.session_token.data(), cred.session_token.size()));
            client = std::make_shared<Aws::S3::S3Client>(provider, aws_cfg,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                cfg.use_path_style);

        } else if constexpr (std::is_same_v<Cred, aws::RoleArn>) {
            // Assume-role via STS — call AssumeRole, extract temp creds.
            std::shared_ptr<Aws::Auth::AWSCredentialsProvider> seed;
            if (cred.seed_credentials && cred.seed_credentials->valid()) {
                seed = Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
                    "celer",
                    Aws::String(cred.seed_credentials->access_key_id.data(),
                                cred.seed_credentials->access_key_id.size()),
                    Aws::String(cred.seed_credentials->secret_access_key.data(),
                                cred.seed_credentials->secret_access_key.size()),
                    Aws::String(cred.seed_credentials->session_token.data(),
                                cred.seed_credentials->session_token.size()));
            } else {
                seed = Aws::MakeShared<Aws::Auth::DefaultAWSCredentialsProviderChain>("celer");
            }

            Aws::STS::STSClient sts_client(seed, aws_cfg);

            Aws::STS::Model::AssumeRoleRequest sts_req;
            sts_req.SetRoleArn(Aws::String(cred.role_arn.data(), cred.role_arn.size()));
            sts_req.SetRoleSessionName(Aws::String(cred.session_name.data(), cred.session_name.size()));
            if (!cred.external_id.empty()) {
                sts_req.SetExternalId(Aws::String(cred.external_id.data(), cred.external_id.size()));
            }
            sts_req.SetDurationSeconds(static_cast<int>(cred.duration_seconds));

            auto sts_outcome = sts_client.AssumeRole(sts_req);
            if (!sts_outcome.IsSuccess()) {
                std::fprintf(stderr, "[celer] STS AssumeRole failed: %s\n",
                    sts_outcome.GetError().GetMessage().c_str());
                // Fall back to default chain on STS failure — client will fail at first S3 call.
                client = std::make_shared<Aws::S3::S3Client>(aws_cfg,
                    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                    cfg.use_path_style);
            } else {
                const auto& sts_creds = sts_outcome.GetResult().GetCredentials();
                auto role_provider = Aws::MakeShared<Aws::Auth::SimpleAWSCredentialsProvider>(
                    "celer",
                    sts_creds.GetAccessKeyId(),
                    sts_creds.GetSecretAccessKey(),
                    sts_creds.GetSessionToken());
                client = std::make_shared<Aws::S3::S3Client>(role_provider, aws_cfg,
                    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                    cfg.use_path_style);
            }

        } else {
            // DefaultChain — let the SDK resolve.
            client = std::make_shared<Aws::S3::S3Client>(aws_cfg,
                Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
                cfg.use_path_style);
        }
    }, cfg.credentials);

    return [c = std::move(cfg), client = std::move(client)]
           (std::string_view scope, std::string_view table) -> Result<BackendHandle> {
        auto key_prefix = c.prefix + std::string(scope) + "/" + std::string(table) + "/";
        auto* backend = new S3Backend(client, c.bucket, key_prefix, c.chunk_size, c.versioning_enabled);
        return make_backend_handle<S3Backend>(backend);
    };
}

} // namespace backends::s3

#else // CELER_HAS_S3 == 0

namespace backends::s3 {

auto factory(Config /*cfg*/) -> BackendFactory {
    return [](std::string_view, std::string_view) -> Result<BackendHandle> {
        return std::unexpected(Error{"NotAvailable", "compiled without S3 support (aws-sdk-cpp)"});
    };
}

} // namespace backends::s3

#endif

} // namespace celer
