#include "esphttpd.h"

#include "freertos/FreeRTOS.h"
#include "esp_heap_alloc_caps.h"
#include "hwcrypto/sha.h"
#include "esp_system.h"
#include "mbedtls/base64.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include <arpa/inet.h>


#define WEBSERVER_PORT (80)
#define MAX_WS_CONNECTIONS (16)
#define WS_MASK_LEN (4)

#define SEND_BUFFER_SIZE 1024
#define RCV_BUFFER_SIZE 2048

http_route *http_routes = NULL;
http_route *ws_routes = NULL;

static const char ws_sec_key[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
static const char http_resp_main_header[] = "HTTP/1.1 %d %s\r\n";
static const char http_resp_header[] = "%s: %s\r\n";
static const char* TAG = "ESPHTTPD";

SemaphoreHandle_t ws_connection_semaphore;
ws_ctx *ws_connections[16] = { NULL };

void webserver_send_status(http_req *req, int status_code, char *status_text){
  char buf[128];

  sprintf(buf, http_resp_main_header, status_code, status_text);
  send(req->sock, buf, strlen(buf), 0);
}

void webserver_send_header(http_req *req, char *key, char *value){
  char buf[128];

  sprintf(buf, http_resp_header, key, value);
  send(req->sock, buf, strlen(buf), 0);
}

void webserver_send_body(http_req *req, char *body, unsigned int body_len){
  send(req->sock, "\r\n", 2, 0);
  send(req->sock, body, body_len, 0);
}

void webserver_send_response(http_req *req, http_res *res)
{
  webserver_send_status(req, res->status_code, res->status_text);
  webserver_send_header(req, "Access-Control-Allow-Origin", "*");
  webserver_send_header(req, "Content-Type", res->content_type);
  webserver_send_body(req, res->body, res->body_len);
}

void upload_writer_thread(http_upload_params *upload_params){
  size_t item_size;

  while(upload_params->running){
    char *item = (char *)xRingbufferReceive(upload_params->buf_handle, &item_size, 100/portTICK_PERIOD_MS);

    if(item != NULL){
      //fwrite(item, item_size, 1, upload_params->file_handle);
      vRingbufferReturnItem(upload_params->buf_handle, (void *) item);
    }
  }

  fclose(upload_params->file_handle);
  vRingbufferDelete(upload_params->buf_handle);
  free(upload_params);
  vTaskDelete(NULL);
}

err_t webserver_pipe_body_to_file(http_req *req, char *file_path){
  char* expect = webserver_get_request_header(req, "Expect");

  if(expect != NULL){
    webserver_send_status(req, 100, "Continue");
  }

  unsigned long start_time = esp_timer_get_time();
  char *buf = malloc(RCV_BUFFER_SIZE);
  unsigned int total_bytes = 0, r = 0, buffer_offset = 0;

  http_upload_params *upload_params = malloc(sizeof(http_upload_params));
  upload_params->running = true;
  upload_params->file_handle = fopen(file_path, "w");

  if (upload_params->file_handle == NULL){
    printf("Upload failed\n");
    return ESP_FAIL;
  }

  while( (r = webserver_recv_body(req, buf + buffer_offset, RCV_BUFFER_SIZE - buffer_offset)) != 0){
    buffer_offset += r;

    if(buffer_offset < RCV_BUFFER_SIZE && req->remaining_content_length > 0){
      continue; // Keep receiving until the buffer is full
    }

    fwrite(buf, buffer_offset, 1, upload_params->file_handle);
    total_bytes += buffer_offset;


    buffer_offset = 0;
  }

  fclose(upload_params->file_handle);
  free(buf);

/*
  upload_params->buf_handle = xRingbufferCreate(4*RCV_BUFFER_SIZE, RINGBUF_TYPE_NOSPLIT);

  if (upload_params->buf_handle == NULL) {
    return ESP_ERR_NO_MEM;
  }

  TaskHandle_t thread_handle = NULL;

  xTaskCreate(
    upload_writer_thread,
    "upload_writer_thread",
    2048,
    (void *) upload_params,
    10,
    &thread_handle
  );

  UBaseType_t rbuf_result;
  while( (r = webserver_recv_body(req, buf, RCV_BUFFER_SIZE)) != 0 ){
    while(xRingbufferSend(upload_params->buf_handle, buf, r, portMAX_DELAY) != pdTRUE){
      vTaskDelay(1/portTICK_PERIOD_MS);
    }

    total_bytes += r;
  }

  free(buf);

  upload_params->running = false;
*/


  double delta_time = (esp_timer_get_time() - start_time) / (1000.0 * 1000.0);
  double result = (total_bytes / 1024.0) / delta_time;
  ESP_LOGE(TAG, "total_bytes=%d, kB/s=%f, t=%f\n", total_bytes, result, delta_time);

  return ESP_OK;
}

unsigned int webserver_recv_body(http_req *req, char *buf, unsigned int len){
  len = req->remaining_content_length < len ? req->remaining_content_length : len;

  if(len == 0) return 0;

  if(req->recv_buf && len <= req->recv_buf_len) {
    memcpy(buf, req->recv_buf, len);

    unsigned int leftover_len = req->recv_buf_len - len;
    char *leftover = malloc(leftover_len);
    memcpy(leftover, req->recv_buf + len, leftover_len);
    free(req->recv_buf);

    req->recv_buf = leftover;
    req->recv_buf_len = leftover_len;
    req->remaining_content_length -= len;
    return len;
  }

  int copied_len = 0;

  if(req->recv_buf){
    // copy the data we have
    memcpy(buf, req->recv_buf, req->recv_buf_len);
    copied_len += req->recv_buf_len;
    req->remaining_content_length -= copied_len;

    buf += req->recv_buf_len;
    len -= req->recv_buf_len;

    free(req->recv_buf);
    req->recv_buf = NULL;
    req->recv_buf_len = 0;
  }

  int rxlen = recv(req->sock, buf + copied_len, len, 0);

  req->remaining_content_length -= rxlen;
  copied_len += rxlen;

  return copied_len;
}


bool url_match(const char *pattern, const char *candidate, int p, int c) {
  if (pattern[p] == '\0') {
    return candidate[c] == '\0';
  } else if (pattern[p] == '*') {
    for (; candidate[c] != '\0'; c++) {
      if (url_match(pattern, candidate, p+1, c))
        return true;
    }
    return url_match(pattern, candidate, p+1, c);
  } else if (pattern[p] != '?' && pattern[p] != candidate[c]) {
    return false;
  }  else {
    return url_match(pattern, candidate, p+1, c+1);
  }
}

int webserver_set_method(http_req *req, char *str_method){
  if (strcmp(str_method, "GET") == 0) {
    req->method = GET;
  }
  else if (strcmp(str_method, "HEAD") == 0) {
    req->method = HEAD;
  }
  else if (strcmp(str_method, "POST") == 0) {
    req->method = POST;
  }
  else if (strcmp(str_method, "PUT") == 0) {
    req->method = PUT;
  }
  else if (strcmp(str_method, "DELETE") == 0) {
    req->method = DELETE;
  }
  else if (strcmp(str_method, "OPTIONS") == 0) {
    req->method = OPTIONS;
  }
  else{
    return -1;
  }

  return 0;
}

int webserver_parse_url_params(http_req *req, char *str_url)
{
  char *url = strtok(str_url, "?");
  char *params = strtok(NULL, " ");

  req->url = strdup(url);

  if(params != NULL)
    req->params = strdup(params);

  return 0;
}

char* webserver_get_request_header(http_req *req, char *key)
{
  http_header *current = req->headers;

  while(current){
    if(strcmp(current->key, key) == 0)
      return current->value;

    current = current->next;
  }

  return NULL;
}

void webserver_append_request_header(http_req *req, char *str_header)
{
  http_header *new_header = malloc(sizeof(http_header));

  char *key = strtok(str_header, ": ");
  char *value = strtok(NULL, "") + 1;

  // Remove the space at the start of the value if it exists
  if(value[0] == ' ') new_header->value++;

  new_header->next = NULL;
  new_header->key = strdup(key);
  new_header->value = strdup(value);

  if(req->headers == NULL){
    req->headers = new_header;
    return;
  }

  http_header *current = req->headers;

  while(current->next != NULL){
    current = current->next;
  }

  current->next = new_header;
}

void webserver_free_request_headers(http_req *req){
  if(req->headers == NULL) return;

  http_header *current = req->headers;
  http_header *next;

  while(current){
    next = current->next;

    free(current->key);
    free(current->value);
    free(current);

    current = next;
  }
}

void webserver_send_file_response(http_req *req, char *file_path, char *content_type)
{
  char *buf = malloc(SEND_BUFFER_SIZE);
  size_t n = 0;

  FILE* f = fopen(file_path, "r");

  if (f == NULL) {
    ESP_LOGE(TAG, "File not found %s", file_path);
    webserver_send_not_found(req);
    return;
  }

  fseek(f, 0L, SEEK_END);
  size_t file_size = ftell(f);
  rewind(f);

  webserver_send_status(req, 200, "OK");
  webserver_send_header(req, "Access-Control-Allow-Origin", "*");
  webserver_send_header(req, "Access-Control-Expose-Headers", "Content-Length");
  webserver_send_header(req, "Content-Type", content_type);
  sprintf(buf, "%u", file_size);
  webserver_send_header(req, "Content-Length", buf);

  send(req->sock, "\r\n", 2, 0);
  

  
  while(
    (n = fread(buf, 1, SEND_BUFFER_SIZE, f)) != 0 &&
    send(req->sock, buf, n, 0) != -1
  ){}

  fclose(f);
  free(buf);
}

static void webserver_send_not_found(http_req *req){
  http_res res = {
    .status_code = 404,
    .status_text = "Not Found",
    .content_type = "text/html",
    .body = "Not found",
    .body_len = 9
  };

  webserver_send_response(req, &res);
}

static bool webserver_is_ws_request(http_req* req){
  char *ws_header = webserver_get_request_header(req, "Upgrade");

  return (ws_header != NULL) && (strcmp(ws_header, "websocket") == 0);
}


ws_ctx* register_ws_ctx(ws_ctx *ctx){
  xSemaphoreTake(ws_connection_semaphore, portMAX_DELAY);

  for(int i=0; i < MAX_WS_CONNECTIONS; i++){
    if(ws_connections[i] == NULL) {
      ws_connections[i] = ctx;
      xSemaphoreGive(ws_connection_semaphore);

      return ctx;
    }
  }

  free(ctx);
  xSemaphoreGive(ws_connection_semaphore);

  return NULL;
}

void remove_ws_ctx(ws_ctx *ctx){
  xSemaphoreTake(ws_connection_semaphore, portMAX_DELAY);

  for(int i=0; i < MAX_WS_CONNECTIONS; i++){
    if(ws_connections[i] == ctx) {
      free(ws_connections[i]);
      ws_connections[i] = NULL;
      xSemaphoreGive(ws_connection_semaphore);
      return;
    }
  }

  xSemaphoreGive(ws_connection_semaphore);
}

err_t webserver_broadcast_ws_message(char* p_data, size_t length, ws_opcode_t opcode){
  for(int i=0; i < MAX_WS_CONNECTIONS; i++){
    if(ws_connections[i] != NULL)
      webserver_send_ws_message(ws_connections[i], p_data, length, opcode);
  }

  return ESP_OK;
}

err_t webserver_send_ws_message(ws_ctx *ctx, char* p_data, size_t length, ws_opcode_t opcode){
	if (ctx->sock == NULL)
		return ERR_CONN;

  const unsigned int data_offset = length < 126 ? sizeof(WS_frame_header_t) : sizeof(WS_frame_header_t) + 2;
  char* data = malloc(data_offset + length);

	//prepare header
	WS_frame_header_t *hdr = (WS_frame_header_t*) data;
	hdr->fin = 0x1;
	hdr->mask = 0;
	hdr->reserved = 0;
	hdr->opcode = opcode;

  if(length < 126) {
  	hdr->payload_length = length;
  } else {
  	hdr->payload_length = 126;

    data[sizeof(WS_frame_header_t)] = (length >> 8) & 0xff;
    data[sizeof(WS_frame_header_t) + 1] = length & 0xff;
  }
  
  memcpy(data + data_offset, p_data, length);
  send(ctx->sock, data, data_offset + length, 0);

  free(data);
  return ESP_OK;
}

void webserver_accept_ws(ws_ctx *ctx){
  webserver_send_status(ctx, 101, "Switching Protocols");
  webserver_send_header(ctx, "Upgrade", "websocket");
  webserver_send_header(ctx, "Connection", "Upgrade");

  char *ws_client_key = webserver_get_request_header(ctx->req, "Sec-WebSocket-Key");
  char *ws_concat_keys = malloc(strlen(ws_client_key) + strlen(ws_sec_key) + 1);

  strcpy(ws_concat_keys, ws_client_key);
  strcat(ws_concat_keys, ws_sec_key);

	unsigned char sha1_result[20];
  esp_sha(SHA1, (unsigned char*) ws_concat_keys, strlen(ws_concat_keys), &sha1_result);
  
  size_t reply_key_size;
  unsigned char reply_key[64];
  mbedtls_base64_encode(reply_key, 64, &reply_key_size, sha1_result, 20);

  webserver_send_header(ctx, "Sec-WebSocket-Accept", (char *) reply_key);
  send(ctx->sock, "\r\n", 2, 0);

  free(ws_concat_keys);
}

static void webserver_ws_task(void *pvParameter){
  ws_ctx *ctx = (ws_ctx *) pvParameter;
	WS_frame_header_t frame_header;
  int length;
  uint64_t payload_length;
  uint8_t masking_key[4];

  ESP_LOGI(TAG, "WS task start");

  register_ws_ctx(ctx);

	while ((length = recv(ctx->sock, (void *) &frame_header, 2, 0)) == 2) {
	  //check if clients wants to close the connection
	  if (frame_header.opcode == WS_OP_CLS){
		  break;
    }

    ws_event event = {
      .event_type = WS_MESSAGE,
      .payload = NULL,
      .len = frame_header.payload_length
    };


    if(event.len == 126){
      char data[2];
      recv(ctx->sock, data, 2, 0);

      event.len = (data[0] << 8) + data[1];

    } else if(event.len == 127){
      char data[4];
      recv(ctx->sock, data, 4, 0);

      event.len = (data[0] << 24) + (data[1] << 16) + (data[2] << 8) + data[3];
    }

    // Read the mask
    if(frame_header.mask){
      recv(ctx->sock, (void *) &masking_key, sizeof(masking_key), 0);
    }
    
    if(event.len > 0){
      event.payload = malloc(event.len);
      recv(ctx->sock, event.payload, event.len, 0);


      if(frame_header.mask){
			  for(int i = 0; i < event.len; i++)
				  event.payload[i] ^= masking_key[i % WS_MASK_LEN];
      }
    }

    ctx->handler(ctx, &event);
    free(event.payload);
  }

  close(ctx->sock);
  remove_ws_ctx(ctx);

  ESP_LOGI(TAG, "WS task end");

  vTaskDelete( NULL );
}

static void webserver_handle_ws(http_req *req){
  ESP_LOGI(TAG, "WS Connection start");

  void (*handler)(ws_ctx *, ws_event *) = NULL;
  ws_route *current = ws_routes;

  while(current){
    if(strcmp(req->url, current->url) == 0){
      handler = current->callback;
      break;
    }
    current = current->next;
  };

  if(handler){
    ws_ctx *ctx = calloc(1, sizeof(ws_ctx));

    ctx->sock = req->sock;
    ctx->handler = handler;
    ctx->req = req;

    ws_event event = {
      .event_type = WS_CONNECT,
    };

    ctx->handler(ctx, &event);

    ctx->req = NULL;

    xTaskCreate(
      webserver_ws_task,
      "webserver_ws_task",
      2048,
      ( void * ) ctx,
      tskIDLE_PRIORITY,
      NULL
    );
  } else {
    webserver_send_not_found(req);
    close(req->sock);
  }
}

static void webserver_handle_http(http_req *req){
  void (*handler)(http_req *) = NULL;
  http_route *current = http_routes;

  while(current){
    if(req->method == current->method && url_match(current->url, req->url, 0, 0)){
      handler = current->callback;
      break;
    }
    current = current->next;
  };

  if(handler) {
    if(req->method == POST){
      char *content_length_str = webserver_get_request_header(req, "Content-Length");

      req->remaining_content_length = atoi(content_length_str);
      //req->remaining_content_length -= req->recv_buf_len;
    }

    handler(req);
  } else {
    webserver_send_not_found(req);
  }

  close(req->sock);
}

static void webserver_serve(int clientfd)
{
  char rx_buffer[1024];
  unsigned int rx_buffer_length = 0;

  err_t err;
  http_req req = {
    .sock = clientfd,
    .headers = NULL,
    .recv_buf = NULL,
    .recv_buf_len = 0
  };
  char *str_method = "GET";
  
  unsigned int c = 0;
  bool reached_body = false;
  unsigned long start_time = esp_timer_get_time();


  while(!reached_body){
    int len = recv(req.sock, rx_buffer + rx_buffer_length, sizeof(rx_buffer) - rx_buffer_length - 1, 0);

    if (len < 0) {
        ESP_LOGE(TAG, "recv failed: errno %d", errno);
        return;
    }

    rx_buffer_length += len;

    char *line_start = rx_buffer;
    char *line_end = NULL;

    while((line_end = strstr(line_start, "\r\n")) != NULL){
      line_end[0] = 0;

      if(line_start == line_end){
        // save anything leftover into the recv buffer
        line_end += 2;
        int leftover_len = (rx_buffer + rx_buffer_length) - line_end;

        if(leftover_len > 0) {
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
      } else if(c == 0){ // first packet so parse http header
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
  }

  if(webserver_is_ws_request(&req)){
    str_method = "WS";
    webserver_handle_ws(&req);
  } else {
    webserver_handle_http(&req);
  };

  unsigned long end_time = (esp_timer_get_time() - start_time) / 1000;
  ESP_LOGI(TAG, "%d %s %lums\n", req.method, req.url, end_time);

  webserver_free_request_headers(&req);

  free(req.url);
  free(req.params);
}

void webserver_add_route(http_route new_route){
  http_route *route = malloc(sizeof(http_route));
  memcpy(route, &new_route, sizeof(http_route));
  route->next = NULL;

  if(http_routes == NULL){
    http_routes = route;
    return;
  }

  http_route *current_route = http_routes;
  while(current_route->next){
    current_route = current_route->next;
  }

  current_route->next = route;
}

void webserver_add_ws_route(ws_route new_route){
  ws_route *route = malloc(sizeof(ws_route));
  memcpy(route, &new_route, sizeof(ws_route));
  route->next = NULL;

  if(ws_routes == NULL){
    ws_routes = route;
    return;
  }

  ws_route *current_route = ws_routes;
  while(current_route->next){
    current_route = current_route->next;
  }

  current_route->next = route;
}

int webserver_listen(){
  int sockfd;
	struct sockaddr_in self;

	/** Create streaming socket */
  if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		ESP_LOGE(TAG, "Socket error");
		return -1;
	}

	/** Initialize address/port structure */
	bzero(&self, sizeof(self));
	self.sin_family = AF_INET;
	self.sin_port = htons(WEBSERVER_PORT);
	self.sin_addr.s_addr = INADDR_ANY;

	/** Assign a port number to the socket */
  if (bind(sockfd, (struct sockaddr*)&self, sizeof(self)) != 0)
	{
		ESP_LOGE(TAG, "Bind error");
		return -1;
	}

	/** Make it a "listening socket". Limit to 20 connections */
	if (listen(sockfd, 5) != 0)
	{
		ESP_LOGE(TAG, "Listen error");
		return -1;
	}

  return sockfd;
}

static void webserver_task()
{
  int sockfd = webserver_listen();

  while (1)
	{
    int clientfd;
		struct sockaddr_in client_addr;
		unsigned int addrlen = sizeof(client_addr);

		/** accept an incomming connection  */
		clientfd = accept(sockfd, (struct sockaddr *)&client_addr, &addrlen);

    ESP_LOGI(TAG, "%s:%d connected", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
    //int flags =1;
    //setsockopt(clientfd, IPPROTO_TCP, TCP_NODELAY, (void *)&flags, sizeof(flags));

		webserver_serve(clientfd);
	}
}


void webserver_start(int port)
{
  ws_connection_semaphore = xSemaphoreCreateMutex();
  xTaskCreate(&webserver_task, "webserver_task", 4096, NULL, 10, NULL);
}
