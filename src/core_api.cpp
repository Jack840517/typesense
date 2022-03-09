#include <chrono>
#include <thread>
#include <app_metrics.h>
#include "typesense_server_utils.h"
#include "core_api.h"
#include "string_utils.h"
#include "collection.h"
#include "collection_manager.h"
#include "system_metrics.h"
#include "logger.h"
#include "core_api_utils.h"
#include "lru/lru.hpp"

using namespace std::chrono_literals;

std::shared_mutex mutex;
LRU::Cache<uint64_t, cached_res_t> res_cache;

bool handle_authentication(std::map<std::string, std::string>& req_params, const std::string& body,
                           const route_path& rpath, const std::string& auth_key) {
    CollectionManager & collectionManager = CollectionManager::get_instance();

    std::vector<std::string> collections;

    get_collections_for_auth(req_params, body, rpath, collections);

    if(rpath.handler == get_health) {
        // health endpoint requires no authentication
        return true;
    }

    return collectionManager.auth_key_matches(auth_key, rpath.action, collections, req_params);
}

void stream_response(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    if(!res->is_alive) {
        // underlying request is dead or this is a raft log playback
        return ;
    }

    if(req->_req->res.status != 0) {
        // not the first response chunk, so wait for previous chunk to finish
        res->wait();
    }

    auto req_res = new async_req_res_t(req, res, true);
    server->get_message_dispatcher()->send_message(HttpServer::STREAM_RESPONSE_MESSAGE, req_res);
}

void defer_processing(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res, size_t timeout_ms) {
    defer_processing_t* defer = new defer_processing_t(req, res, timeout_ms, server);
    //LOG(INFO) << "core_api req " << req.get() << ", use count: " << req.use_count();
    server->get_message_dispatcher()->send_message(HttpServer::DEFER_PROCESSING_MESSAGE, defer);
}

void get_collections_for_auth(std::map<std::string, std::string> &req_params, const std::string &body,
                              const route_path &rpath, std::vector<std::string> &collections) {
    if(req_params.count("collection") != 0) {
        collections.emplace_back(req_params.at("collection"));
    }

    if(rpath.handler == post_multi_search) {
        nlohmann::json obj = nlohmann::json::parse(body, nullptr, false);

        if(obj == nlohmann::json::value_t::discarded) {
            LOG(ERROR) << "Multi search request body is malformed.";
        }

        if(obj != nlohmann::json::value_t::discarded && obj.count("searches") != 0 && obj["searches"].is_array()) {
            for(auto& el : obj["searches"]) {
                if(el.is_object() && el.count("collection") != 0) {
                    collections.emplace_back(el["collection"].get<std::string>());
                }
            }
        }
    } else if(rpath.handler == post_create_collection) {
        nlohmann::json obj = nlohmann::json::parse(body, nullptr, false);

        if(obj == nlohmann::json::value_t::discarded) {
            LOG(ERROR) << "Create collection request body is malformed.";
        }

        if(obj != nlohmann::json::value_t::discarded && obj.count("name") != 0 && obj["name"].is_string()) {
            collections.emplace_back(obj["name"].get<std::string>());
        }
    }

    else if(collections.empty()) {
        collections.emplace_back("");
    }
}

index_operation_t get_index_operation(const std::string& action) {
    if(action == "create") {
        return CREATE;
    } else if(action == "update") {
        return UPDATE;
    } else if(action == "upsert") {
        return UPSERT;
    }

    return CREATE;
}

bool get_collections(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    nlohmann::json json_response = collectionManager.get_collection_summaries();
    res->set_200(json_response.dump());
    return true;
}

