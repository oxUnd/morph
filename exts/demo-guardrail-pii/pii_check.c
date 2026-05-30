#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static int match_pattern(const char *text, const char *pattern)
{
	return strstr(text, pattern) != NULL;
}

static int looks_like_ssn(const char *text)
{
	for (const char *p = text; *p; p++) {
		if (isdigit((unsigned char)*p)) {
			int d1 = 0, d2 = 0, d3 = 0;
			const char *s = p;
			while (s < p + 3 && isdigit((unsigned char)*s)) { d1++; s++; }
			if (d1 == 3 && *s == '-') {
				s++;
				while (s < p + 6 && isdigit((unsigned char)*s)) { d2++; s++; }
				if (d2 == 2 && *s == '-') {
					s++;
					while (s < p + 11 && isdigit((unsigned char)*s)) { d3++; s++; }
					if (d3 == 4) return 1;
				}
			}
		}
	}
	return 0;
}

static int looks_like_credit_card(const char *text)
{
	for (const char *p = text; *p; p++) {
		if (isdigit((unsigned char)*p)) {
			int count = 0;
			const char *s = p;
			while (*s && count < 16) {
				if (isdigit((unsigned char)*s)) count++;
				else if (*s != ' ' && *s != '-') break;
				s++;
			}
			if (count == 16) return 1;
		}
	}
	return 0;
}

static int looks_like_email(const char *text)
{
	const char *at = strchr(text, '@');
	if (!at) return 0;
	const char *dot = strchr(at, '.');
	return dot != NULL && dot > at + 1;
}

static int looks_like_phone(const char *text)
{
	for (const char *p = text; *p; p++) {
		if (isdigit((unsigned char)*p)) {
			int digits = 0;
			const char *s = p;
			while (*s && digits < 15) {
				if (isdigit((unsigned char)*s)) digits++;
				else if (*s != ' ' && *s != '-' && *s != '('
					 && *s != ')' && *s != '+') break;
				s++;
			}
			if (digits >= 10) return 1;
		}
	}
	return 0;
}

int guardrail_check(const char *text, const char *rule,
		    const char *description, char **result_json)
{
	(void)rule;
	(void)description;
	if (!text || !result_json) return -1;

	int passed = 1;
	const char *reason = "";

	if (looks_like_ssn(text)) {
		passed = 0;
		reason = "Input contains a Social Security Number (XXX-XX-XXXX).";
	} else if (looks_like_credit_card(text)) {
		passed = 0;
		reason = "Input contains a credit card number (16 digits).";
	} else if (looks_like_email(text)) {
		passed = 0;
		reason = "Input contains an email address.";
	} else if (looks_like_phone(text)) {
		passed = 0;
		reason = "Input contains a phone number (10+ digits).";
	} else if (match_pattern(text, "password:") ||
		   match_pattern(text, "Password:") ||
		   match_pattern(text, "secret_key") ||
		   match_pattern(text, "api_key")) {
		passed = 0;
		reason = "Input appears to contain credentials or secret keys.";
	}

	size_t rlen = strlen(reason);
	size_t bufsize = rlen + 64;
	char *buf = (char *)malloc(bufsize);
	if (!buf) return -1;

	snprintf(buf, bufsize,
		 "{\"pass\":%s,\"reason\":\"%s\"}",
		 passed ? "true" : "false", reason);
	*result_json = buf;
	return 0;
}
