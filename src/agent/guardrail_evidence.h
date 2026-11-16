#ifndef MORPH_GUARDRAIL_EVIDENCE_H
#define MORPH_GUARDRAIL_EVIDENCE_H

#include "guardrail.h"
#include "tool_artifact.h"
#include <nb/buf.h>
#include <nb/file.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Borrow the matching observation; legacy traces without IDs retain FIFO pairing. */
const struct react_step *guardrail_observation_borrow(const struct react_step *action,
						      const struct react_step **legacy_cursor);

enum guardrail_artifact_verification {
	GUARDRAIL_ARTIFACT_REMOTE = 1,
	GUARDRAIL_ARTIFACT_FORMAT_UNVERIFIED = 2,
};

/* Positive results identify remote or unsupported-format data that remains unverified. */
int guardrail_artifact_inspect(const struct tool_artifact *artifact, const char *workdir,
			       nb_file_info *info);
int guardrail_local_path_copy(const char *path, const char *workdir, nb_buf *out);

/* Caller owns the evidence object. Text excerpts and omitted calls are marked explicitly. */
cJSON *guardrail_evidence_create(const struct guardrail_eval_ctx *ctx);

#ifdef __cplusplus
}
#endif

#endif
