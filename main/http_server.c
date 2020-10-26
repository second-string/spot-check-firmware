#include <sys/param.h>

#include "freertos/FreeRTOS.h"
#include "esp_log.h"

#include "constants.h"
#include "http_server.h"
#include "url_decode.h"
#include "wifi.h"

static esp_err_t setup_page_get_handler(httpd_req_t *req);
static esp_err_t save_config_post_handler(httpd_req_t *req);

static const httpd_uri_t setup_page_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = setup_page_get_handler,
    .user_ctx  = NULL
};

static const httpd_uri_t save_config_uri = {
    .uri       = "/save_config",
    .method    = HTTP_POST,
    .handler   = save_config_post_handler,
    .user_ctx  = NULL
};

// Must be global/static because need to live for the full life of the spawned task
static connect_to_network_task_args connection_args;

static esp_err_t setup_page_get_handler(httpd_req_t *req) {
    const char *html = "<!DOCTYPE html>\
\
<html lang=\"en\">\
<head>\
  <meta charset=\"utf-8\">\
\
  <title>Spot Check Configuration</title>\
<!--   <meta name=\"description\" content=\"The HTML5 Herald\">\
  <meta name=\"author\" content=\"SitePoint\"> -->\
\
  <!-- <link rel=\"stylesheet\" href=\"css/styles.css?v=1.0\"> -->\
\
</head>\
\
<body>\
    <h2>Shalom</h2>\
    <p>Enter network information for Spot Check to connect</p>\
    <form action=\"/save_config\" method=\"POST\">\
        <label for=\"ssid\">SSID</label>\
        <input type=\"text\" id=\"ssid\" name=\"ssid\">\
        <label for=\"password\">Password</label>\
        <input type=\"password\" id=\"password\" name=\"password\">\
        <input type=\"submit\" value=\"Save\">\
    </form>\
</body>\
</html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_config_post_handler(httpd_req_t *req) {
    int buf_len = 100;
    char buf[buf_len];
    int received_bytes;
    int remaining_bytes = req->content_len;

    // Only proceed if we got a form-urlencoded response, otherwise bail with a 415
    received_bytes = httpd_req_get_hdr_value_len(req, "Content-Type") + 1;
    if (received_bytes > 1) {
        if (httpd_req_get_hdr_value_str(req, "Content-Type", buf, received_bytes) == ESP_OK) {
            if (strcmp(buf, "application/x-www-form-urlencoded")) {
                ESP_LOGE(TAG, "Unsupported content-type in save_config POST: %s", buf);
                // httpd_resp_set_status(req, "HTTP/1.1 415 Unsupported Media Type");
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unsupported Media Type: Expected application/x-www-form-urlencoded");
            }
            ESP_LOGI(TAG, "Found header => Content-Type: %s", buf);
        }
    }

    // Reset buf to all zeroes because reading the headers can persist some of that data deeper
    // in the buffer if the POST data isn't as long as the headers
    memset(buf, 0, buf_len);

    // Chunk in POST data payload
    while (remaining_bytes > 0) {
        received_bytes = httpd_req_recv(req, buf, MIN(remaining_bytes, sizeof(buf)));
        if (received_bytes <= 0) {
            if (received_bytes == HTTPD_SOCK_ERR_TIMEOUT) {
                /* Retry receiving if timeout occurred */
                continue;
            }
            return ESP_FAIL;
        }

        /* Send back the same data */
        // httpd_resp_send_chunk(req, buf, received_bytes);
        remaining_bytes -= received_bytes;

        /* Log data received */
        ESP_LOGI(TAG, "=========== RECEIVED DATA ==========");
        ESP_LOGI(TAG, "%.*s", received_bytes, buf);
        ESP_LOGI(TAG, "====================================");
    }

    // TODO :: Making big assumption that all data fits in a single buffer frame, otherwise
    // the multiple calls to "httpd_req_recv" above will overwrite
    char decoded_buf[100] = { 0 };
    urldecode2(decoded_buf, buf);
    ESP_LOGI(TAG, "Decoded into buf: '%s'", decoded_buf);

    Tuple key_value_pairs[MAX_KEY_VALUE_QUERYSTRING_AGS] = { { { 0 }, { 0 } } } ;
    ESP_ERROR_CHECK(get_key_values(decoded_buf, key_value_pairs, MAX_KEY_VALUE_QUERYSTRING_AGS));

    char *ssid = NULL;
    char *pw = NULL;
    int i;
    for (i = 0; i < MAX_KEY_VALUE_QUERYSTRING_AGS; i++) {
        if (strcmp(key_value_pairs[i].key, "ssid") == 0) {
            ssid = key_value_pairs[i].value;
        } else if (strcmp(key_value_pairs[i].key, "password") == 0) {
            pw = key_value_pairs[i].value;
        }
    }

    if (ssid == NULL || pw == NULL) {
        ESP_LOGE(TAG, "SSID or PW missing from query params: %s - %s", ssid, pw);
        return ESP_ERR_NOT_FOUND;
    }

    // TODO :: get this out of the response handler. Either A) spawn a task to do this, or B) register a callback
    // before kicking this off asyncly somehow, and then in the callback handle the internal state of saying "we're connected now"
    ESP_LOGI(TAG, "Attempting to connect to network with ssid '%s' and pw '%s'", ssid, pw);
    connection_args = (connect_to_network_task_args){
        .ssid = ssid,
        .password = pw,
        .ssid_len = strlen(ssid),
        .password_len = strlen(pw)
    };
    // xTaskCreate(connect_to_network, "CNCT_TO_NTWRK", 4096, &connection_args, tskIDLE_PRIORITY, connect_to_network_task);
    // connect_to_network(&connection_args);

    // End response
    ESP_ERROR_CHECK(httpd_resp_send(req, "Successfully configured.", HTTPD_RESP_USE_STRLEN));

    return ESP_OK;
}

httpd_handle_t http_server_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();

    // Start the httpd server
    ESP_LOGI(TAG, "Starting server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        // Set URI handlers
        ESP_LOGI(TAG, "Registering URI handlers");
        httpd_register_uri_handler(server, &setup_page_uri);
        httpd_register_uri_handler(server, &save_config_uri);
        return server;
    }

    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}
