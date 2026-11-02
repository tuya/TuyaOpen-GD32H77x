/**
 * @file test_http_response_content_length.c
 * @brief Regression tests for valid HTTP responses with no body.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core_http_client.h"
#include "tal_memory.h"

typedef struct {
    const char *wire;
    size_t length;
    size_t offset;
    size_t fragment;
    unsigned send_calls;
} fake_transport_t;

static int fake_send(NetworkContext_t *context, const void *data, size_t length)
{
    fake_transport_t *transport = (fake_transport_t *)(void *)context;
    (void)data;
    transport->send_calls++;
    return (int)length;
}

static int fake_recv(NetworkContext_t *context, void *data, size_t length)
{
    fake_transport_t *transport = (fake_transport_t *)(void *)context;
    if (transport->offset >= transport->length) {
        return 0;
    }

    size_t remaining = transport->length - transport->offset;
    size_t chunk = remaining < length ? remaining : length;
    if (transport->fragment != 0U && chunk > transport->fragment) {
        chunk = transport->fragment;
    }
    memcpy(data, transport->wire + transport->offset, chunk);
    transport->offset += chunk;
    return (int)chunk;
}

void *tal_malloc(size_t size)
{
    return malloc(size);
}

void tal_free(void *ptr)
{
    free(ptr);
}

OPERATE_RET tal_log_print_secure(BOOL_T is_const_fmt, TAL_LOG_LEVEL_E level,
                                 const char *file, int line, const char *format, ...)
{
    (void)is_const_fmt;
    (void)level;
    (void)file;
    (void)line;
    (void)format;
    return OPRT_OK;
}

static void init_headers(HTTPRequestHeaders_t *headers, uint8_t *buffer, size_t length)
{
    memset(headers, 0, sizeof(*headers));
    headers->pBuffer = buffer;
    headers->bufferLen = length;

    const HTTPRequestInfo_t info = {
        .pMethod = HTTP_METHOD_GET,
        .methodLen = sizeof(HTTP_METHOD_GET) - 1U,
        .pHost = "a2.tuyacn.com",
        .hostLen = sizeof("a2.tuyacn.com") - 1U,
        .pPath = "/",
        .pathLen = 1U,
    };
    assert(HTTPClient_InitializeRequestHeaders(headers, &info) == HTTPSuccess);
}

static HTTPStatus_t request(const char *wire, size_t fragment, HTTPResponse_t *response)
{
    uint8_t header_buffer[512];
    HTTPRequestHeaders_t headers;
    init_headers(&headers, header_buffer, sizeof(header_buffer));

    fake_transport_t transport = {
        .wire = wire,
        .length = strlen(wire),
        .fragment = fragment,
    };
    TransportInterface_t interface = {
        .pNetworkContext = (NetworkContext_t *)(void *)&transport,
        .send = fake_send,
        .recv = fake_recv,
    };

    memset(response, 0, sizeof(*response));
    response->pBuffer = tal_malloc(4096U);
    assert(response->pBuffer != NULL);
    response->bufferLen = 4096U;
    return HTTPClient_Send(&interface, &headers, NULL, 0U, response, 0U);
}

static void free_response(HTTPResponse_t *response)
{
    /* HTTPClient_Send places the body inside the caller-provided pBuffer. */
    free(response->pBuffer);
}

static void test_zero_content_length(void)
{
    const char *wire = "HTTP/1.1 200 OK\r\n"
                       "content-length: 0\r\n"
                       "content-type: application/json\r\n"
                       "server: https\r\n\r\n";
    HTTPResponse_t response;
    HTTPStatus_t status = request(wire, 1U, &response);
    assert(status == HTTPSuccess);
    assert(response.statusCode == 200U);
    assert(response.contentLength == 0U);
    assert(response.bodyLen == 0U);
    assert(response.pBody == NULL);
    free_response(&response);
}

static void test_no_content_status_without_length(void)
{
    const char *wire = "HTTP/1.1 204 No Content\r\nserver: https\r\n\r\n";
    HTTPResponse_t response;
    HTTPStatus_t status = request(wire, 3U, &response);
    assert(status == HTTPSuccess);
    assert(response.statusCode == 204U);
    assert(response.bodyLen == 0U);
    free_response(&response);
}

static void test_nonzero_content_length(void)
{
    char wire[256];
    const char *header = "HTTP/1.1 200 OK\r\ncontent-length: 121\r\n\r\n";
    size_t header_len = strlen(header);
    memcpy(wire, header, header_len);
    memset(wire + header_len, 'x', 121U);
    wire[header_len + 121U] = '\0';

    HTTPResponse_t response;
    HTTPStatus_t status = request(wire, 7U, &response);
    assert(status == HTTPSuccess);
    assert(response.statusCode == 200U);
    assert(response.contentLength == 121U);
    assert(response.bodyLen == 121U);
    assert(response.pBody != NULL);
    for (size_t i = 0U; i < 121U; ++i) {
        assert(response.pBody[i] == 'x');
    }
    free_response(&response);
}

static void test_invalid_content_length(void)
{
    const char *wire = "HTTP/1.1 200 OK\r\ncontent-length: nope\r\n\r\n";
    HTTPResponse_t response;
    HTTPStatus_t status = request(wire, 1U, &response);
    assert(status == HTTPSecurityAlertInvalidContentLength);
    assert(response.pBuffer != NULL);
    assert(response.pBody == NULL);
    free_response(&response);
}

static void test_transfer_encoding_with_content_length(void)
{
    const char *wire = "HTTP/1.1 200 OK\r\n"
                       "transfer-encoding: chunked\r\n"
                       "content-length: 5\r\n\r\n"
                       "5\r\nhello\r\n0\r\n\r\n";
    HTTPResponse_t response;
    HTTPStatus_t status = request(wire, 1U, &response);
    assert(status == HTTPSecurityAlertInvalidContentLength);
    assert(response.pBuffer != NULL);
    assert(response.pBody == NULL);
    free_response(&response);
}

int main(void)
{
    test_zero_content_length();
    test_no_content_status_without_length();
    test_nonzero_content_length();
    test_invalid_content_length();
    test_transfer_encoding_with_content_length();
    puts("http_response_content_length: all cases passed");
    return 0;
}