bool post_create_collection(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json req_json;

    try {
        req_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        //LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }

    CollectionManager & collectionManager = CollectionManager::get_instance();
    const Option<Collection*> & collection_op = collectionManager.create_collection(req_json);

    if(collection_op.ok()) {
        nlohmann::json json_response = collection_op.get()->get_summary_json();
        res->set_201(json_response.dump());
        return true;
    }

    res->set(collection_op.code(), collection_op.error());
    return false;
}

bool del_drop_collection(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    std::string doc_id = req->params["id"];
    CollectionManager & collectionManager = CollectionManager::get_instance();
    Option<nlohmann::json> drop_op = collectionManager.drop_collection(req->params["collection"], true);

    if(!drop_op.ok()) {
        res->set(drop_op.code(), drop_op.error());
        return false;
    }

    res->set_200(drop_op.get().dump());
    return true;
}

bool get_debug(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json result;
    result["version"] = server->get_version();

    uint64_t state = server->node_state();
    result["state"] = state;

    res->set_200(result.dump());
    return true;
}

bool get_health(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json result;
    bool alive = server->is_alive();
    result["ok"] = alive;

    if(alive) {
        res->set_body(200, result.dump());
    } else {
        res->set_body(503, result.dump());
    }

    return alive;
}

bool post_health(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json result;
    bool alive = server->is_alive();
    result["ok"] = alive;

    if(alive) {
        res->set_body(200, result.dump());
    } else {
        res->set_body(503, result.dump());
    }

    return alive;
}

bool get_metrics_json(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json result;

    CollectionManager & collectionManager = CollectionManager::get_instance();
    const std::string & data_dir_path = collectionManager.get_store()->get_state_dir_path();

    SystemMetrics sys_metrics;
    sys_metrics.get(data_dir_path, result);

    res->set_body(200, result.dump(2));
    return true;
}

bool get_stats_json(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json result;
    AppMetrics::get_instance().get("requests_per_second", "latency_ms", result);
    result["pending_write_batches"] = server->get_num_queued_writes();

    res->set_body(200, result.dump(2));
    return true;
}

bool get_log_sequence(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    const uint64_t seq_num = collectionManager.get_store()->get_latest_seq_number();
    res->content_type_header = "text/plain; charset=utf8";
    res->set_body(200, std::to_string(seq_num));
    return true;
}

uint64_t hash_request(const std::shared_ptr<http_req>& req) {
    std::stringstream ss;
    ss << req->route_hash << req->body;

    for(auto& kv: req->params) {
        if(kv.first != "use_cache") {
            ss << kv.second;
        }
    }

    const std::string& req_str = ss.str();
    return StringUtils::hash_wy(req_str.c_str(), req_str.size());
}

bool get_search(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const auto use_cache_it = req->params.find("use_cache");
    bool use_cache = (use_cache_it != req->params.end()) && (use_cache_it->second == "1" || use_cache_it->second == "true");
    uint64_t req_hash = 0;

    if(use_cache) {
        // cache enabled, let's check if request is already in the cache
        req_hash = hash_request(req);

        //LOG(INFO) << "req_hash = " << req_hash;

        std::shared_lock lock(mutex);
        auto hit_it = res_cache.find(req_hash);
        if(hit_it != res_cache.end()) {
            //LOG(INFO) << "Result found in cache.";
            const auto& cached_value = hit_it.value();

            // we still need to check that TTL has not expired
            uint32_t ttl = cached_value.ttl;
            uint64_t seconds_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::high_resolution_clock::now() - cached_value.created_at).count();

            if(seconds_elapsed < cached_value.ttl) {
                res->set_content(cached_value.status_code, cached_value.content_type_header, cached_value.body, true);
                return true;
            }

            //LOG(INFO) << "Result found in cache but ttl lapsed.";
        }
    }

    std::string results_json_str;
    Option<bool> search_op = CollectionManager::do_search(req->params, results_json_str);

    if(!search_op.ok()) {
        res->set(search_op.code(), search_op.error());
        return false;
    }

    res->set_200(results_json_str);

    // we will cache only successful requests
    if(use_cache) {
        //LOG(INFO) << "Adding to cache, key = " << req_hash;
        auto now = std::chrono::high_resolution_clock::now();
        const auto cache_ttl_it = req->params.find("cache_ttl");
        uint32_t cache_ttl = 60;
        if(cache_ttl_it != req->params.end() && StringUtils::is_int32_t(cache_ttl_it->second)) {
            cache_ttl = std::stoul(cache_ttl_it->second);
        }

        cached_res_t cached_res;
        cached_res.load(res->status_code, res->content_type_header, res->body, now, cache_ttl, req_hash);

        std::unique_lock lock(mutex);
        res_cache.insert(req_hash, cached_res);
    }

    return true;
}

