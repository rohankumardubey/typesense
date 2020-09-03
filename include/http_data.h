#pragma once

#include <cstdint>
#include <string>
#include <map>
#include <vector>
#include <future>
#include <chrono>
#include "json.hpp"
#include "string_utils.h"

#define H2O_USE_LIBUV 0
extern "C" {
    #include "h2o.h"
}

struct h2o_custom_timer_t {
    h2o_timer_t timer;
    void *data;

    h2o_custom_timer_t(): data(nullptr) {}

    explicit h2o_custom_timer_t(void *data): data(data) {

    }
};

struct http_res {
    uint32_t status_code;
    std::string content_type_header;
    std::string body;
    bool final;

    // fulfilled by an async response handler to pass control back for further writes
    std::promise<bool>* promise = nullptr;

    h2o_generator_t* generator = nullptr;

    // for async requests, automatically progresses request body on response proceed
    bool proceed_req_after_write = true;

    http_res(): status_code(0), content_type_header("application/json; charset=utf-8"), final(true) {

    }

    static const char* get_status_reason(uint32_t status_code) {
        switch(status_code) {
            case 200: return "OK";
            case 201: return "Created";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 405: return "Not Allowed";
            case 409: return "Conflict";
            case 422: return "Unprocessable Entity";
            case 500: return "Internal Server Error";
            default: return "";
        }
    }

    void set_200(const std::string & res_body) {
        status_code = 200;
        body = res_body;
    }

    void set_201(const std::string & res_body) {
        status_code = 201;
        body = res_body;
    }

