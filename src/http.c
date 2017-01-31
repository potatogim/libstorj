#include <nettle/sha.h>
#include <nettle/ripemd160.h>

#include "http.h"

// TODO error check the calloc and realloc calls

static size_t body_shard_send(void *contents, size_t size, size_t nmemb, void *userp)
{
    shard_body_t *body = userp;

    if (*body->canceled) {
        uint64_t remain = body->remain;
        body->remain = 0;
        return remain;
    }

    if (buflen == 0) {
        body->remain = body->length;
        body->pnt = body->shard_data;
    } else {
        if (body->remain < buflen) {
            buflen = body->remain;
        }
        memcpy(buffer, body->pnt, buflen);

        body->pnt += buflen;
        body->total_sent += buflen;
        body->bytes_since_progress += buflen;

        body->remain -= buflen;
    }

    // give progress updates at set interval
    if (body->progress_handle && buflen &&
        (body->bytes_since_progress > SHARD_PROGRESS_INTERVAL ||
         body->remain == 0)) {

        shard_upload_progress_t *progress = body->progress_handle->data;
        progress->bytes = body->total_sent;
        uv_async_send(body->progress_handle);

        body->bytes_since_progress = 0;
    }

    return buflen;
}

int put_shard(storj_http_options_t *http_options,
              char *farmer_id,
              char *proto,
              char *host,
              int port,
              char *shard_hash,
              uint64_t shard_total_bytes,
              uint8_t *shard_data,
              char *token,
              int *status_code,
              uv_async_t *progress_handle,
              bool *canceled)
{

    CURL *curl = curl_easy_init();
    if (!curl) {
        return 1;
    }

    char query_args[80];
    snprintf(query_args, 80, "?token=%s", token);

    int url_len = strlen(proto) + 3 + strlen(host) + 1 + 10 + 8
        + strlen(shard_hash) + strlen(query_args);
    char url = calloc(url_len + 1, sizeof(char));

    snprintf(url, "%s://%s:%i/shards/%s%s", proto, host, port,
             shard_hash, query_args);

    curl_easy_setopt(curl, CURLOPT_URL, url);

    if (http_options->user_agent) {
        curl_easy_setopt(curl, CURLOPT_USERAGENT, http_options->user_agent);
    }

    int proxy_len = strlen(http_options->proxy_version) + 3 +
        stlen(http_options->proxy_host) + 1 + 10;

    char *proxy = calloc(proxy_len + 1, sizeof(char));
    snprintf(proxy, proxy_len, "%s://");

    if (http_options->proxy_url) {
        curl_easy_setopt(curl, CURLOPT_PROXY, http_options->proxy_url);
    }

    //curl_easy_setopt(curl, CURLOPT_POST, 1);

    struct curl_slist *content_chunk = NULL;
    content_chunk = curl_slist_append(content_chunk, "Content-Type: application/octet-stream");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, content_chunk);

    struct curl_slist *node_chunk = NULL;
    char *header = calloc(17 + 40 + 1, sizeof(char));
    strcat(header, "x-storj-node-id: ");
    strncat(header, farmer_id, 40);
    chunk = curl_slist_append(node_chunk, header);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);

    shard_body_t *shard_body = NULL;

    if (shard_data && shard_total_bytes) {

        shard_body = malloc(sizeof(shard_body_t));
        shard_body->shard_data = shard_data;
        shard_body->length = shard_total_bytes;
        shard_body->remain = shard_total_bytes;
        shard_body->pnt = shard_data;
        shard_body->total_sent = 0;
        shard_body->bytes_since_progress = 0;
        shard_body->progress_handle = progress_handle;
        shard_body->canceled = canceled;

        curl_easy_setopt(curl, CURLOPT_READFUNCTION, body_shard_send);
        curl_easy_setopt(curl, CURLOPT_READDATA, (void *)shard_body);
    }

#ifdef _WIN32
    signal(WSAECONNRESET, SIG_IGN);
#else
    signal(SIGPIPE, SIG_IGN);