bool post_multi_search(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const auto use_cache_it = req->params.find("use_cache");
    bool use_cache = (use_cache_it != req->params.end()) && (use_cache_it->second == "1" || use_cache_it->second == "true");
    uint64_t req_hash = 0;

    if(use_cache) {
        // cache enabled, let's check if request is already in the cache
        req_hash = hash_request(req);

        //LOG(INFO) << "req_hash = " << req_hash;

        std::shared_lock lock(mutex);
        auto hit_it = res_cache.find(req_hash);
        if(hit_it != res_cache.end()) {
            //LOG(INFO) << "Result found in cache.";
            const auto& cached_value = hit_it.value();

            // we still need to check that TTL has not expired
            uint32_t ttl = cached_value.ttl;
            uint64_t seconds_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::high_resolution_clock::now() - cached_value.created_at).count();

            if(seconds_elapsed < cached_value.ttl) {
                res->set_content(cached_value.status_code, cached_value.content_type_header, cached_value.body, true);
                return true;
            }
        }
    }

    nlohmann::json req_json;

    try {
        req_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }

    if(req_json.count("searches") == 0) {
        res->set_400("Missing `searches` array.");
        return false;
    }

    if(!req_json["searches"].is_array()) {
        res->set_400("Missing `searches` array.");
        return false;
    }

    const char* LIMIT_MULTI_SEARCHES = "limit_multi_searches";
    size_t limit_multi_searches = 50;

    if(req->params.count(LIMIT_MULTI_SEARCHES) != 0 && StringUtils::is_uint32_t(req->params[LIMIT_MULTI_SEARCHES])) {
        limit_multi_searches = std::stoi(req->params[LIMIT_MULTI_SEARCHES]);
    }

    if(req_json["searches"].size() > limit_multi_searches) {
        res->set_400(std::string("Number of multi searches exceeds `") + LIMIT_MULTI_SEARCHES + "` parameter.");
        return false;
    }

    auto orig_req_params = req->params;

    nlohmann::json response;
    response["results"] = nlohmann::json::array();

    nlohmann::json& searches = req_json["searches"];

    for(auto& search_params: searches) {
        if(!search_params.is_object()) {
            res->set_400("The value of `searches` must be an array of objects.");
            return false;
        }

        req->params = orig_req_params;

        for(auto& search_item: search_params.items()) {
            if(search_item.key() == "cache_ttl") {
                // cache ttl can be applied only from an embedded key: cannot be a multi search param
                continue;
            }

            // overwrite = false since req params will contain embedded params and so has higher priority
            bool populated = AuthManager::add_item_to_params(req->params, search_item, false);
            if(!populated) {
                res->set_400("One or more search parameters are malformed.");
                return false;
            }
        }

        std::string results_json_str;
        Option<bool> search_op = CollectionManager::do_search(req->params, results_json_str);

        if(search_op.ok()) {
            response["results"].push_back(nlohmann::json::parse(results_json_str));
        } else {
            nlohmann::json err_res;
            err_res["error"] = search_op.error();
            err_res["code"] = search_op.code();
            response["results"].push_back(err_res);
        }
    }

    res->set_200(response.dump());

    // we will cache only successful requests
    if(use_cache) {
        //LOG(INFO) << "Adding to cache, key = " << req_hash;
        auto now = std::chrono::high_resolution_clock::now();
        const auto cache_ttl_it = req->params.find("cache_ttl");
        uint32_t cache_ttl = 60;
        if(cache_ttl_it != req->params.end() && StringUtils::is_int32_t(cache_ttl_it->second)) {
            cache_ttl = std::stoul(cache_ttl_it->second);
        }

        cached_res_t cached_res;
        cached_res.load(res->status_code, res->content_type_header, res->body, now, cache_ttl, req_hash);

        std::unique_lock lock(mutex);
        res_cache.insert(req_hash, cached_res);
    }

    return true;
}

bool get_collection_summary(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager& collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    nlohmann::json json_response = collection->get_summary_json();
    res->set_200(json_response.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore));

    return true;
}

bool get_export_documents(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    // NOTE: this is a streaming response end-point so this handler will be called multiple times
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        req->last_chunk_aggregate = true;
        res->final = true;
        res->set_404();
        stream_response(req, res);
        return false;
    }

    const char* FILTER_BY = "filter_by";
    const char* INCLUDE_FIELDS = "include_fields";
    const char* EXCLUDE_FIELDS = "exclude_fields";

    export_state_t* export_state = nullptr;

    const std::string seq_id_prefix = collection->get_seq_id_collection_prefix();

    if(req->data == nullptr) {
        export_state = new export_state_t();

        std::string simple_filter_query;

        if(req->params.count(FILTER_BY) != 0) {
            simple_filter_query = req->params[FILTER_BY];
        }

        if(req->params.count(INCLUDE_FIELDS) != 0) {
            std::vector<std::string> include_fields_vec;
            StringUtils::split(req->params[INCLUDE_FIELDS], include_fields_vec, ",");
            export_state->include_fields = std::set<std::string>(include_fields_vec.begin(), include_fields_vec.end());
        }

        if(req->params.count(EXCLUDE_FIELDS) != 0) {
            std::vector<std::string> exclude_fields_vec;
            StringUtils::split(req->params[EXCLUDE_FIELDS], exclude_fields_vec, ",");
            export_state->exclude_fields = std::set<std::string>(exclude_fields_vec.begin(), exclude_fields_vec.end());
        }

        if(simple_filter_query.empty()) {
            export_state->it = collectionManager.get_store()->scan(seq_id_prefix);
        } else {
            auto filter_ids_op = collection->get_filter_ids(simple_filter_query, export_state->index_ids);

            if(!filter_ids_op.ok()) {
                res->set(filter_ids_op.code(), filter_ids_op.error());
                req->last_chunk_aggregate = true;
                res->final = true;
                stream_response(req, res);
                delete export_state;
                return false;
            }

            for(size_t i=0; i<export_state->index_ids.size(); i++) {
                export_state->offsets.push_back(0);
            }
            export_state->res_body = &res->body;
            export_state->collection = collection.get();
        }
    } else {
        export_state = static_cast<export_state_t*>(req->data);
    }

    req->data = export_state;

    if(export_state->it != nullptr) {
        rocksdb::Iterator* it = export_state->it;

        if(it->Valid() && it->key().ToString().compare(0, seq_id_prefix.size(), seq_id_prefix) == 0) {
            if(export_state->include_fields.empty() && export_state->exclude_fields.empty()) {
                res->body = it->value().ToString();
            } else {
                nlohmann::json doc = nlohmann::json::parse(it->value().ToString());
                nlohmann::json filtered_doc;
                for(const auto& kv: doc.items()) {
                    bool must_include = export_state->include_fields.empty() ||
                                        (export_state->include_fields.count(kv.key()) != 0);

                    bool must_exclude = !export_state->exclude_fields.empty() &&
                                        (export_state->exclude_fields.count(kv.key()) != 0);

                    if(must_include && !must_exclude) {
                        filtered_doc[kv.key()] = kv.value();
                    }
                }

                res->body = filtered_doc.dump();
            }

            it->Next();

            // append a new line character if there is going to be one more record to send
            if(it->Valid() && it->key().ToString().compare(0, seq_id_prefix.size(), seq_id_prefix) == 0) {
                res->body += "\n";
                req->last_chunk_aggregate = false;
                res->final = false;
            } else {
                req->last_chunk_aggregate = true;
                res->final = true;
                delete export_state;
                req->data = nullptr;
            }
        }
    } else {
        bool done;
        stateful_export_docs(export_state, 100, done);

        if(!done) {
            req->last_chunk_aggregate = false;
            res->final = false;
        } else {
            req->last_chunk_aggregate = true;
            res->final = true;
            delete export_state;
            req->data = nullptr;
        }
    }

    res->content_type_header = "application/octet-stream";
    res->status_code = 200;

    stream_response(req, res);
    return true;
}

