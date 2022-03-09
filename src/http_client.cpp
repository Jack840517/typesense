#include "http_client.h"
#include "file_utils.h"
#include "logger.h"
#include <vector>
#include <json.hpp>

std::string HttpClient::api_key = "";
std::string HttpClient::ca_cert_path = "";

long HttpClient::post_response(const std::string &url, const std::string &body, std::string &response,
                               std::map<std::string, std::string>& res_headers, long timeout_ms) {
    CURL *curl = init_curl(url, response);
    if(curl == nullptr) {
        return 500;
    }

    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    return perform_curl(curl, res_headers);
}

long HttpClient::post_response_async(const std::string &url, const std::shared_ptr<http_req> request,
                                     const std::shared_ptr<http_res> response, HttpServer* server) {
    deferred_req_res_t* req_res = new deferred_req_res_t(request, response, server, false);
    std::unique_ptr<deferred_req_res_t> req_res_guard(req_res);
    struct curl_slist* chunk = nullptr;

    CURL *curl = init_curl_async(url, req_res, chunk);
    if(curl == nullptr) {
        return 500;
    }

    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_perform(curl);
    curl_easy_cleanup(curl);

    curl_slist_free_all(chunk);

    return 0;
}

long HttpClient::put_response(const std::string &url, const std::string &body, std::string &response,
                              std::map<std::string, std::string>& res_headers, long timeout_ms) {
    CURL *curl = init_curl(url, response);
    if(curl == nullptr) {
        return 500;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    return perform_curl(curl, res_headers);
}

long HttpClient::patch_response(const std::string &url, const std::string &body, std::string &response,
                              std::map<std::string, std::string>& res_headers, long timeout_ms) {
    CURL *curl = init_curl(url, response);
    if(curl == nullptr) {
        return 500;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    return perform_curl(curl, res_headers);
}

long HttpClient::delete_response(const std::string &url, std::string &response,
                                 std::map<std::string, std::string>& res_headers, long timeout_ms) {
    CURL *curl = init_curl(url, response);
    if(curl == nullptr) {
        return 500;
    }

    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
    return perform_curl(curl, res_headers);
}

long HttpClient::get_response(const std::string &url, std::string &response,
                              std::map<std::string, std::string>& res_headers, long timeout_ms) {
    CURL *curl = init_curl(url, response);
    if(curl == nullptr) {
        return 500;
    }

    return perform_curl(curl, res_headers);
}

void HttpClient::init(const std::string &api_key) {
    HttpClient::api_key = api_key;

    // try to locate ca cert file (from: https://serverfault.com/a/722646/117601)
    std::vector<std::string> locations = {
        "/etc/ssl/certs/ca-certificates.crt",                // Debian/Ubuntu/Gentoo etc.
        "/etc/pki/tls/certs/ca-bundle.crt",                  // Fedora/RHEL 6
        "/etc/ssl/ca-bundle.pem",                            // OpenSUSE
        "/etc/pki/tls/cacert.pem",                           // OpenELEC
        "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem", // CentOS/RHEL 7
        "/usr/local/etc/openssl/cert.pem",                   // OSX
        "/usr/local/etc/openssl@1.1/cert.pem",               // OSX
    };

    HttpClient::ca_cert_path = "";

    for(const std::string & location: locations) {
        if(file_exists(location)) {
            HttpClient::ca_cert_path = location;
            break;
        }
    }
}

long HttpClient::perform_curl(CURL *curl, std::map<std::string, std::string>& res_headers) {
    struct curl_slist *chunk = nullptr;
    std::string api_key_header = std::string("x-typesense-api-key: ") + HttpClient::api_key;
    chunk = curl_slist_append(chunk, api_key_header.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        char* url = nullptr;
        curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &url);
        LOG(ERROR) << "CURL failed. URL: " << url << ", Code: " << res << ", strerror: " << curl_easy_strerror(res);
        curl_easy_cleanup(curl);
        curl_slist_free_all(chunk);
        return 500;
    }

    long http_code = 500;
    curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);

    extract_response_headers(curl, res_headers);

    curl_easy_cleanup(curl);
    curl_slist_free_all(chunk);

    return http_code == 0 ? 500 : http_code;
}

void HttpClient::extract_response_headers(CURL* curl, std::map<std::string, std::string> &res_headers) {
    char* content_type;
    CURLcode res = curl_easy_getinfo (curl, CURLINFO_CONTENT_TYPE, &content_type);
    if(res == CURLE_OK && content_type != nullptr) {
        res_headers.emplace("content-type", content_type);
    }
}

size_t HttpClient::curl_req_send_callback(char* buffer, size_t size, size_t nitems, void* userdata) {
    //LOG(INFO) << "curl_req_send_callback";
    // callback for request body to be sent to remote host
    deferred_req_res_t* req_res = static_cast<deferred_req_res_t *>(userdata);

    if(!req_res->res->is_alive) {
        // underlying client request is dead, don't proxy anymore data to upstream (leader)
        //LOG(INFO) << "req_res->req->req is: null";
        return 0;
    }

    size_t max_req_bytes = (size * nitems);

    const char* total_body_buf = req_res->req->body.c_str();
    size_t available_body_bytes = (req_res->req->body.size() - req_res->req->body_index);

    // copy data into `buffer` not exceeding max_req_bytes
    size_t bytes_to_read = std::min(max_req_bytes, available_body_bytes);

    memcpy(buffer, total_body_buf + req_res->req->body_index, bytes_to_read);

    req_res->req->body_index += bytes_to_read;

    /*LOG(INFO) << "Wrote " << bytes_to_read << " bytes to request body (max_buffer_bytes=" << max_req_bytes << ")";
    LOG(INFO) << "req_res->req->body_index: " << req_res->req->body_index
              << ", req_res->req->body.size(): " << req_res->req->body.size();*/

    if(req_res->req->body_index == req_res->req->body.size()) {
        //LOG(INFO) << "Current body buffer has been consumed fully.";

        req_res->req->body_index = 0;
        req_res->req->body = "";

        HttpServer *server = req_res->server;

        server->get_message_dispatcher()->send_message(HttpServer::REQUEST_PROCEED_MESSAGE, req_res);

        if(!req_res->req->last_chunk_aggregate) {
            //LOG(INFO) << "Waiting for request body to be ready";
            req_res->req->wait();
            //LOG(INFO) << "Request body is ready";
            //LOG(INFO) << "Buffer refilled, unpausing request forwarding, body_size=" << req_res->req->body.size();
        }
    }

    return bytes_to_read;
}

