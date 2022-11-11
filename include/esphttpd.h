#ifndef _WEBSERVER_H_
#define _WEBSERVER_H_

#include "lwip/api.h"
#include "freertos/ringbuf.h"

typedef enum http_method
{
  GET,
  HEAD,
  POST,
  PUT,
  DELETE,
  OPTIONS
} http_method;
typedef enum ws_event_type
{
  WS_CONNECT,
  WS_DISCONNECT,
  WS_MESSAGE
} ws_event_type;
typedef enum
{
  WS_OP_CON = 0x0, /*!< Continuation Frame*/
  WS_OP_TXT = 0x1, /*!< Text Frame*/
  WS_OP_BIN = 0x2, /*!< Binary Frame*/
  WS_OP_CLS = 0x8, /*!< Connection Close Frame*/
  WS_OP_PIN = 0x9, /*!< Ping Frame*/
  WS_OP_PON = 0xa  /*!< Pong Frame*/
} ws_opcode_t;

typedef struct
{
  RingbufHandle_t buf_handle;
  FILE *file_handle;
  bool running;
} http_upload_params;

typedef struct
{
  char *key;
  char *value;
  struct http_header *next;
} http_header;

typedef struct http_param
{
  char *name;
  char *value;
  struct http_param *next;
} http_param;

typedef struct
{
  int sock;
  http_method method;
  char *url;
  http_param *params;
  http_header *headers;

  char *recv_buf;
  unsigned int recv_buf_len;
  unsigned int remaining_content_length;
} http_req;

typedef struct
{
  int status_code;
  char *status_text;
  char *content_type;
  char *content_encoding;
  char *body;
  unsigned int body_len;
} http_res;

typedef struct
{
  ws_event_type event_type;
  char *payload;
  unsigned int len;
} ws_event;

typedef struct
{
  int sock;
  void (*handler)(struct ws_ctx *, ws_event *);
  struct http_req *req;
} ws_ctx;

typedef struct
{
  http_method method;
  char *url;
  void (*callback)(struct netconn *, http_req *);
  struct http_route *next;
} http_route;

typedef struct
{
  char *url;
  void (*callback)(ws_event *);
  struct ws_route *next;
} ws_route;

typedef struct
{
  uint8_t opcode : 4;
  uint8_t reserved : 3;
  uint8_t fin : 1;
  uint8_t payload_length : 7;
  uint8_t mask : 1;
} WS_frame_header_t;

err_t webserver_pipe_body_to_file(http_req *req, char *file_path);
unsigned int webserver_recv_body(http_req *req, char *buf, unsigned int len);
void webserver_accept_ws(ws_ctx *ctx);
static void webserver_send_not_found(http_req *req);
err_t webserver_broadcast_ws_message(char *p_data, size_t length, ws_opcode_t opcode);
err_t webserver_send_ws_message(ws_ctx *ctx, char *p_data, size_t length, ws_opcode_t opcode);
void webserver_send_response(http_req *req, http_res *res);
void webserver_send_file_response(http_req *req, char *file_path, char *content_type);
void webserver_add_route(http_route new_route);
void webserver_add_ws_route(ws_route new_route);
static void webserver_serve(int clientfd);
static void webserver_task();
void webserver_start(int port);
void webserver_stop();
void webserver_send_body(http_req *req, char *body, unsigned int body_len);
void webserver_send_status(http_req *req, int status_code, char *status_text);
void webserver_send_header(http_req *req, char *key, char *value);
char *webserver_get_request_header(http_req *req, char *key);
void webserver_auth_challenge(http_req *req);
bool webserver_check_basic_auth(http_req *req, char *auth_user, char *auth_password);
http_param *webserver_get_param(http_req *req, char *name);
void webserver_handle_cors_options(http_req *req);

#endif