bool post_import_documents(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    //LOG(INFO) << "Import, req->body_index=" << req->body_index << ", body size: " << req->body.size();
    //LOG(INFO) << "req->first_chunk=" << req->first_chunk_aggregate << ", last_chunk=" << req->last_chunk_aggregate;

    const char *BATCH_SIZE = "batch_size";
    const char *ACTION = "action";
    const char *DIRTY_VALUES = "dirty_values";

    if(req->params.count(BATCH_SIZE) == 0) {
        req->params[BATCH_SIZE] = "40";
    }

    if(req->params.count(ACTION) == 0) {
        req->params[ACTION] = "create";
    }

    if(req->params.count(DIRTY_VALUES) == 0) {
        req->params[DIRTY_VALUES] = "";  // set it empty as default will depend on `index_all_fields`
    }

    if(!StringUtils::is_uint32_t(req->params[BATCH_SIZE])) {
        res->final = true;
        res->set_400("Parameter `" + std::string(BATCH_SIZE) + "` must be a positive integer.");
        stream_response(req, res);
        return false;
    }

    if(req->params[ACTION] != "create" && req->params[ACTION] != "update" && req->params[ACTION] != "upsert") {
        res->final = true;
        res->set_400("Parameter `" + std::string(ACTION) + "` must be a create|update|upsert.");
        stream_response(req, res);
        return false;
    }

    const size_t IMPORT_BATCH_SIZE = std::stoi(req->params[BATCH_SIZE]);

    if(IMPORT_BATCH_SIZE == 0) {
        res->final = true;
        res->set_400("Parameter `" + std::string(BATCH_SIZE) + "` must be a positive integer.");
        stream_response(req, res);
        return false;
    }

    if(req->body_index == 0) {
        // will log for every major chunk of request body
        //LOG(INFO) << "Import, req->body.size=" << req->body.size() << ", batch_size=" << IMPORT_BATCH_SIZE;
        //int nminusten_pos = std::max(0, int(req->body.size())-10);
        //LOG(INFO) << "Last 10 chars: " << req->body.substr(nminusten_pos);
    }

    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        //LOG(INFO) << "collection == nullptr, for collection: " << req->params["collection"];
        res->final = true;
        res->set_404();
        stream_response(req, res);
        return false;
    }

    //LOG(INFO) << "Import, " << "req->body_index=" << req->body_index << ", req->body.size: " << req->body.size();
    //LOG(INFO) << "req body %: " << (float(req->body_index)/req->body.size())*100;

    std::vector<std::string> json_lines;
    StringUtils::split(req->body, json_lines, "\n", false);

    //LOG(INFO) << "json_lines.size before: " << json_lines.size() << ", req->body_index: " << req->body_index;

    if(req->last_chunk_aggregate) {
        //LOG(INFO) << "req->last_chunk_aggregate is true";
        req->body = "";
    } else {
        if(!json_lines.empty()) {
            // check if req->body had complete last record
            bool complete_document;

            try {
                nlohmann::json document = nlohmann::json::parse(json_lines.back());
                complete_document = document.is_object();
            } catch(const std::exception& e) {
                complete_document = false;
            }

            if(!complete_document) {
                // eject partial record
                req->body = json_lines.back();
                json_lines.pop_back();
            } else {
                req->body = "";
            }
        }
    }

    //LOG(INFO) << "json_lines.size after: " << json_lines.size() << ", stream_proceed: " << stream_proceed;
    //LOG(INFO) << "json_lines.size: " << json_lines.size() << ", req->res_state: " << req->res_state;

    // When only one partial record arrives as a chunk, an empty body is pushed to response stream
    bool single_partial_record_body = (json_lines.empty() && !req->body.empty());
    std::stringstream response_stream;

    //LOG(INFO) << "single_partial_record_body: " << single_partial_record_body;

    const index_operation_t operation = get_index_operation(req->params[ACTION]);

    if(!single_partial_record_body) {
        nlohmann::json document;

        const auto& dirty_values = collection->parse_dirty_values_option(req->params[DIRTY_VALUES]);
        nlohmann::json json_res = collection->add_many(json_lines, document, operation, "",
                                                       dirty_values);
        //const std::string& import_summary_json = json_res->dump();
        //response_stream << import_summary_json << "\n";

        for (size_t i = 0; i < json_lines.size(); i++) {
            bool res_final = req->last_chunk_aggregate && (i == json_lines.size()-1);

            if(res_final) {
                // indicates last record of last batch
                response_stream << json_lines[i];
            } else {
                response_stream << json_lines[i] << "\n";
            }
        }
    }

    res->content_type_header = "text/plain; charset=utf8";
    res->status_code = 200;
    res->body = response_stream.str();

    res->final.store(req->last_chunk_aggregate);
    stream_response(req, res);

    return true;
}

