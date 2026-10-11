/* SPDX-License-Identifier: Apache-2.0 */
#include "gateway_http.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <ctype.h>

int http_parse_request(char *header, struct http_request *r)
{
	memset(r, 0, sizeof(*r));
	char *line_end = strstr(header, "\r\n");
	if (!line_end) return -EINVAL;
	*line_end = 0;
	char version[16], extra;
	if (sscanf(header, "%7s %95s %15s%c", r->method, r->path, version, &extra) != 3 ||
	    strcmp(version, "HTTP/1.1") || r->path[0] != '/') return -EINVAL;
	bool has_length = false, has_auth = false, has_host = false, has_connection = false;
	char *p = line_end + 2;
	while (*p) {
		line_end = strstr(p, "\r\n");
		if (!line_end) return -EINVAL;
		if (line_end == p) return has_host ? 0 : -EINVAL;
		*line_end = 0;
		char *colon = strchr(p, ':');
		if (!colon || colon == p) return -EINVAL;
		for (char *q = p; q < colon; q++) {
			if (!isalnum((unsigned char)*q) && *q != '-') return -EINVAL;
		}
		*colon++ = 0;
		while (*colon == ' ' || *colon == '\t') colon++;
		if (!strcasecmp(p, "Transfer-Encoding")) return -EINVAL;
		if (!strcasecmp(p, "Connection")) {
			if (has_connection) return -EINVAL;
			has_connection = true;
			if (!strcasecmp(colon, "keep-alive")) r->keep_alive = true;
			else if (strcasecmp(colon, "close")) return -EINVAL;
		}
		if (!strcasecmp(p, "Host")) {
			if (has_host || !*colon) return -EINVAL;
			has_host = true;
		}
		if (!strcasecmp(p, "Content-Length")) {
			if (has_length || !*colon) return -EINVAL;
			has_length = true;
			size_t value = 0;
			for (char *q = colon; *q; q++) {
				if (*q < '0' || *q > '9' || value > 100000) return -EINVAL;
				value = value * 10 + (unsigned)(*q - '0');
			}
			r->content_length = value;
		}
		if (!strcasecmp(p, "Authorization")) {
			if (has_auth || strncmp(colon, "Bearer ", 7) || strlen(colon + 7) > 128) return -EINVAL;
			has_auth = true;
			strcpy(r->bearer, colon + 7);
		}
		p = line_end + 2;
	}
	return -EINVAL;
}
