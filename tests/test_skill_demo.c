#include "skill/skill.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int main(void)
{
	struct skill sk;
	int rc = skill_load(&sk, "skills/demo-translate");
	if (rc < 0) {
		rc = skill_load(&sk, "../skills/demo-translate");
	}
	if (rc < 0) {
		rc = skill_load(&sk, "/Users/und/Work/c-family/morph/skills/demo-translate");
	}
	if (rc < 0) {
		printf("FAIL: skill_load failed (%d)\n", rc);
		return 1;
	}
	printf("skill loaded: name='%s' type='%s' entry='%s'\n",
	       sk.manifest.name, sk.manifest.type, sk.manifest.entry);

	char *result = NULL;
	rc = skill_run(&sk, "{\"text\":\"hello world\",\"target_lang\":\"zh\"}", &result);
	if (rc < 0) {
		printf("FAIL: skill_run failed (%d)\n", rc);
		skill_unload(&sk);
		return 1;
	}
	printf("skill result: %s\n", result ? result : "(null)");

	/* skill_run now extracts the result field from JSON-RPC response */
	int ok = (result && strstr(result, "翻译结果") != NULL);
	free(result);
	skill_unload(&sk);
	printf("%s\n", ok ? "PASS: translate skill works correctly" : "FAIL: unexpected result");
	return ok ? 0 : 1;
}