bool post_add_document(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const char *ACTION = "action";
    const char *DIRTY_VALUES_PARAM = "dirty_values";

    if(req->params.count(ACTION) == 0) {
        req->params[ACTION] = "create";
    }

    if(req->params[ACTION] != "create" && req->params[ACTION] != "update" && req->params[ACTION] != "upsert") {
        res->set_400("Parameter `" + std::string(ACTION) + "` must be a create|update|upsert.");
        return false;
    }

    if(req->params.count(DIRTY_VALUES_PARAM) == 0) {
        req->params[DIRTY_VALUES_PARAM] = "";  // set it empty as default will depend on whether schema is enabled
    }

    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    const index_operation_t operation = get_index_operation(req->params[ACTION]);
    const auto& dirty_values = collection->parse_dirty_values_option(req->params[DIRTY_VALUES_PARAM]);

    Option<nlohmann::json> inserted_doc_op = collection->add(req->body, operation, "", dirty_values);

    if(!inserted_doc_op.ok()) {
        res->set(inserted_doc_op.code(), inserted_doc_op.error());
        return false;
    }

    res->set_201(inserted_doc_op.get().dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore));
    return true;
}

bool patch_update_document(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    std::string doc_id = req->params["id"];

    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    const char* DIRTY_VALUES_PARAM = "dirty_values";

    if(req->params.count(DIRTY_VALUES_PARAM) == 0) {
        req->params[DIRTY_VALUES_PARAM] = "";  // set it empty as default will depend on whether schema is enabled
    }

    const auto& dirty_values = collection->parse_dirty_values_option(req->params[DIRTY_VALUES_PARAM]);
    Option<nlohmann::json> upserted_doc_op = collection->add(req->body, index_operation_t::UPDATE, doc_id, dirty_values);

    if(!upserted_doc_op.ok()) {
        res->set(upserted_doc_op.code(), upserted_doc_op.error());
        return false;
    }

    res->set_201(upserted_doc_op.get().dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore));
    return true;
}

bool get_fetch_document(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    std::string doc_id = req->params["id"];

    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);
    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    Option<nlohmann::json> doc_option = collection->get(doc_id);

    if(!doc_option.ok()) {
        res->set(doc_option.code(), doc_option.error());
        return false;
    }

    res->set_200(doc_option.get().dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore));
    return true;
}

bool del_remove_document(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    std::string doc_id = req->params["id"];

    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);
    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    Option<nlohmann::json> doc_option = collection->get(doc_id);

    if(!doc_option.ok()) {
        res->set(doc_option.code(), doc_option.error());
        return false;
    }

    Option<std::string> deleted_id_op = collection->remove(doc_id);

    if(!deleted_id_op.ok()) {
        res->set(deleted_id_op.code(), deleted_id_op.error());
        return false;
    }

    nlohmann::json doc = doc_option.get();
    res->set_200(doc.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore));
    return true;
}

