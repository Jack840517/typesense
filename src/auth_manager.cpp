#include "auth_manager.h"
#include <openssl/evp.h>
#include <regex>

constexpr const char* AuthManager::DOCUMENTS_SEARCH_ACTION;
constexpr const uint64_t api_key_t::FAR_FUTURE_TIMESTAMP;

Option<bool> AuthManager::init(Store* store, const std::string& bootstrap_auth_key) {
    // This function must be idempotent, i.e. when called multiple times, must produce the same state without leaks
    //LOG(INFO) << "AuthManager::init()";
    std::unique_lock lock(mutex);

    this->store = store;
    this->bootstrap_auth_key = bootstrap_auth_key;

    std::string next_api_key_id_str;
    StoreStatus next_api_key_id_status = store->get(API_KEY_NEXT_ID_KEY, next_api_key_id_str);

    if(next_api_key_id_status == StoreStatus::ERROR) {
        return Option<bool>(500, "Error while fetching the next API key id from the store.");
    }

    if(next_api_key_id_status == StoreStatus::FOUND) {
        next_api_key_id = (uint32_t) StringUtils::deserialize_uint32_t(next_api_key_id_str);
    } else {
        next_api_key_id = 0;
    }

    std::vector<std::string> api_key_json_strs;
    store->scan_fill(API_KEYS_PREFIX, api_key_json_strs);

    LOG(INFO) << "Indexing " << api_key_json_strs.size() << " API key(s) found on disk.";

    for(auto & api_key_json_str: api_key_json_strs) {
        api_key_t api_key;
        Option<bool> load_op = api_key.load(api_key_json_str);
        if(!load_op.ok()) {
            return Option<bool>(load_op.code(), load_op.error());
        }

        api_keys.emplace(api_key.value, api_key);
    }

    return Option<bool>(true);
}

Option<std::vector<api_key_t>> AuthManager::list_keys() const {
    std::shared_lock lock(mutex);

    std::vector<std::string> api_key_json_strs;
    store->scan_fill(API_KEYS_PREFIX, api_key_json_strs);

    std::vector<api_key_t> stored_api_keys;

    for(const auto& api_key_json_str: api_key_json_strs) {
        api_key_t api_key;
        Option<bool> load_op = api_key.load(api_key_json_str);
        if(!load_op.ok()) {
            return Option<std::vector<api_key_t>>(load_op.code(), load_op.error());
        }

        stored_api_keys.push_back(api_key.truncate_value());
    }

    return Option<std::vector<api_key_t>>(stored_api_keys);
}

Option<api_key_t> AuthManager::get_key(uint32_t id, bool truncate_value) const {
    std::shared_lock lock(mutex);

    std::string api_key_store_key = std::string(API_KEYS_PREFIX) + "_" + std::to_string(id);
    std::string api_key_json_str;
    StoreStatus status = store->get(api_key_store_key, api_key_json_str);

    if(status == StoreStatus::FOUND) {
        api_key_t api_key;
        const Option<bool> & load_op = api_key.load(api_key_json_str);
        if(!load_op.ok()) {
            return Option<api_key_t>(load_op.code(), load_op.error());
        }

        if(truncate_value) {
            api_key.truncate_value();
        }
        return Option<api_key_t>(api_key);
    }

    if(status == StoreStatus::NOT_FOUND) {
        return Option<api_key_t>(404, "Not found.");
    }

    return Option<api_key_t>(500, "Error while fetching key from store.");
}

Option<api_key_t> AuthManager::create_key(api_key_t& api_key) {
    //LOG(INFO) << "AuthManager::create_key()";
    std::unique_lock lock(mutex);

    if(api_keys.count(api_key.value) != 0 || api_key.value == bootstrap_auth_key) {
        return Option<api_key_t>(409, "API key generation conflict.");
    }

    api_key.id = get_next_api_key_id();

    std::string api_key_store_key = std::string(API_KEYS_PREFIX) + "_" + std::to_string(api_key.id);
    const nlohmann::json & api_key_obj = api_key.to_json();

    bool inserted = store->insert(api_key_store_key, api_key_obj.dump());
    if(!inserted) {
        return Option<api_key_t>(500, "Could not store generated API key.");
    }

    api_keys.emplace(api_key.value, api_key);
    return Option<api_key_t>(api_key);
}

Option<api_key_t> AuthManager::remove_key(uint32_t id) {
    Option<api_key_t> key_op = get_key(id, false);
    
    if(!key_op.ok()) {
        return Option<api_key_t>(key_op.code(), key_op.error());
    }

    std::string api_key_store_key = std::string(API_KEYS_PREFIX) + "_" + std::to_string(id);
    if(!store->remove(api_key_store_key)) {
        return Option<api_key_t>(500, "Could not delete API key.");
    }

    std::unique_lock lock(mutex);

    api_key_t&& key = key_op.get();
    api_keys.erase(key.value);

    return Option<api_key_t>(key.truncate_value());
}

