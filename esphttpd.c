#include "esphttpd.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "mbedtls/base64.h"
#include "sha/sha_parallel_engine.h"
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "lwip/api.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/opt.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <arpa/inet.h>
#include <lwip/netdb.h>

#define WEBSERVER_PORT (80)
#define MAX_WS_CONNECTIONS (16)
#define WS_MASK_LEN (4)

#define SEND_BUFFER_SIZE 1024
#define RCV_BUFFER_SIZE 1024

static TaskHandle_t esphttpd_task_handle = NULL;
static http_route *http_routes = NULL;
static ws_route *ws_routes = NULL;

static const char ws_sec_key[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const char http_resp_main_header[] = "HTTP/1.1 %d %s\r\n";
static const char http_resp_header[] = "%s: %s\r\n";
static const char *TAG = "ESPHTTPD";

static SemaphoreHandle_t ws_connection_semaphore = NULL;
static ws_ctx *ws_connections[MAX_WS_CONNECTIONS] = {NULL};

static void webserver_serve(struct netconn *conn);
static void webserver_task();

void webserver_send_status(struct netconn *conn, int status_code,
                           const char *status_text) {
  char buf[128];
  err_t err;

  snprintf(buf, sizeof(buf), http_resp_main_header, status_code, status_text);

  err = netconn_write(conn, buf, strlen(buf), NETCONN_COPY);

  if (err != ERR_OK) {
    ESP_LOGE(TAG, "Failed to send status: %d", err);
  }
}

void webserver_send_header(struct netconn *conn, const char *key,
                           const char *value) {
  char buf[128];
  err_t err;

  snprintf(buf, sizeof(buf), http_resp_header, key, value);

  err = netconn_write(conn, buf, strlen(buf), NETCONN_COPY);
  if (err != ERR_OK) {
    ESP_LOGE(TAG, "Failed to send header: %d", err);
  }
}

void webserver_send_body(struct netconn *conn, const char *body,
                         unsigned int body_len) {
  err_t err;

  err = netconn_write(conn, "\r\n", 2, NETCONN_NOCOPY);
  if (err != ERR_OK) {
    ESP_LOGE(TAG, "Failed to send newline: %d", err);
    return;
  }

  unsigned int offset = 0;

  while (offset < body_len) {
    unsigned int len = 512;
    if (offset + len > body_len)
      len = body_len - offset;

    err = netconn_write(conn, body + offset, len, NETCONN_NOCOPY);
    if (err != ERR_OK) {
      ESP_LOGE(TAG, "Error occurred during sending: %d", err);
      break;
    }

    offset += len;
  }
}

bool starts_with(const char *str, const char *prefix) {
  return strncmp(str, prefix, strlen(prefix)) == 0;
}

void webserver_send_body_template(http_req *req, char *body,
                                  unsigned int body_len,
                                  const char *start_delim,
                                  const char *end_delim,
                                  variable_callback var_cb) {
  if (!start_delim || !end_delim || !body || !var_cb) {
    ESP_LOGE(TAG, "Null pointer argument");
    return;
  }

  ESP_LOGI(TAG, "start delim: %s, end delim: %s", start_delim, end_delim);

  netconn_write(req->conn, "\r\n", 2, NETCONN_COPY);

  int offset = 0;
  int start_delim_len = strlen(start_delim);
  int end_delim_len = strlen(end_delim);
  bool delimiters_same = strcmp(start_delim, end_delim) == 0;

  while (offset < body_len) {
    bool var_found = false;
    int var_start = 0;
    int var_end = 0;

    for (int i = offset; i < body_len;) {
      if (!var_found && starts_with(body + i, start_delim)) {
        var_start = i;
        var_found = true;
        i += start_delim_len;
      } else if (var_found && starts_with(body + i, end_delim)) {
        var_end = i;
        i += end_delim_len;
        break;
      } else if (delimiters_same && starts_with(body + i, start_delim)) {
        i += start_delim_len;
        break;
      } else {
        i++;
      }
    }

    if (var_found && var_end > var_start) {
      int var_name_len = var_end - var_start - start_delim_len;
      char var_name[var_name_len + 1];
      strncpy(var_name, body + var_start + start_delim_len, var_name_len);
      var_name[var_name_len] = '\0';

      char *replacement = var_cb(var_name);

      if (replacement != NULL) {
        int prefix_len = var_start - offset;
        netconn_write(req->conn, body + offset, prefix_len, NETCONN_COPY);
        netconn_write(req->conn, replacement, strlen(replacement),
                      NETCONN_COPY);
        offset = var_end + end_delim_len;

        free(replacement);
      } else {
        int len = 1024;
        if (offset + len > body_len)
          len = body_len - offset;

        netconn_write(req->conn, body + offset, len, NETCONN_COPY);
        offset += 1024;
      }
    } else {
      int len = 1024;
      if (offset + len > body_len)
        len = body_len - offset;

      netconn_write(req->conn, body + offset, len, NETCONN_COPY);
      offset += 1024;
    }
  }
}

void webserver_send_response(http_req *req, http_res *res) {
  webserver_send_status(req->conn, res->status_code, res->status_text);
  webserver_send_header(req->conn, "Access-Control-Allow-Origin", "*");
  webserver_send_header(req->conn, "Content-Type", res->content_type);

  if (res->content_encoding != NULL)
    webserver_send_header(req->conn, "Content-Encoding", res->content_encoding);

  webserver_send_body(req->conn, res->body, res->body_len);
}

void webserver_send_response_template(http_req *req, http_res *res) {
  webserver_send_status(req->conn, res->status_code, res->status_text);
  webserver_send_header(req->conn, "Access-Control-Allow-Origin", "*");
  webserver_send_header(req->conn, "Content-Type", res->content_type);

  if (res->content_encoding != NULL)
    webserver_send_header(req->conn, "Content-Encoding", res->content_encoding);

  webserver_send_body_template(req, res->body, res->body_len,
                               res->start_delimiter, res->end_delimiter,
                               res->variable_callback);
}

err_t webserver_pipe_body_to_file(http_req *req, char *file_path) {
  char *expect = webserver_get_request_header(req, "Expect");

  if (expect != NULL) {
    webserver_send_status(req->conn, 100, "Continue");
  }

  char *buf = malloc(RCV_BUFFER_SIZE);

  if (buf == NULL) {
    ESP_LOGE(TAG, "Failed to allocate buffer for upload");
    return ESP_FAIL;
  }

  unsigned int r = 0, buffer_offset = 0;

  http_upload_params *upload_params = malloc(sizeof(http_upload_params));
  upload_params->running = true;
  upload_params->file_handle = fopen(file_path, "w");

  if (upload_params->file_handle == NULL) {
    ESP_LOGE(TAG, "Failed to open file for upload");
    return ESP_FAIL;
  }

  while ((r = webserver_recv_body(req, buf + buffer_offset,
                                  RCV_BUFFER_SIZE - buffer_offset)) != 0) {
    buffer_offset += r;

    if (buffer_offset < RCV_BUFFER_SIZE && req->remaining_content_length > 0) {
      continue; // Keep receiving until the buffer is full
    }

    fwrite(buf, buffer_offset, 1, upload_params->file_handle);
    buffer_offset = 0;
  }

  fclose(upload_params->file_handle);
  free(buf);

  return ESP_OK;
}

unsigned int webserver_recv_body(http_req *req, char *buf, unsigned int len) {
  len =
      req->remaining_content_length < len ? req->remaining_content_length : len;

  if (len == 0) {
    return 0;
  }

  if (req->recv_buf && len < req->recv_buf_len) {
    memcpy(buf, req->recv_buf, len);
    unsigned int leftover_len = req->recv_buf_len - len;
    char *leftover = (char *)malloc(leftover_len);

    if (!leftover) {
      // Handle memory allocation failure
      ESP_LOGE(TAG, "Leftover memory allocation failed. %d", leftover_len);
      return 0;
    }

    memcpy(leftover, req->recv_buf + len, leftover_len);
    free(req->recv_buf);

    req->recv_buf = leftover;
    req->recv_buf_len = leftover_len;
    req->remaining_content_length -= len;
    return len;
  }

  unsigned int copied_len = 0;

  if (req->recv_buf) {
    // copy the data we have
    memcpy(buf, req->recv_buf, req->recv_buf_len);
    copied_len += req->recv_buf_len;
    req->remaining_content_length -= copied_len;

    free(req->recv_buf);
    req->recv_buf = NULL;
    req->recv_buf_len = 0;
    return copied_len;
  }

  struct netbuf *nb;
  if (netconn_recv(req->conn, &nb) == ERR_OK) {
    void *data;
    u16_t datalen;
    netbuf_data(nb, &data, &datalen);

    datalen = datalen < len - copied_len ? datalen : len - copied_len;
    memcpy(buf + copied_len, data, datalen);

    req->remaining_content_length -= datalen;
    copied_len += datalen;

    netbuf_delete(nb);
  } else {
    // Handle reception error
    ESP_LOGE(TAG, "Reception error");
  }

  return copied_len;
}

bool url_match(const char *pattern, const char *candidate, int p, int c) {
  if (pattern[p] == '\0') {
    return candidate[c] == '\0';
  } else if (pattern[p] == '*') {
    for (; candidate[c] != '\0'; c++) {
      if (url_match(pattern, candidate, p + 1, c))
        return true;
    }
    return url_match(pattern, candidate, p + 1, c);
  } else if (pattern[p] != '?' && pattern[p] != candidate[c]) {
    return false;
  } else {
    return url_match(pattern, candidate, p + 1, c + 1);
  }
}

int webserver_set_method(http_req *req, char *str_method) {
  if (strcmp(str_method, "GET") == 0) {
    req->method = GET;
  } else if (strcmp(str_method, "HEAD") == 0) {
    req->method = HEAD;
  } else if (strcmp(str_method, "POST") == 0) {
    req->method = POST;
  } else if (strcmp(str_method, "PUT") == 0) {
    req->method = PUT;
  } else if (strcmp(str_method, "DELETE") == 0) {
    req->method = DELETE;
  } else if (strcmp(str_method, "OPTIONS") == 0) {
    req->method = OPTIONS;
  } else {
    return -1;
  }

  return 0;
}

http_param *webserver_get_param(http_req *req, char *name) {
  http_param *p = req->params;
  while (p != NULL) {
    if (strcmp(p->name, name) == 0) {
      return p;
    }
    p = p->next;
  }

  return NULL;
}

void webserver_add_param(http_req *req, char *name, char *value) {
  ESP_LOGI(TAG, "PARAM. name=%s value=%s", name, value);

  http_param *new_param = malloc(sizeof(http_param));
  new_param->name = strdup(name);
  new_param->value = strdup(value);
  new_param->next = NULL;

  if (req->params == NULL) {
    req->params = new_param;
  } else {
    http_param *p = req->params;
    while (p->next != NULL) {
      p = p->next;
    }
    p->next = new_param;
  }
}

void webserver_free_params(http_req *req) {
  http_param *p = req->params;
  while (p != NULL) {
    http_param *next = p->next;
    free(p->name);
    free(p->value);
    free(p);
    p = next;
  }
}

void webserver_parse_params(http_req *req, char *params) {
  ESP_LOGI(TAG, "PARAMS. %s", params);

  char *saveptr1;
  char *saveptr2;
  char *param = strtok_r(params, "&", &saveptr1);

  while (param != NULL) {
    char *name = strtok_r(param, "=", &saveptr2);
    char *value = strtok_r(NULL, "=", &saveptr2);
    if (value == NULL) {
      value = "";
    }
    webserver_add_param(req, name, value);
    param = strtok_r(NULL, "&", &saveptr1);
  }
}

int webserver_parse_url_params(http_req *req, char *str_url) {
  char *url = strtok(str_url, "?");
  char *params = strtok(NULL, " ");

  req->url = strdup(url);

  if (params != NULL)
    webserver_parse_params(req, params);

  return 0;
}

char *webserver_get_request_header(http_req *req, char *key) {
  http_header *current = req->headers;

  while (current) {
    if (strcmp(current->key, key) == 0)
      return current->value;

    current = current->next;
  }

  return NULL;
}

void webserver_append_request_header(http_req *req, char *str_header) {
  http_header *new_header = malloc(sizeof(http_header));

  char *key = strtok(str_header, ": ");
  char *value = strtok(NULL, "") + 1;

  // Remove the space at the start of the value if it exists
  if (value[0] == ' ')
    new_header->value++;

  new_header->next = NULL;
  new_header->key = strdup(key);
  new_header->value = strdup(value);

  if (req->headers == NULL) {
    req->headers = new_header;
    return;
  }

  http_header *current = req->headers;

  while (current->next != NULL) {
    current = current->next;
  }

  current->next = new_header;
}

void webserver_free_request_headers(http_req *req) {
  if (req->headers == NULL)
    return;

  http_header *current = req->headers;
  http_header *next;

  while (current) {
    next = current->next;

    free(current->key);
    free(current->value);
    free(current);

    current = next;
  }
}

bool webserver_check_basic_auth(http_req *req, char *auth_user,
                                char *auth_password) {
  char *auth_header = webserver_get_request_header(req, "Authorization");

  if (!auth_header)
    return false;

  strtok(auth_header, " ");
  char *value = strtok(NULL, "");

  char auth_value[128];
  size_t auth_value_len = 0;
  mbedtls_base64_decode((unsigned char *)auth_value, 128, &auth_value_len,
                        (unsigned char *)value, strlen(value));

  auth_value[auth_value_len] = 0;

  char *username = strtok(auth_value, ":");
  char *password = strtok(NULL, "");

  if (username == NULL || password == NULL)
    return false;

  return strcmp(username, auth_user) == 0 &&
         strcmp(password, auth_password) == 0;
}

void webserver_auth_challenge(http_req *req) {
  webserver_send_status(req->conn, 401, "Unauthorized");
  webserver_send_header(req->conn, "WWW-Authenticate",
                        "Basic realm=\"ESPHTTPD\"");
  webserver_send_header(req->conn, "Access-Control-Allow-Origin", "*");

  netconn_write(req->conn, "\r\n", 2, NETCONN_COPY);
}

void webserver_handle_cors_options(http_req *req) {
  webserver_send_status(req->conn, 200, "OK");
  webserver_send_header(req->conn, "Access-Control-Allow-Origin", "*");
  webserver_send_header(
      req->conn, "Access-Control-Allow-Headers",
      "Origin, X-Requested-With, Content-Type, Accept, Authorization");
  webserver_send_header(req->conn, "Access-Control-Allow-Methods",
                        "GET,HEAD,OPTIONS,POST,PUT");

  netconn_write(req->conn, "\r\n", 2, NETCONN_COPY);
}

void webserver_send_file_response(http_req *req, const char *file_path,
                                  const char *content_type) {
  char *buf = (char *)malloc(SEND_BUFFER_SIZE);
  if (!buf) {
    ESP_LOGE(TAG, "Send file memory allocation failed");
    webserver_send_status(req->conn, 500, "Internal Server Error");
    return;
  }

  FILE *f = fopen(file_path, "rb");
  if (!f) {
    ESP_LOGE(TAG, "File not found: %s", file_path);
    webserver_send_not_found(req);
    free(buf);
    return;
  }

  fseek(f, 0L, SEEK_END);
  size_t file_size = ftell(f);
  rewind(f);

  webserver_send_status(req->conn, 200, "OK");
  webserver_send_header(req->conn, "Access-Control-Allow-Origin", "*");
  webserver_send_header(req->conn, "Access-Control-Expose-Headers",
                        "Content-Length");
  webserver_send_header(req->conn, "Content-Type", content_type);
  sprintf(buf, "%u", (unsigned int)file_size);
  webserver_send_header(req->conn, "Content-Length", buf);

  netconn_write(req->conn, "\r\n", 2, NETCONN_COPY);

  size_t n = 0;
  err_t err = ERR_OK;
  while ((n = fread(buf, 1, SEND_BUFFER_SIZE, f)) > 0 && err == ERR_OK) {
    err = netconn_write(req->conn, buf, n, NETCONN_COPY);
  }

  fclose(f);
  free(buf);

  if (err != ERR_OK) {
    ESP_LOGE(TAG, "File transfer failed: %d", (int)err);
  }
}

void webserver_send_not_found(http_req *req) {
  http_res res = {.status_code = 404,
                  .status_text = "Not Found",
                  .content_type = "text/html",
                  .body = "Not found",
                  .body_len = 9};

  webserver_send_response(req, &res);
}

static bool webserver_is_ws_request(http_req *req) {
  char *ws_header = webserver_get_request_header(req, "Upgrade");

  return (ws_header != NULL) && (strcmp(ws_header, "websocket") == 0);
}

ws_ctx *register_ws_ctx(ws_ctx *ctx) {
  xSemaphoreTake(ws_connection_semaphore, portMAX_DELAY);

  for (int i = 0; i < MAX_WS_CONNECTIONS; i++) {
    if (ws_connections[i] == NULL) {
      ws_connections[i] = ctx;

      xSemaphoreGive(ws_connection_semaphore);

      return ctx;
    }
  }

  free(ctx);
  xSemaphoreGive(ws_connection_semaphore);

  return NULL;
}

void remove_ws_ctx(ws_ctx *ctx) {
  xSemaphoreTake(ws_connection_semaphore, portMAX_DELAY);

  for (int i = 0; i < MAX_WS_CONNECTIONS; i++) {
    if (ws_connections[i] == ctx) {
      free(ws_connections[i]);
      ws_connections[i] = NULL;
      xSemaphoreGive(ws_connection_semaphore);
      return;
    }
  }

  xSemaphoreGive(ws_connection_semaphore);
}

unsigned int webserver_ws_connection_count() {
  unsigned int count = 0;

  if (esphttpd_task_handle == NULL)
    return count;

  xSemaphoreTake(ws_connection_semaphore, portMAX_DELAY);

  vTaskDelay(100 / portTICK_PERIOD_MS);

  for (int i = 0; i < MAX_WS_CONNECTIONS; i++) {
    if (ws_connections[i] != NULL)
      count++;
  }

  vTaskDelay(100 / portTICK_PERIOD_MS);

  xSemaphoreGive(ws_connection_semaphore);

  return count;
}

err_t webserver_broadcast_ws_message(char *p_data, size_t length,
                                     ws_opcode_t opcode) {
  if (esphttpd_task_handle == NULL)
    return ESP_OK;

  for (int i = 0; i < MAX_WS_CONNECTIONS; i++) {
    if (ws_connections[i] != NULL)
      webserver_send_ws_message(ws_connections[i], p_data, length, opcode);
  }

  return ESP_OK;
}

err_t webserver_send_ws_message(ws_ctx *ctx, const char *p_data, size_t length,
                                ws_opcode_t opcode) {
  if (esphttpd_task_handle == NULL) {
    return ESP_OK;
  }

  if (ctx->conn == NULL) {
    return ERR_CONN;
  }

  const unsigned int data_offset =
      length < 126 ? sizeof(WS_frame_header_t) : sizeof(WS_frame_header_t) + 2;

  char *data = (char *)malloc(data_offset + length);
  if (data == NULL) {
    return ERR_MEM;
  }

  // Prepare header
  WS_frame_header_t *hdr = (WS_frame_header_t *)data;
  hdr->fin = 0x1;
  hdr->mask = 0;
  hdr->reserved = 0;
  hdr->opcode = opcode;

  if (length < 126) {
    hdr->payload_length = length;
  } else {
    hdr->payload_length = 126;
    data[sizeof(WS_frame_header_t)] = (length >> 8) & 0xff;
    data[sizeof(WS_frame_header_t) + 1] = length & 0xff;
  }

  memcpy(data + data_offset, p_data, length);

  err_t err =
      netconn_write(ctx->conn, data, data_offset + length, NETCONN_COPY);

  free(data);

  return err;
}

void webserver_accept_ws(ws_ctx *ctx) {
  if (ctx->accepted) {
    return;
  }

  ctx->accepted = true;

  webserver_send_status(ctx->conn, 101, "Switching Protocols");
  webserver_send_header(ctx->conn, "Upgrade", "websocket");
  webserver_send_header(ctx->conn, "Connection", "Upgrade");

  char *ws_client_key =
      webserver_get_request_header(ctx->req, "Sec-WebSocket-Key");
  if (!ws_client_key) {
    ESP_LOGE(TAG, "Sec-WebSocket-Key header not found");
    return;
  }

  size_t ws_concat_keys_length = strlen(ws_client_key) + strlen(ws_sec_key) + 1;
  char *ws_concat_keys = malloc(ws_concat_keys_length);
  if (!ws_concat_keys) {
    ESP_LOGE(TAG, "WS Concat memory allocation failed");
    return;
  }

  strcpy(ws_concat_keys, ws_client_key);
  strcat(ws_concat_keys, ws_sec_key);

  unsigned char sha1_result[20];
  esp_sha(SHA1, (unsigned char *)ws_concat_keys, strlen(ws_concat_keys),
          sha1_result);

  size_t reply_key_size;
  unsigned char reply_key[64];
  mbedtls_base64_encode(reply_key, 64, &reply_key_size, sha1_result, 20);

  webserver_send_header(ctx->conn, "Sec-WebSocket-Accept", (char *)reply_key);
  netconn_write(ctx->conn, "\r\n", 2, NETCONN_COPY);

  free(ws_concat_keys);

  register_ws_ctx(ctx);
}

static void webserver_ws_task(void *pvParameter) {
  ws_ctx *ctx = (ws_ctx *)pvParameter;
  struct netbuf *buf;
  uint8_t masking_key[WS_MASK_LEN] = {0, 0, 0, 0};

  while (true) {
    if (netconn_recv(ctx->conn, &buf) == ERR_OK) {

      uint8_t *data;
      u16_t len;
      netbuf_data(buf, (void *)&data, &len);

      WS_frame_header_t *frame_header = (WS_frame_header_t *)data;
      data += sizeof(WS_frame_header_t);

      // check if clients want to close the connection
      if (frame_header->opcode == WS_OP_CLS) {
        netbuf_delete(buf);
        break;
      }

      ws_event event = {
          .event_type = WS_MESSAGE,
          .payload = NULL,
          .len = frame_header->payload_length,
      };

      if (event.len == 126) {
        event.len = (data[0] << 8) + data[1];
        data += 2;
      } else if (event.len == 127) {
        // Use uint64_t for the extended payload length

        uint64_t extended_payload_length = 0;
        for (int i = 0; i < 8; i++) {
          extended_payload_length =
              (extended_payload_length << 8) + (uint8_t)data[i];
        }
        event.len = extended_payload_length;

        data += 8;
      }

      // Read the mask
      if (frame_header->mask) {
        memcpy(masking_key, data, WS_MASK_LEN);
        data += WS_MASK_LEN;
      }

      if (event.len > 0) {
        event.payload = malloc(event.len);

        if (event.payload == NULL) {
          // Handle memory allocation failure
          netbuf_delete(buf);
          continue;
        }

        if (frame_header->mask) {

          for (int i = 0; i < event.len; i++) {
            event.payload[i] = data[i] ^ masking_key[i % WS_MASK_LEN];
          }
        }
      }

      ctx->handler(ctx, &event);
      free(event.payload);
      netbuf_delete(buf);
    }
  }

  remove_ws_ctx(ctx);
  netconn_close(ctx->conn);

  vTaskDelete(NULL);
}

static void webserver_handle_ws(http_req *req) {
  ESP_LOGI(TAG, "WS Connection start");

  void (*handler)(ws_ctx *, ws_event *) = NULL;
  ws_route *current = ws_routes;

  while (current) {
    if (strcmp(req->url, current->url) == 0) {
      handler = current->callback;
      break;
    }
    current = current->next;
  };

  if (handler) {
    ws_ctx *ctx = calloc(1, sizeof(ws_ctx));
    assert(ctx != NULL);

    ctx->conn = req->conn; // replace sock with conn
    ctx->handler = handler;
    ctx->req = req;

    ws_event event = {
        .event_type = WS_CONNECT,
    };

    ctx->handler(ctx, &event);

    if (ctx->accepted) {
      ctx->req = NULL;
      xTaskCreate(webserver_ws_task, "webserver_ws_task", 3072, (void *)ctx,
                  tskIDLE_PRIORITY, NULL);
    } else {
      webserver_send_not_found(req);
      netconn_close(ctx->conn);
      netconn_delete(ctx->conn);
      free(ctx);
    }
  } else {
    webserver_send_not_found(req);
    netconn_close(req->conn);
    netconn_delete(req->conn);
  }
}

static void webserver_handle_http(http_req *req) {
  void (*handler)(http_req *) = NULL;
  http_route *current = http_routes;

  while (current) {
    if (req->method == current->method &&
        url_match(current->url, req->url, 0, 0)) {
      handler = current->callback;
      break;
    }
    current = current->next;
  };

  if (handler) {
    if (req->method == POST) {
      char *content_length_str =
          webserver_get_request_header(req, "Content-Length");
      if (content_length_str) {
        req->remaining_content_length = atoi(content_length_str);
      } else {
        ESP_LOGE(TAG, "Content-Length header not found");
      }
      // req->remaining_content_length -= req->recv_buf_len;
    }

    handler(req);
  } else {
    webserver_send_not_found(req);
  }
}

static void webserver_serve(struct netconn *conn) {
  if (!conn)
    return;

  struct netbuf *buf;
  void *data;
  u16_t len;

  http_req req = {
      .conn = conn, .headers = NULL, .recv_buf = NULL, .recv_buf_len = 0};
  char *str_method = "GET";

  unsigned int c = 0;
  bool reached_body = false;
  int64_t start_time = esp_timer_get_time();

  while (!reached_body) {
    err_t err = netconn_recv(conn, &buf);
    if (err != ERR_OK) {
      ESP_LOGE(TAG, "recv failed: error %d", err);
      goto cleanup;
    }

    netbuf_data(buf, &data, &len);

    // Assuming data is a char buffer; if not, you'll have to adjust this
    char *rx_buffer = (char *)data;
    unsigned int rx_buffer_length = len;

    char *line_start = rx_buffer;
    char *line_end = NULL;

    while ((line_end = strstr(line_start, "\r\n")) != NULL) {
      *line_end = 0;

      if (line_start == line_end) {
        line_end += 2;
        int leftover_len = (rx_buffer + rx_buffer_length) - line_end;

        if (leftover_len > 0) {
          char *leftover = malloc(leftover_len);
          memcpy(leftover, line_end, leftover_len);

          req.recv_buf = leftover;
          req.recv_buf_len = leftover_len;
        } else {
          free(req.recv_buf);
          req.recv_buf = NULL;
          req.recv_buf_len = 0;
        }

        reached_body = true;
        break;
      } else if (c == 0) {
        str_method = strtok(line_start, " ");
        char *str_url = strtok(NULL, " ");

        webserver_set_method(&req, str_method);
        webserver_parse_url_params(&req, str_url);
      } else {
        webserver_append_request_header(&req, line_start);
      }

      c++;
      line_start = line_end + 2;
    }
    netbuf_delete(buf);
  }

  if (webserver_is_ws_request(&req)) {
    str_method = "WS";
    webserver_handle_ws(&req);
  } else {
    webserver_handle_http(&req);
    netconn_close(conn);
    netconn_delete(conn);
  };

  int64_t end_time = (esp_timer_get_time() - start_time) / 1000;
  ESP_LOGI(TAG, "%d %s %lldms\n", req.method, req.url, end_time);

cleanup:
  webserver_free_request_headers(&req);
  free(req.url);
  webserver_free_params(&req);
}

void webserver_add_route(http_route new_route) {
  http_route *route = malloc(sizeof(http_route));
  memcpy(route, &new_route, sizeof(http_route));
  route->next = NULL;

  if (http_routes == NULL) {
    http_routes = route;
    return;
  }

  http_route *current_route = http_routes;
  while (current_route->next) {
    current_route = current_route->next;
  }

  current_route->next = route;
}

void webserver_add_ws_route(ws_route new_route) {
  ws_route *route = malloc(sizeof(ws_route));
  memcpy(route, &new_route, sizeof(ws_route));
  route->next = NULL;

  if (ws_routes == NULL) {
    ws_routes = route;
    return;
  }

  ws_route *current_route = ws_routes;
  while (current_route->next) {
    current_route = current_route->next;
  }

  current_route->next = route;
}

struct netconn *webserver_listen() {
  struct netconn *conn;
  ip4_addr_t ipaddr;
  IP4_ADDR(&ipaddr, 0, 0, 0, 0);

  /** Create streaming socket */
  conn = netconn_new(NETCONN_TCP);
  if (conn == NULL) {
    ESP_LOGE(TAG, "netconn_new failed");
    return NULL;
  }

  /** Bind the socket to port WEBSERVER_PORT */
  if (netconn_bind(conn, (const ip_addr_t *)&ipaddr, WEBSERVER_PORT) !=
      ERR_OK) {
    ESP_LOGE(TAG, "Bind error");
    netconn_delete(conn);
    return NULL;
  }

  /** Make it a "listening socket". Limit to 5 connections */
  if (netconn_listen(conn) != ERR_OK) {
    ESP_LOGE(TAG, "Listen error");
    netconn_delete(conn);
    return NULL;
  }

  return conn;
}

static void webserver_task(void *pvParameters) {
  struct netconn *conn = webserver_listen();
  if (conn == NULL) {
    // Handle error (maybe stop the task)
    vTaskDelete(NULL);
  }

  while (1) {
    struct netconn *new_conn;
    err_t err;
    ip_addr_t naddr;
    u16_t port;

    /** accept an incoming connection */
    err = netconn_accept(conn, &new_conn);
    if (err == ERR_OK) {
      if (netconn_peer(new_conn, &naddr, &port) == ERR_OK) {
        ESP_LOGI(TAG, "%s:%d connected", ipaddr_ntoa(&naddr), port);

        webserver_serve(new_conn);
      }
    }
  }
}

void webserver_start(int port) {
  if (esphttpd_task_handle != NULL) {
    ESP_LOGE(TAG, "webserver already started");
    return;
  }

  ws_connection_semaphore = xSemaphoreCreateMutex();

  for (int i = 0; i < MAX_WS_CONNECTIONS; i++) {
    ws_connections[i] = NULL;
  }

  xTaskCreate(webserver_task, "webserver_task", 16384, NULL, 6,
              &esphttpd_task_handle);
}

void webserver_stop() {
  if (esphttpd_task_handle == NULL) {
    ESP_LOGE(TAG, "webserver not started");
    return;
  }

  vTaskDelete(esphttpd_task_handle);
  esphttpd_task_handle = NULL;
  vSemaphoreDelete(ws_connection_semaphore);
}