bool del_remove_documents(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    // defaults: will get overridden later if needed
    res->content_type_header = "application/json";
    res->status_code = 200;

    // NOTE: this is a streaming response end-point so this handler will be called multiple times
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        req->last_chunk_aggregate = true;
        res->final = true;
        res->set_404();
        stream_response(req, res);
        return false;
    }

    const char *BATCH_SIZE = "batch_size";
    const char *FILTER_BY = "filter_by";

    if(req->params.count(BATCH_SIZE) == 0) {
        req->params[BATCH_SIZE] = "40";
    }

    if(req->params.count(FILTER_BY) == 0) {
        req->last_chunk_aggregate = true;
        res->final = true;
        res->set_400("Parameter `" + std::string(FILTER_BY) + "` must be provided.");
        stream_response(req, res);
        return false;
    }

    if(!StringUtils::is_uint32_t(req->params[BATCH_SIZE])) {
        req->last_chunk_aggregate = true;
        res->final = true;
        res->set_400("Parameter `" + std::string(BATCH_SIZE) + "` must be a positive integer.");
        stream_response(req, res);
        return false;
    }

    const size_t DELETE_BATCH_SIZE = std::stoi(req->params[BATCH_SIZE]);

    if(DELETE_BATCH_SIZE == 0) {
        req->last_chunk_aggregate = true;
        res->final = true;
        res->set_400("Parameter `" + std::string(BATCH_SIZE) + "` must be a positive integer.");
        stream_response(req, res);
        return false;
    }

    std::string simple_filter_query;

    if(req->params.count(FILTER_BY) != 0) {
        simple_filter_query = req->params[FILTER_BY];
    }

    deletion_state_t* deletion_state = nullptr;

    if(req->data == nullptr) {
        deletion_state = new deletion_state_t{};
        auto filter_ids_op = collection->get_filter_ids(simple_filter_query, deletion_state->index_ids);

        if(!filter_ids_op.ok()) {
            res->set(filter_ids_op.code(), filter_ids_op.error());
            req->last_chunk_aggregate = true;
            res->final = true;
            stream_response(req, res);
            delete deletion_state;
            return false;
        }

        for(size_t i=0; i<deletion_state->index_ids.size(); i++) {
            deletion_state->offsets.push_back(0);
        }
        deletion_state->collection = collection.get();
        deletion_state->num_removed = 0;
        req->data = deletion_state;
    } else {
        deletion_state = static_cast<deletion_state_t*>(req->data);
    }

    bool done = true;
    Option<bool> remove_op = stateful_remove_docs(deletion_state, DELETE_BATCH_SIZE, done);

    if(!remove_op.ok()) {
        res->set(remove_op.code(), remove_op.error());
        req->last_chunk_aggregate = true;
        res->final = true;
    } else {
        if(!done) {
            req->last_chunk_aggregate = false;
            res->final = false;
        } else {
            nlohmann::json response;
            response["num_deleted"] = deletion_state->num_removed;

            req->last_chunk_aggregate = true;
            req->data = nullptr;
            res->body = response.dump();
            res->final = true;
            delete deletion_state;
        }
    }

    if(res->final) {
        stream_response(req, res);
    } else {
        defer_processing(req, res, 0);
    }

    return true;
}

bool get_aliases(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    const spp::sparse_hash_map<std::string, std::string> & symlinks = collectionManager.get_symlinks();
    nlohmann::json res_json = nlohmann::json::object();
    res_json["aliases"] = nlohmann::json::array();

    for(const auto & symlink_collection: symlinks) {
        nlohmann::json symlink;
        symlink["name"] = symlink_collection.first;
        symlink["collection_name"] = symlink_collection.second;
        res_json["aliases"].push_back(symlink);
    }

    res->set_200(res_json.dump());
    return true;
}

bool get_alias(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const std::string & alias = req->params["alias"];
    CollectionManager & collectionManager = CollectionManager::get_instance();
    Option<std::string> collection_name_op = collectionManager.resolve_symlink(alias);

    if(!collection_name_op.ok()) {
        res->set_404();
        return false;
    }

    nlohmann::json res_json;
    res_json["name"] = alias;
    res_json["collection_name"] = collection_name_op.get();

    res->set_200(res_json.dump());
    return true;
}

bool put_upsert_alias(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json req_json;

    try {
        req_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }

    CollectionManager & collectionManager = CollectionManager::get_instance();
    const std::string & alias = req->params["alias"];

    const char* COLLECTION_NAME = "collection_name";

    if(req_json.count(COLLECTION_NAME) == 0) {
        res->set_400(std::string("Parameter `") + COLLECTION_NAME + "` is required.");
        return false;
    }

    Option<bool> success_op = collectionManager.upsert_symlink(alias, req_json[COLLECTION_NAME]);
    if(!success_op.ok()) {
        res->set_500(success_op.error());
        return false;
    }

    req_json["name"] = alias;
    res->set_200(req_json.dump());
    return true;
}

bool del_alias(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const std::string & alias = req->params["alias"];
    CollectionManager & collectionManager = CollectionManager::get_instance();

    Option<std::string> collection_name_op = collectionManager.resolve_symlink(alias);
    if(!collection_name_op.ok()) {
        res->set_404();
        return false;
    }

    Option<bool> delete_op = collectionManager.delete_symlink(alias);

    if(!delete_op.ok()) {
        res->set_500(delete_op.error());
        return false;
    }

    nlohmann::json res_json;
    res_json["name"] = alias;
    res_json["collection_name"] = collection_name_op.get();
    res->set_200(res_json.dump());
    return true;
}

bool get_overrides(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    nlohmann::json res_json;
    res_json["overrides"] = nlohmann::json::array();

    const std::map<std::string, override_t>& overrides = collection->get_overrides();
    for(const auto & kv: overrides) {
        nlohmann::json override = kv.second.to_json();
        res_json["overrides"].push_back(override);
    }

    res->set_200(res_json.dump());
    return true;
}