uint32_t AuthManager::get_next_api_key_id() {
    store->increment(std::string(API_KEY_NEXT_ID_KEY), 1);
    return next_api_key_id++;
}

bool AuthManager::authenticate(const std::string& req_api_key, const std::string& action,
                               const std::vector<std::string>& collections,
                               std::map<std::string, std::string>& params) const {

    std::shared_lock lock(mutex);
    //LOG(INFO) << "AuthManager::authenticate()";

    const auto& key_it = api_keys.find(req_api_key);
    if(key_it != api_keys.end()) {
        const api_key_t& api_key = key_it->second;
        return auth_against_key(collections, action, api_key, false);
    }

    // could be a scoped API key
    nlohmann::json embedded_params;
    Option<bool> auth_op = authenticate_parse_params(req_api_key, action, collections, embedded_params);
    if(!auth_op.ok()) {
        return false;
    }

    // enrich params with values from embedded_params
    for(auto& item: embedded_params.items()) {
        if(item.key() == "expires_at") {
            continue;
        }

        // overwrite = true as embedded params have higher priority
        AuthManager::add_item_to_params(params, item, true);
    }

    //LOG(INFO) << "api_keys.size() = " << api_keys.size();
    return true;
}

bool AuthManager::auth_against_key(const std::vector<std::string>& collections, const std::string& action,
                                   const api_key_t& api_key, const bool search_only) const {

    if(uint64_t(std::time(0)) > api_key.expires_at) {
        LOG(ERROR) << fmt_error("Rejecting expired API key.", api_key.value);
        return false;
    }

    if(search_only) {
        // ensure that parent key has only search scope
        if(api_key.actions.size() != 1 || api_key.actions[0] != DOCUMENTS_SEARCH_ACTION) {
            LOG(ERROR) << fmt_error(std::string("Parent API key must allow only `") + DOCUMENTS_SEARCH_ACTION + "` action.",
                                    api_key.value);
            return false;
        }
    } else {
        bool action_is_allowed = false;
        for(const std::string& allowed_action: api_key.actions) {
            if(allowed_action == "*" || (action != "*" && allowed_action == action)) {
                action_is_allowed = true;
                break;
            }

            // e.g. collections:create or documents:create
            if (allowed_action.size() >= 2 && allowed_action[allowed_action.size() - 2] == ':' &&
                allowed_action.back() == '*') {
                std::string allowed_resource = allowed_action.substr(0, allowed_action.size() - 2);
                std::vector<std::string> actual_action_parts;
                StringUtils::split(action, actual_action_parts, ":");
                if(actual_action_parts[0] == allowed_resource) {
                    action_is_allowed = true;
                    break;
                }
            }
        }

        if(!action_is_allowed) {
            return false;
        }
    }

    for(const std::string& req_collection: collections) {
        bool coll_allowed = false;

        for(const std::string& allowed_collection: api_key.collections) {
            if(allowed_collection == "*" || (allowed_collection == req_collection) || req_collection.empty() ||
               std::regex_match (req_collection, std::regex(allowed_collection))) {
                coll_allowed = true;
                break;
            }
        }

        if(!coll_allowed) {
            // even if one collection is not allowed, we reject the entire request
            return false;
        }
    }

    return true;
}