    void set_400(const std::string & message) {
        status_code = 400;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_401(const std::string & message) {
        status_code = 400;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_403() {
        status_code = 403;
        body = "{\"message\": \"Forbidden\"}";
    }

    void set_404() {
        status_code = 404;
        body = "{\"message\": \"Not Found\"}";
    }

    void set_405(const std::string & message) {
        status_code = 405;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_409(const std::string & message) {
        status_code = 409;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_422(const std::string & message) {
        status_code = 422;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_500(const std::string & message) {
        status_code = 500;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_503(const std::string & message) {
        status_code = 503;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set(uint32_t code, const std::string & message) {
        status_code = code;
        body = "{\"message\": \"" + message + "\"}";
    }

    void set_body(uint32_t code, const std::string & message) {
        status_code = code;
        body = message;
    }
};

enum class ROUTE_CODES {
    NOT_FOUND = 1,
    ALREADY_HANDLED = 2,
};

struct http_req {
    h2o_req_t* _req;
    std::string http_method;
    uint64_t route_hash;
    std::map<std::string, std::string> params;

    bool first_chunk_aggregate;
    bool last_chunk_aggregate;
    size_t chunk_len;

    std::string body;
    size_t body_index;
    std::string metadata;

    void* data;

    // used during forwarding of requests from follower to leader
    std::promise<bool>* promise = nullptr;

    // for deffered processing of async handlers
    h2o_custom_timer_t defer_timer;

    http_req(): _req(nullptr), route_hash(1),
                first_chunk_aggregate(true), last_chunk_aggregate(false),
                chunk_len(0), body_index(0), data(nullptr), promise(nullptr) {

    }

    http_req(h2o_req_t* _req, const std::string & http_method, uint64_t route_hash,
            const std::map<std::string, std::string> & params, const std::string& body):
            _req(_req), http_method(http_method), route_hash(route_hash), params(params),
            first_chunk_aggregate(true), last_chunk_aggregate(false),
            chunk_len(0), body(body), body_index(0), data(nullptr), promise(nullptr) {

    }

    // NOTE: we don't ser/de all fields, only ones needed for write forwarding
    // Take care to check for existence of key to ensure backward compatibility during upgrade

    void deserialize(const std::string& serialized_content) {
        nlohmann::json content = nlohmann::json::parse(serialized_content);
        route_hash = content["route_hash"];
        body = content["body"];

        for (nlohmann::json::iterator it = content["params"].begin(); it != content["params"].end(); ++it) {
            params.emplace(it.key(), it.value());
        }

        metadata = content.count("metadata") != 0 ? content["metadata"] : "";
        first_chunk_aggregate = content.count("first_chunk_aggregate") != 0 ? content["first_chunk_aggregate"].get<bool>() : true;
        last_chunk_aggregate = content.count("last_chunk_aggregate") != 0 ? content["last_chunk_aggregate"].get<bool>() : false;
        _req = nullptr;
    }

    std::string serialize() const {
        nlohmann::json content;
        content["route_hash"] = route_hash;
        content["params"] = params;
        content["first_chunk_aggregate"] = first_chunk_aggregate;
        content["last_chunk_aggregate"] = last_chunk_aggregate;
        content["body"] = body;
        content["metadata"] = metadata;

        return content.dump(-1, ' ', false, nlohmann::detail::error_handler_t::ignore);
    }

    bool is_http_v1() {
        return (_req->version < 0x200);
    }
};

struct request_response {
    http_req* req;
    http_res* res;
};

struct route_path {
    std::string http_method;
    std::vector<std::string> path_parts;
    bool (*handler)(http_req &, http_res &);
    bool async_req;
    bool async_res;
    std::string action;

    route_path(const std::string &httpMethod, const std::vector<std::string> &pathParts,
               bool (*handler)(http_req &, http_res &), bool async_req, bool async_res) :
            http_method(httpMethod), path_parts(pathParts), handler(handler),
            async_req(async_req), async_res(async_res) {
        action = _get_action();
        if(async_req) {
            // once a request is async, response also needs to be async
            this->async_res = true;
        }
    }

    inline bool operator< (const route_path& rhs) const {
        return true;
    }

    uint64_t route_hash() {
        std::string path = StringUtils::join(path_parts, "/");
        std::string method_path = http_method + path;
        uint64_t hash = StringUtils::hash_wy(method_path.c_str(), method_path.size());
        return (hash > 100) ? hash : (hash + 100);  // [0-99] reserved for special codes
    }

    std::string _get_action() {
        // `resource:operation` forms an action
        // operations: create, get, list, delete, search, import, export

        std::string resource;
        std::string operation;

        size_t resource_index = 0;
        size_t identifier_index = 0;

        for(size_t i = 0; i < path_parts.size(); i++) {
            if(path_parts[i][0] == ':') {
                identifier_index = i;
            }
        }

        if(identifier_index == 0) {
            // means that no identifier found, so set the last part as resource
            resource_index = path_parts.size() - 1;
        } else if(identifier_index == (path_parts.size() - 1)) {
            // is already last position
            resource_index = identifier_index - 1;
        } else {
            resource_index = identifier_index + 1;
        }

        resource = path_parts[resource_index];

        if(resource_index != path_parts.size() - 1 && path_parts[resource_index+1][0] != ':') {
            // e.g. /collections/:collection/documents/search
            operation = path_parts[resource_index+1];
        } else {
            // e.g /collections or /collections/:collection/foo or /collections/:collection

            if(http_method == "GET") {
                // GET can be a `get` or `list`
                operation = (resource_index == path_parts.size()-1) ? "list" : "get";
            } else if(http_method == "POST") {
                operation = "create";
            } else if(http_method == "PUT") {
                operation = "upsert";
            } else if(http_method == "DELETE") {
                operation = "delete";
            } else {
                operation = "unknown";
            }
        }

        return resource + ":" + operation;
    }
};

struct h2o_custom_res_message_t {
    h2o_multithread_message_t super;
    std::map<std::string, bool (*)(void*)> *message_handlers;
    std::string type;
    void* data;
};

struct http_message_dispatcher {
    h2o_multithread_queue_t* message_queue;
    h2o_multithread_receiver_t* message_receiver;
    std::map<std::string, bool (*)(void*)> message_handlers;

    void init(h2o_loop_t *loop) {
        message_queue = h2o_multithread_create_queue(loop);
        message_receiver = new h2o_multithread_receiver_t();
        h2o_multithread_register_receiver(message_queue, message_receiver, on_message);
    }

    ~http_message_dispatcher() {
        // drain existing messages
        on_message(message_receiver, &message_receiver->_messages);

        h2o_multithread_unregister_receiver(message_queue, message_receiver);
        h2o_multithread_destroy_queue(message_queue);

        delete message_receiver;
    }

    static void on_message(h2o_multithread_receiver_t *receiver, h2o_linklist_t *messages) {
        while (!h2o_linklist_is_empty(messages)) {
            h2o_multithread_message_t *message = H2O_STRUCT_FROM_MEMBER(h2o_multithread_message_t, link, messages->next);
            h2o_custom_res_message_t *custom_message = reinterpret_cast<h2o_custom_res_message_t*>(message);

            if(custom_message->message_handlers->count(custom_message->type) != 0) {
                auto handler = custom_message->message_handlers->at(custom_message->type);
                (handler)(custom_message->data);
            }

            h2o_linklist_unlink(&message->link);
            delete custom_message;
        }
    }

    void send_message(const std::string & type, void* data) {
        h2o_custom_res_message_t* message = new h2o_custom_res_message_t{{{nullptr, nullptr}}, &message_handlers, type, data};
        h2o_multithread_send_message(message_receiver, &message->super);
    }

    void on(const std::string & message, bool (*handler)(void*)) {
        message_handlers.emplace(message, handler);
    }
};

struct AsyncIndexArg {
    http_req* req;
    http_res* res;
    std::promise<bool>* promise;
};