bool get_override(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    std::string override_id = req->params["id"];

    const std::map<std::string, override_t>& overrides = collection->get_overrides();

    if(overrides.count(override_id) != 0) {
        nlohmann::json override = overrides.at(override_id).to_json();
        res->set_200(override.dump());
        return true;
    }

    res->set_404();
    return false;
}

bool put_override(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    std::string override_id = req->params["id"];

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    nlohmann::json req_json;

    try {
        req_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }
    
    override_t override;
    Option<bool> parse_op = override_t::parse(req_json, override_id, override);
    if(!parse_op.ok()) {
        res->set(parse_op.code(), parse_op.error());
        return false;
    }
    
    Option<uint32_t> add_op = collection->add_override(override);

    if(!add_op.ok()) {
        res->set(add_op.code(), add_op.error());
        return false;
    }

    req_json["id"] = override.id;

    res->set_200(req_json.dump());
    return true;
}

bool del_override(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    Option<uint32_t> rem_op = collection->remove_override(req->params["id"]);
    if(!rem_op.ok()) {
        res->set(rem_op.code(), rem_op.error());
        return false;
    }

    nlohmann::json res_json;
    res_json["id"] = req->params["id"];

    res->set_200(res_json.dump());
    return true;
}

bool get_keys(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    AuthManager &auth_manager = collectionManager.getAuthManager();

    const Option<std::vector<api_key_t>>& keys_op = auth_manager.list_keys();
    if(!keys_op.ok()) {
        res->set(keys_op.code(), keys_op.error());
        return false;
    }

    nlohmann::json res_json;
    res_json["keys"] = nlohmann::json::array();

    const std::vector<api_key_t>& keys = keys_op.get();
    for(const auto & key: keys) {
        nlohmann::json key_obj = key.to_json();
        key_obj["value_prefix"] = key_obj["value"];
        key_obj.erase("value");
        res_json["keys"].push_back(key_obj);
    }

    res->set_200(res_json.dump());
    return true;
}

bool post_create_key(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    //LOG(INFO) << "post_create_key";

    CollectionManager & collectionManager = CollectionManager::get_instance();
    AuthManager &auth_manager = collectionManager.getAuthManager();

    nlohmann::json req_json;

    try {
        req_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }


    const Option<uint32_t>& validate_op = api_key_t::validate(req_json);
    if(!validate_op.ok()) {
        res->set(validate_op.code(), validate_op.error());
        return false;
    }

    if(req_json.count("expires_at") == 0) {
        req_json["expires_at"] = api_key_t::FAR_FUTURE_TIMESTAMP;
    }

    const std::string &rand_key = (req_json.count("value") != 0) ?
            req_json["value"].get<std::string>() : req->metadata;

    api_key_t api_key(
        rand_key,
        req_json["description"].get<std::string>(),
        req_json["actions"].get<std::vector<std::string>>(),
        req_json["collections"].get<std::vector<std::string>>(),
        req_json["expires_at"].get<uint64_t>()
    );

    const Option<api_key_t>& api_key_op = auth_manager.create_key(api_key);
    if(!api_key_op.ok()) {
        res->set(api_key_op.code(), api_key_op.error());
        return false;
    }

    res->set_201(api_key_op.get().to_json().dump());
    return true;
}

bool get_key(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    AuthManager &auth_manager = collectionManager.getAuthManager();

    const std::string& key_id_str = req->params["id"];
    uint32_t key_id = (uint32_t) std::stoul(key_id_str);

    const Option<api_key_t>& key_op = auth_manager.get_key(key_id);

    if(!key_op.ok()) {
        res->set(key_op.code(), key_op.error());
        return false;
    }

    nlohmann::json key_obj = key_op.get().to_json();
    key_obj["value_prefix"] = key_obj["value"];
    key_obj.erase("value");

    res->set_200(key_obj.dump());
    return true;
}

bool del_key(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    AuthManager &auth_manager = collectionManager.getAuthManager();

    const std::string& key_id_str = req->params["id"];
    uint32_t key_id = (uint32_t) std::stoul(key_id_str);

    const Option<api_key_t> &del_op = auth_manager.remove_key(key_id);

    if(!del_op.ok()) {
        res->set(del_op.code(), del_op.error());
        return false;
    }

    nlohmann::json res_json;
    res_json["id"] = del_op.get().id;

    res->set_200(res_json.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore));
    return true;
}

bool post_snapshot(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    const std::string SNAPSHOT_PATH = "snapshot_path";

    res->status_code = 201;
    res->content_type_header = "application/json";

    if(req->params.count(SNAPSHOT_PATH) == 0) {
        req->last_chunk_aggregate = true;
        res->final = true;
        res->set_400(std::string("Parameter `") + SNAPSHOT_PATH + "` is required.");
        stream_response(req, res);
        return false;
    }

    server->do_snapshot(req->params[SNAPSHOT_PATH], req, res);

    return true;
}

