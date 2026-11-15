#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "esp_http_server.h"
esp_err_t korvo_planos_register(httpd_handle_t server, bool (*authenticated)(httpd_req_t *));