#endif

    int req = curl_easy_perform(curl);

    if (*canceled) {
        goto clean_up;
    }

    if (req != CURLE_OK) {
        // TODO log using logger
        printf("Put shard request error: %s\n", curl_easy_strerror(req));
        return req;
    }

    // set the status code
    *status_code = ne_get_status(req)->code;

clean_up:

    // clean up memory
    if (shard_body) {
        free(shard_body);
    }
    free(path);
    curl_easy_cleanup(cleanup);

    return 0;
}

/* shard_data must be allocated for shard_total_bytes */
int fetch_shard(storj_http_options_t *http_options,
                char *farmer_id,
                char *proto,
                char *host,
                int port,
                char *shard_hash,
                uint64_t shard_total_bytes,
                char *shard_data,
                char *token,
                int *status_code,
                uv_async_t *progress_handle,
                bool *canceled)
{
    struct sha256_ctx ctx;
    sha256_init(&ctx);

    ne_session *sess = ne_session_create(proto, host, port);

    if (http_options->user_agent) {
        ne_set_useragent(sess, http_options->user_agent);
    }

    if (http_options->proxy_version &&
        http_options->proxy_host &&
        http_options->proxy_port) {

        ne_session_socks_proxy(sess,
                               (enum ne_sock_sversion)http_options->proxy_version,
                               http_options->proxy_host,
                               http_options->proxy_port,
                               "",
                               "");
    }

    char query_args[80];
    ne_snprintf(query_args, 80, "?token=%s", token);

    char *path = ne_concat("/shards/", shard_hash, query_args, NULL);

    ne_request *req = ne_request_create(sess, "GET", path);

    ne_add_request_header(req, "x-storj-node-id", farmer_id);

    if (0 == strcmp(proto, "https")) {
        ne_ssl_trust_default_ca(sess);
    }

    if (ne_begin_request(req) != NE_OK) {
        clean_up_neon(sess, req);
        return STORJ_FARMER_REQUEST_ERROR;
    }

    *status_code = ne_get_status(req)->code;

    char *buf = calloc(NE_BUFSIZ, sizeof(char));

    ssize_t bytes = 0;
    ssize_t total = 0;

    ssize_t bytes_since_progress = 0;

    int error_code = 0;

    while ((bytes = ne_read_response_block(req, buf, NE_BUFSIZ)) > 0) {
        if (total + bytes > shard_total_bytes) {
            error_code = STORJ_FARMER_INTEGRITY_ERROR;
            break;
        }

        sha256_update(&ctx, bytes, (uint8_t *)buf);

        memcpy(shard_data + total, buf, bytes);
        total += bytes;

        bytes_since_progress += bytes;

        // give progress updates at set interval
        if (progress_handle && bytes_since_progress > SHARD_PROGRESS_INTERVAL) {
            shard_download_progress_t *progress = progress_handle->data;
            progress->bytes = total;
            uv_async_send(progress_handle);
            bytes_since_progress = 0;
        }

        if (*canceled) {
            error_code = STORJ_TRANSFER_CANCELED;
            break;
        }

    }

    ne_end_request(req);
    clean_up_neon(sess, req);
    free(buf);
    free(path);

    if (!error_code && total != shard_total_bytes) {
        error_code = STORJ_FARMER_INTEGRITY_ERROR;
    }

    if (error_code) {
        return error_code;
    }

    uint8_t *hash_sha256 = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
    sha256_digest(&ctx, SHA256_DIGEST_SIZE, hash_sha256);

    struct ripemd160_ctx rctx;
    ripemd160_init(&rctx);
    ripemd160_update(&rctx, SHA256_DIGEST_SIZE, hash_sha256);

    free(hash_sha256);

    uint8_t *hash_rmd160 = calloc(RIPEMD160_DIGEST_SIZE + 1, sizeof(uint8_t));
    ripemd160_digest(&rctx, RIPEMD160_DIGEST_SIZE, hash_rmd160);

    char *hash = calloc(RIPEMD160_DIGEST_SIZE * 2 + 1, sizeof(char));
    for (unsigned i = 0; i < RIPEMD160_DIGEST_SIZE; i++) {
        sprintf(&hash[i*2], "%02x", hash_rmd160[i]);
    }

    free(hash_rmd160);

    if (strcmp(shard_hash, hash) != 0) {
        error_code = STORJ_FARMER_INTEGRITY_ERROR;
    }

    free(hash);

    if (error_code) {
        return error_code;
    }

    // final progress update
    if (progress_handle) {
        shard_download_progress_t *progress = progress_handle->data;
        progress->bytes = total;
        uv_async_send(progress_handle);
    }

    return 0;
}

