/* SPDX-License-Identifier: Apache-2.0 */
/* Host test: cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
 * -Ifirmware/nu87_lcd_gateway/include \
 * firmware/nu87_lcd_gateway/tests/test_http_parse.c \
 * firmware/nu87_lcd_gateway/src/http_parse.c -o /tmp/test_http_parse && /tmp/test_http_parse
 */
#include "gateway_http.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
static void check(int condition, const char *name)
{
	checks++;
	if (!condition) { fprintf(stderr, "FAIL: %s\n", name); failures++; }
}
static int parse(const char *wire, struct http_request *request)
{
	char header[HTTP_HEADER_CAPACITY];
	size_t length = strlen(wire);
	if (length >= sizeof(header)) { fprintf(stderr, "oversize test fixture\n"); abort(); }
	memcpy(header, wire, length + 1);
	return http_parse_request(header, request);
}
static void invalid_cases(void)
{
	static const struct { const char *name, *wire; } cases[] = {
		{"empty", ""},
		{"missing CRLF", "GET /v1/status HTTP/1.1"},
		{"LF only", "GET /v1/status HTTP/1.1\nHost: x\n\n"},
		{"missing host", "GET /v1/status HTTP/1.1\r\n\r\n"},
		{"empty host", "GET /v1/status HTTP/1.1\r\nHost: \t\r\n\r\n"},
		{"unterminated headers", "GET /v1/status HTTP/1.1\r\nHost: x\r\n"},
		{"HTTP 1.0", "GET /v1/status HTTP/1.0\r\nHost: x\r\n\r\n"},
		{"HTTP 2", "GET /v1/status HTTP/2.0\r\nHost: x\r\n\r\n"},
		{"absolute URI", "GET http://x/v1/status HTTP/1.1\r\nHost: x\r\n\r\n"},
		{"missing method", "/v1/status HTTP/1.1\r\nHost: x\r\n\r\n"},
		{"extra request field", "GET /v1/status HTTP/1.1 extra\r\nHost: x\r\n\r\n"},
		{"method overflow", "TOOLONGG /v1/status HTTP/1.1\r\nHost: x\r\n\r\n"},
		{"missing colon", "GET /v1/status HTTP/1.1\r\nHost x\r\n\r\n"},
		{"empty field name", "GET /v1/status HTTP/1.1\r\nHost: x\r\n: value\r\n\r\n"},
		{"field name space", "GET /v1/status HTTP/1.1\r\nHost: x\r\nContent-Length : 4\r\n\r\n"},
		{"obs fold", "GET /v1/status HTTP/1.1\r\nHost: x\r\n folded: value\r\n\r\n"},
		{"duplicate host", "GET /v1/status HTTP/1.1\r\nHost: x\r\nhOsT: x\r\n\r\n"},
		{"duplicate equal length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\ncontent-length: 4\r\n\r\n"},
		{"duplicate unequal length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nContent-Length: 80\r\n\r\n"},
		{"empty length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length:\r\n\r\n"},
		{"signed length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: +4\r\n\r\n"},
		{"negative length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: -1\r\n\r\n"},
		{"hex length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 0x14\r\n\r\n"},
		{"comma lengths", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 4,4\r\n\r\n"},
		{"length overflow", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 18446744073709551616\r\n\r\n"},
		{"length whitespace suffix", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 4 \r\n\r\n"},
		{"chunked transfer", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n"},
		{"identity transfer", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\ntRaNsFeR-EnCoDiNg: identity\r\n\r\n"},
		{"transfer plus length", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nContent-Length: 4\r\nTransfer-Encoding: chunked\r\n\r\n"},
		{"duplicate auth", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer a\r\nauthorization: Bearer b\r\n\r\n"},
		{"basic auth", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nAuthorization: Basic abc\r\n\r\n"},
		{"malformed bearer separator", "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer\tabc\r\n\r\n"},
	};
	struct http_request request;
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		check(parse(cases[i].wire, &request) < 0, cases[i].name);
	}
}
int main(void)
{
	struct http_request request;
	check(parse("GET /v1/status HTTP/1.1\r\nHost: example\r\n\r\n", &request) == 0, "valid status");
	check(!strcmp(request.method, "GET") && !strcmp(request.path, "/v1/status") &&
	      request.content_length == 0 && request.bearer[0] == 0, "status fields initialized");
	check(parse("POST /v1/lcd HTTP/1.1\r\nhOsT:\texample\r\nContent-Length: 20\r\nAuthorization: Bearer test-token\r\nConnection: close\r\n\r\n", &request) == 0,
	      "valid lcd request");
	check(request.content_length == 20 && !strcmp(request.bearer, "test-token") &&
	      !strcmp(request.method, "POST") && !strcmp(request.path, "/v1/lcd"), "lcd fields intact");
	check(parse("POST /v1/ota HTTP/1.1\r\nHost: example\r\nContent-Length: 80\r\n\r\n", &request) == 0 &&
	      request.content_length == OTA_DESCRIPTOR_SIZE, "OTA descriptor size");
	check(parse("POST /v1/cancel HTTP/1.1\r\nHost: example\r\nContent-Length: 0\r\n\r\n", &request) == 0 &&
	      request.content_length == 0, "zero-length request");
	char path[97], token[130], header[512];
	memset(path, 'p', sizeof(path)); path[0] = '/'; path[95] = 0;
	snprintf(header, sizeof(header), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
	check(parse(header, &request) == 0 && strlen(request.path) == 95, "path bound accepted intact");
	path[95] = 'p'; path[96] = 0;
	snprintf(header, sizeof(header), "GET %s HTTP/1.1\r\nHost: x\r\n\r\n", path);
	check(parse(header, &request) < 0, "overlong path rejected, not truncated");
	memset(token, 't', sizeof(token)); token[128] = 0;
	snprintf(header, sizeof(header), "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\n\r\n", token);
	check(parse(header, &request) == 0 && strlen(request.bearer) == 128, "token bound accepted intact");
	token[128] = 't'; token[129] = 0;
	snprintf(header, sizeof(header), "POST /v1/lcd HTTP/1.1\r\nHost: x\r\nAuthorization: Bearer %s\r\n\r\n", token);
	check(parse(header, &request) < 0, "overlong bearer rejected, not truncated");
	invalid_cases();
	printf("HTTP parser: %u checks, %u failures\n", checks, failures);
	return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