size_t HttpClient::curl_write_async(char *buffer, size_t size, size_t nmemb, void *context) {
    // callback for response body to be sent back to client
    //LOG(INFO) << "curl_write_async";
    deferred_req_res_t* req_res = static_cast<deferred_req_res_t *>(context);

    if(!req_res->res->is_alive) {
        // underlying client request is dead, don't try to send anymore data
        return 0;
    }

    size_t res_size = size * nmemb;

    // set headers if not already set
    if(req_res->res->status_code == 0) {
        CURL* curl = req_res->req->data;
        long http_code = 500;
        CURLcode res = curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &http_code);
        if(res == CURLE_OK) {
            req_res->res->status_code = http_code;
        }

        char* content_type;
        res = curl_easy_getinfo (curl, CURLINFO_CONTENT_TYPE, &content_type);
        if(res == CURLE_OK && content_type != nullptr) {
            req_res->res->content_type_header = content_type;
        }
    }

    // we've got response from remote host: write to client and ask for more request body

    req_res->res->body = std::string(buffer, res_size);
    req_res->res->final = false;

    //LOG(INFO) << "curl_write_async response, res body size: " << req_res->res->body.size();

    async_req_res_t* async_req_res = new async_req_res_t(req_res->req, req_res->res, true);
    req_res->server->get_message_dispatcher()->send_message(HttpServer::STREAM_RESPONSE_MESSAGE, async_req_res);

    // wait until response is sent
    //LOG(INFO) << "Waiting on req_res " << req_res->res;
    req_res->res->wait();
    //LOG(INFO) << "Response sent";

    return res_size;
}

size_t HttpClient::curl_write_async_done(void *context, curl_socket_t item) {
    //LOG(INFO) << "curl_write_async_done";
    deferred_req_res_t* req_res = static_cast<deferred_req_res_t *>(context);

    if(!req_res->res->is_alive) {
        // underlying client request is dead, don't try to send anymore data
        return 0;
    }

    req_res->res->body = "";
    req_res->res->final = true;

    async_req_res_t* async_req_res = new async_req_res_t(req_res->req, req_res->res, true);
    req_res->server->get_message_dispatcher()->send_message(HttpServer::STREAM_RESPONSE_MESSAGE, async_req_res);

    // wait until final response is flushed or response object will be destroyed by caller
    req_res->res->wait();

    // Close the socket as we've overridden the close socket handler!
    close(item);

    return 0;
}

CURL *HttpClient::init_curl_async(const std::string& url, deferred_req_res_t* req_res, curl_slist*& chunk) {
    CURL *curl = curl_easy_init();

    if(curl == nullptr) {
        return nullptr;
    }

    req_res->req->data = curl;

    std::string api_key_header = std::string("x-typesense-api-key: ") + HttpClient::api_key;
    chunk = curl_slist_append(chunk, api_key_header.c_str());

    // set content length
    std::string content_length_header = std::string("content-length: ") + std::to_string(req_res->req->_req->content_length);
    chunk = curl_slist_append(chunk, content_length_header.c_str());

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

    // Enabling this causes issues in mixed mode: client using http/1 but follower -> leader using http/2
    //curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);

    // callback called every time request body is needed
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, HttpClient::curl_req_send_callback);

    // context to callback
    curl_easy_setopt(curl, CURLOPT_READDATA, (void *)req_res);

    if(!ca_cert_path.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_cert_path.c_str());
    } else {
        LOG(WARNING) << "Unable to locate system SSL certificates.";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000);

    // to allow self-signed certs
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HttpClient::curl_write_async);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, req_res);

    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETFUNCTION, HttpClient::curl_write_async_done);
    curl_easy_setopt(curl, CURLOPT_CLOSESOCKETDATA, req_res);

    return curl;
}

CURL *HttpClient::init_curl(const std::string& url, std::string& response) {
    CURL *curl = curl_easy_init();

    if(curl == nullptr) {
        nlohmann::json res;
        res["message"] = "Failed to initialize HTTP client.";
        response = res.dump();
        return nullptr;
    }

    if(!ca_cert_path.empty()) {
        curl_easy_setopt(curl, CURLOPT_CAINFO, ca_cert_path.c_str());
    } else {
        LOG(WARNING) << "Unable to locate system SSL certificates.";
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 4000);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2_PRIOR_KNOWLEDGE);

    // to allow self-signed certs
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, HttpClient::curl_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);

    return curl;
}

size_t HttpClient::curl_write(char *contents, size_t size, size_t nmemb, std::string *s) {
    s->append(contents, size*nmemb);
    return size*nmemb;
}
