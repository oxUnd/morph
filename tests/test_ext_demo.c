#include "ext/ext.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
	struct ext ex;
	int rc = ext_load(&ex, "exts/demo-translate");
	if (rc < 0) {
		rc = ext_load(&ex, "../exts/demo-translate");
	}
	if (rc < 0) {
		rc = ext_load(&ex, "/Users/und/Work/c-family/morph/exts/demo-translate");
	}
	if (rc < 0) {
		printf("FAIL: ext_load failed (%d)\n", rc);
		return 1;
	}
	printf("ext loaded: name='%s' type='%s' entry='%s'\n",
	       ex.manifest.name, ex.manifest.type, ex.manifest.entry);

	char *result = NULL;
	rc = ext_run(&ex, "{\"text\":\"hello world\",\"target_lang\":\"zh\"}", &result);
	if (rc < 0) {
		printf("FAIL: ext_run failed (%d)\n", rc);
		ext_unload(&ex);
		return 1;
	}
	printf("ext result: %s\n", result ? result : "(null)");

	/* ext_run now extracts the result field from JSON-RPC response */
	int ok = (result && strstr(result, "翻译结果") != NULL);
	free(result);
	ext_unload(&ex);
	printf("%s\n", ok ? "PASS: translate ext works correctly" : "FAIL: unexpected result");
	return ok ? 0 : 1;
}
