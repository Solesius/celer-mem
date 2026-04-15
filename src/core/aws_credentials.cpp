#include "celer/core/aws_credentials.hpp"

#include <cstdlib>

namespace celer::aws {

auto from_env() -> AwsCredentials {
    // Check for role ARN first — takes priority (assume-role overrides static keys).
    const char* role_arn = std::getenv("AWS_ROLE_ARN");
    if (role_arn && role_arn[0] != '\0') {
        RoleArn arn;
        arn.role_arn = role_arn;

        const char* session = std::getenv("AWS_ROLE_SESSION_NAME");
        if (session && session[0] != '\0') arn.session_name = session;

        const char* ext_id = std::getenv("AWS_EXTERNAL_ID");
        if (ext_id && ext_id[0] != '\0') arn.external_id = ext_id;

        // If static keys are also set, use them as seed credentials for STS.
        const char* key_id = std::getenv("AWS_ACCESS_KEY_ID");
        const char* secret = std::getenv("AWS_SECRET_ACCESS_KEY");
        if (key_id && key_id[0] != '\0' && secret && secret[0] != '\0') {
            AccessKey seed;
            seed.access_key_id = key_id;
            seed.secret_access_key = secret;
            const char* token = std::getenv("AWS_SESSION_TOKEN");
            if (token && token[0] != '\0') seed.session_token = token;
            arn.seed_credentials = std::move(seed);
        }

        return arn;
    }

    // Check for static access key.
    const char* key_id = std::getenv("AWS_ACCESS_KEY_ID");
    const char* secret = std::getenv("AWS_SECRET_ACCESS_KEY");
    if (key_id && key_id[0] != '\0' && secret && secret[0] != '\0') {
        AccessKey ak;
        ak.access_key_id = key_id;
        ak.secret_access_key = secret;
        const char* token = std::getenv("AWS_SESSION_TOKEN");
        if (token && token[0] != '\0') ak.session_token = token;
        return ak;
    }

    // Nothing explicit — fall back to SDK default chain.
    return DefaultChain{};
}

} // namespace celer::aws
