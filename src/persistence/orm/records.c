#include "records.h"

static const struct norm_field integer_fields[] = {
	NORM_FIELD(struct orm_integer, value, NORM_TYPE_I64, 0),
};

const struct norm_model orm_integer_model = NORM_MODEL(struct orm_integer, "", integer_fields);

static const struct norm_field text_fields[] = {
	NORM_NULLABLE_FIELD(struct orm_text, value, missing, NORM_TYPE_TEXT, 0),
};

const struct norm_model orm_text_model = NORM_MODEL(struct orm_text, "", text_fields);