struct json_object *fetch_json(storj_http_options_t *http_options,
                               storj_bridge_options_t *options,
                               char *method,
                               char *path,
                               struct json_object *request_body,
                               bool auth,
                               char *token,
                               int *status_code)
{
    // TODO: reuse an existing session and socket to the bridge

    ne_session *sess = ne_session_create(options->proto, options->host,
                                         options->port);

    if (http_options->user_agent) {
        ne_set_useragent(sess, http_options->user_agent);
    }

    if (http_options->proxy_version &&
        http_options->proxy_host &&
        http_options->proxy_port) {

        ne_session_socks_proxy(sess,
                               (enum ne_sock_sversion)http_options->proxy_version,
                               http_options->proxy_host,
                               http_options->proxy_port,
                               "",
                               "");
    }

    // TODO: error check the ne calls in this function

    if (0 == strcmp(options->proto, "https")) {
        ne_ssl_trust_default_ca(sess);
    }

    ne_request *req = ne_request_create(sess, method, path);

    // include authentication headers if info is provided
    if (auth && options->user && options->pass) {
        // Hash password
        uint8_t *pass_hash = calloc(SHA256_DIGEST_SIZE, sizeof(uint8_t));
        char *pass = calloc(SHA256_DIGEST_SIZE * 2 + 1, sizeof(char));
        struct sha256_ctx ctx;
        sha256_init(&ctx);
        sha256_update(&ctx, strlen(options->pass), (uint8_t *)options->pass);
        sha256_digest(&ctx, SHA256_DIGEST_SIZE, pass_hash);
        for (unsigned i = 0; i < SHA256_DIGEST_SIZE; i++) {
            sprintf(&pass[i*2], "%02x", pass_hash[i]);
        }

        free(pass_hash);

        char *user_pass = ne_concat(options->user, ":", pass, NULL);

        free(pass);

        char *user_pass_64 = ne_base64((uint8_t *)user_pass, strlen(user_pass));

        free(user_pass);

        int auth_value_len = strlen(user_pass_64) + 6;
        char auth_value[auth_value_len + 1];
        strcpy(auth_value, "Basic ");
        strcat(auth_value, user_pass_64);

        free(user_pass_64);

        auth_value[auth_value_len] = '\0';

        ne_add_request_header(req, "Authorization", auth_value);
    }

    if (token) {
        ne_add_request_header(req, "X-Token", token);
    }

    // include body if request body json is provided
    if (request_body) {
        const char *req_buf = json_object_to_json_string(request_body);

        ne_add_request_header(req, "Content-Type", "application/json");
        ne_set_request_body_buffer(req, req_buf, strlen(req_buf));
    }

    int request_status = 0;
    if ((request_status = ne_begin_request(req)) != NE_OK) {
        // TODO check request status
        // TODO get details if NE_ERROR(1) with ne_get_error(sess)
        clean_up_neon(sess, req);
        return NULL;
    }

    // set the status code
    *status_code = ne_get_status(req)->code;

    int body_sz = NE_BUFSIZ * 4;
    char *body = calloc(NE_BUFSIZ * 4, sizeof(char));
    char *buf = calloc(NE_BUFSIZ, sizeof(char));

    ssize_t bytes = 0;
    ssize_t total = 0;

    while ((bytes = ne_read_response_block(req, buf, NE_BUFSIZ))) {
        if (bytes < 0) {
            // TODO: error. careful with cleanup
        }

        if (total + bytes + 1 > body_sz) {
            body_sz += bytes + 1;
            body = (char *) realloc(body, body_sz);
        }

        memcpy(body + total, buf, bytes);
        total += bytes;
    }

    clean_up_neon(sess, req);

    json_object *j = json_tokener_parse(body);
    // TODO: Error checking

    free(body);
    free(buf);

    return j;
}
