#ifndef MORPH_FCGI_PRESENTATION_MEDIA_REFS_H
#define MORPH_FCGI_PRESENTATION_MEDIA_REFS_H

struct fcgi_turn_job;
struct fcgi_turn_artifact;
struct fcgi_turn_artifact *fcgi_media_artifact_borrow(struct fcgi_turn_job *, const char *path,
						      const char *kind);
char *fcgi_media_refs_render_copy(struct fcgi_turn_job *, const char *text);
char *fcgi_media_refs_summary_copy(struct fcgi_turn_job *);
char *fcgi_media_refs_store_copy(const char *text, void *data);

#endif