bool post_vote(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    res->status_code = 200;
    res->content_type_header = "application/json";

    nlohmann::json response;
    response["success"] = server->trigger_vote();
    res->body = response.dump();

    return true;
}

bool post_config(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    nlohmann::json req_json;

    try {
        req_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }

    bool found_config = false;

    if(req_json.count("log-slow-requests-time-ms") != 0) {
        if(!req_json["log-slow-requests-time-ms"].is_number_integer()) {
            res->set_400("Configuration `log-slow-requests-time-ms` must be an integer.");
            return false;
        }

        Config::get_instance().set_log_slow_requests_time_ms(req_json["log-slow-requests-time-ms"].get<int>());
        found_config = true;
    }

    if(req_json.count("healthy-read-lag") != 0) {
        if(!req_json["healthy-read-lag"].is_number_integer()) {
            res->set_400("Configuration `healthy-read-lag` must be a positive integer.");
            return false;
        }

        size_t read_lag = req_json["healthy-read-lag"].get<int>();
        if(read_lag <= 0) {
            res->set_400("Configuration `healthy-read-lag` must be a positive integer.");
            return false;
        }

        Config::get_instance().set_healthy_read_lag(read_lag);
        found_config = true;
    }

    if(req_json.count("healthy-write-lag") != 0) {
        if(!req_json["healthy-write-lag"].is_number_integer()) {
            res->set_400("Configuration `healthy-write-lag` must be an integer.");
            return false;
        }

        size_t write_lag = req_json["healthy-write-lag"].get<int>();
        if(write_lag <= 0) {
            res->set_400("Configuration `healthy-write-lag` must be a positive integer.");
            return false;
        }

        Config::get_instance().set_healthy_write_lag(req_json["healthy-write-lag"].get<int>());
        found_config = true;
    }

    if(!found_config) {
        res->set_400("Invalid configuration.");
    } else {
        nlohmann::json response;
        response["success"] = true;
        res->set_201(response.dump());
    }

    return true;
}

bool post_clear_cache(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    {
        std::unique_lock lock(mutex);
        res_cache.clear();
    }

    nlohmann::json response;
    response["success"] = true;
    res->set_200(response.dump());

    return true;
}

bool get_synonyms(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    nlohmann::json res_json;
    res_json["synonyms"] = nlohmann::json::array();

    const auto& synonyms = collection->get_synonyms();
    for(const auto & kv: synonyms) {
        nlohmann::json synonym = kv.second.to_view_json();
        res_json["synonyms"].push_back(synonym);
    }

    res->set_200(res_json.dump());
    return true;
}

bool get_synonym(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    std::string synonym_id = req->params["id"];

    synonym_t synonym;
    bool found = collection->get_synonym(synonym_id, synonym);

    if(found) {
        nlohmann::json synonym_json = synonym.to_view_json();
        res->set_200(synonym_json.dump());
        return true;
    }

    res->set_404();
    return false;
}

bool put_synonym(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    std::string synonym_id = req->params["id"];

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    nlohmann::json syn_json;

    try {
        syn_json = nlohmann::json::parse(req->body);
    } catch(const std::exception& e) {
        LOG(ERROR) << "JSON error: " << e.what();
        res->set_400("Bad JSON.");
        return false;
    }

    syn_json["id"] = synonym_id;

    synonym_t synonym;
    Option<bool> syn_op = synonym_t::parse(syn_json, synonym);

    if(!syn_op.ok()) {
        res->set(syn_op.code(), syn_op.error());
        return false;
    }

    Option<bool> upsert_op = collection->add_synonym(synonym);

    if(!upsert_op.ok()) {
        res->set(upsert_op.code(), upsert_op.error());
        return false;
    }

    res->set_200(syn_json.dump());
    return true;
}

bool del_synonym(const std::shared_ptr<http_req>& req, const std::shared_ptr<http_res>& res) {
    CollectionManager & collectionManager = CollectionManager::get_instance();
    auto collection = collectionManager.get_collection(req->params["collection"]);

    if(collection == nullptr) {
        res->set_404();
        return false;
    }

    Option<bool> rem_op = collection->remove_synonym(req->params["id"]);
    if(!rem_op.ok()) {
        res->set(rem_op.code(), rem_op.error());
        return false;
    }

    nlohmann::json res_json;
    res_json["id"] = req->params["id"];

    res->set_200(res_json.dump());
    return true;
}

bool is_doc_import_route(uint64_t route_hash) {
    route_path* rpath;
    bool found = server->get_route(route_hash, &rpath);
    return found && (rpath->handler == post_import_documents);
}

bool is_doc_write_route(uint64_t route_hash) {
    route_path* rpath;
    bool found = server->get_route(route_hash, &rpath);
    return found && (rpath->handler == post_add_document || rpath->handler == patch_update_document);
}

bool is_doc_del_route(uint64_t route_hash) {
    route_path* rpath;
    bool found = server->get_route(route_hash, &rpath);
    return found && (rpath->handler == del_remove_document || rpath->handler == del_remove_documents);
}