Option<bool> AuthManager::authenticate_parse_params(const std::string& scoped_api_key, const std::string& action,
                                                    const std::vector<std::string>& collections,
                                                    nlohmann::json& embedded_params) const {

    // allow only searches from scoped keys
    if(action != DOCUMENTS_SEARCH_ACTION) {
        LOG(ERROR) << "Scoped API keys can only be used for searches.";
        return Option<bool>(403, "Forbidden.");
    }

    const std::string& key_payload = StringUtils::base64_decode(scoped_api_key);

    if(key_payload.size() < HMAC_BASE64_LEN + api_key_t::PREFIX_LEN) {
        LOG(ERROR) << "Malformed scoped API key.";
        return Option<bool>(403, "Forbidden.");
    }

    // FORMAT:
    // <DIGEST><PARENT_KEY_PREFIX><PARAMS>
    const std::string& hmacSHA256 = key_payload.substr(0, HMAC_BASE64_LEN);
    const std::string& key_prefix = key_payload.substr(HMAC_BASE64_LEN, api_key_t::PREFIX_LEN);
    const std::string& custom_params = key_payload.substr(HMAC_BASE64_LEN + api_key_t::PREFIX_LEN);

    // calculate and verify hmac against matching api key
    for (const auto &kv : api_keys) {
        if(kv.first.substr(0, api_key_t::PREFIX_LEN) == key_prefix) {
            const api_key_t& api_key = kv.second;

            // ensure that parent key collection filter matches queried collection
            bool auth_success = auth_against_key(collections, action, api_key, true);

            if(!auth_success) {
                LOG(ERROR) << fmt_error("Parent key does not allow queries against queried collection.", api_key.value);
                return Option<bool>(403, "Forbidden.");
            }

            // finally verify hmac
            std::string digest = StringUtils::hmac(kv.first, custom_params);

            if(digest == hmacSHA256) {
                try {
                    embedded_params = nlohmann::json::parse(custom_params);
                } catch(const std::exception& e) {
                    LOG(ERROR) << "JSON error: " << e.what();
                    return Option<bool>(403, "Forbidden.");
                }

                if(!embedded_params.is_object()) {
                    LOG(ERROR) << fmt_error("Scoped API key contains invalid search parameters.", api_key.value);
                    return Option<bool>(403, "Forbidden.");
                }

                if(embedded_params.count("expires_at") != 0) {
                    if(!embedded_params["expires_at"].is_number_integer() || embedded_params["expires_at"].get<int64_t>() < 0) {
                        LOG(ERROR) << fmt_error("Wrong format for `expires_at`. It should be an unsigned integer.", api_key.value);
                        return Option<bool>(403, "Forbidden.");
                    }

                    // if parent key's expiry timestamp is smaller, it takes precedence
                    uint64_t expiry_ts = std::min(api_key.expires_at, embedded_params["expires_at"].get<uint64_t>());

                    if(uint64_t(std::time(0)) > expiry_ts) {
                        LOG(ERROR) << fmt_error("Scoped API key has expired. ", api_key.value);
                        return Option<bool>(403, "Forbidden.");
                    }
                }

                return Option<bool>(true);
            }
        }
    }

    return Option<bool>(403, "Forbidden.");
}

std::string AuthManager::fmt_error(std::string&& error, const std::string& key) {
    std::stringstream ss;
    ss << error << " Key prefix: " << key.substr(0, api_key_t::PREFIX_LEN) << ", SHA256: "
       << StringUtils::hash_sha256(key);
    return ss.str();
}

Option<uint32_t> api_key_t::validate(const nlohmann::json &key_obj) {
    auto mandatory_keys = {
        "description", "actions", "collections"
    };

    for(auto key: mandatory_keys) {
        if(key_obj.count(key) == 0) {
            return Option<uint32_t>(400, std::string("Could not find a `") + key + "` key.");
        }
    }

    if(key_obj.count("value") != 0 && !key_obj["value"].is_string()) {
        return Option<uint32_t>(400, std::string("Key value must be a string."));
    }

    if(key_obj.count("description") != 0 && !key_obj["description"].is_string()) {
        return Option<uint32_t>(400, std::string("Key description must be a string."));
    }

    if(!key_obj["actions"].is_array() || key_obj["actions"].empty()) {
        return Option<uint32_t>(400,"Wrong format for `actions`. It should be an array of string.");
    }

    if(!key_obj["collections"].is_array() || key_obj["collections"].empty()) {
        return Option<uint32_t>(400,"Wrong format for `collections`. It should be an array of string.");
    }

    for(const nlohmann::json & item: key_obj["actions"]) {
        if(!item.is_string()) {
            return Option<uint32_t>(400,"Wrong format for `actions`. It should be an array of string.");
        }
    }

    for(const nlohmann::json & item: key_obj["collections"]) {
        if(!item.is_string()) {
            return Option<uint32_t>(400,"Wrong format for `collections`. It should be an array of string.");
        }
    }

    if(key_obj.count("expires_at") != 0) {
        if(!key_obj["expires_at"].is_number_integer() || key_obj["expires_at"].get<int64_t>() < 0) {
            return Option<uint32_t>(400,"Wrong format for `expires_at`. It should be an unsigned integer.");
        }
    }

    return Option<uint32_t>(200);
}


bool AuthManager::add_item_to_params(std::map<std::string, std::string>& req_params,
                                     nlohmann::detail::iteration_proxy_value<nlohmann::json::iterator>& item,
                                     bool overwrite) {

    std::string str_value;

    if(item.value().is_string()) {
        str_value = item.value().get<std::string>();
    } else if(item.value().is_number_integer()) {
        str_value = std::to_string(item.value().get<int64_t>());
    } else if(item.value().is_number_float()) {
        str_value = std::to_string(item.value().get<float>());
    } else if(item.value().is_boolean()) {
        str_value = item.value().get<bool>() ? "true" : "false";
    } else {
        return false;
    }

    if(req_params.count(item.key()) == 0) {
        req_params[item.key()] = str_value;
    } else if(item.key() == "filter_by") {
        req_params[item.key()] = req_params[item.key()] + "&&" + str_value;
    } else if(overwrite) {
        req_params[item.key()] = str_value;
    }

    return true;
}


