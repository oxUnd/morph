#ifndef MORPH_JSON_GENERIC_H
#define MORPH_JSON_GENERIC_H

#include "json.h"

#define JSON_NULL (json_arg_null())
/* C11 true is an int macro. Use this for a boolean literal in generic calls. */
#define JSON_BOOL(value) (json_arg_bool(value))

#ifdef __cplusplus
extern "C++" {
template <class T> struct json_value json_arg(T) = delete;
inline struct json_value json_arg(struct json_value value)
{
	return value;
}
inline struct json_value json_arg(bool value)
{
	return json_arg_bool(value);
}
#define MORPH_JSON_INTEGER_OVERLOAD(type, factory)                                                 \
	inline struct json_value json_arg(type value)                                              \
	{                                                                                          \
		return factory(value);                                                             \
	}
MORPH_JSON_INTEGER_OVERLOAD(char, json_arg_int)
MORPH_JSON_INTEGER_OVERLOAD(signed char, json_arg_int)
MORPH_JSON_INTEGER_OVERLOAD(unsigned char, json_arg_uint)
MORPH_JSON_INTEGER_OVERLOAD(short, json_arg_int)
MORPH_JSON_INTEGER_OVERLOAD(unsigned short, json_arg_uint)
MORPH_JSON_INTEGER_OVERLOAD(int, json_arg_int)
MORPH_JSON_INTEGER_OVERLOAD(unsigned int, json_arg_uint)
MORPH_JSON_INTEGER_OVERLOAD(long, json_arg_int)
MORPH_JSON_INTEGER_OVERLOAD(unsigned long, json_arg_uint)
MORPH_JSON_INTEGER_OVERLOAD(long long, json_arg_int)
MORPH_JSON_INTEGER_OVERLOAD(unsigned long long, json_arg_uint)
#undef MORPH_JSON_INTEGER_OVERLOAD
inline struct json_value json_arg(float value)
{
	return json_arg_number(value);
}
inline struct json_value json_arg(double value)
{
	return json_arg_number(value);
}
inline struct json_value json_arg(const char *value)
{
	return json_arg_string(value);
}
inline struct json_value json_arg(char *value)
{
	return json_arg_string(value);
}
inline struct json_value json_arg(const json *value)
{
	return json_arg_node(value);
}
inline struct json_value json_arg(json *value)
{
	return json_arg_node(value);
}
inline struct json_value json_arg(decltype(nullptr))
{
	return json_arg_null();
}
}
#define MORPH_JSON_ARG(value) json_arg(value)
#else
#define MORPH_JSON_ARG(value)                                                                      \
	_Generic((value),                                                                          \
		struct json_value: json_arg_value,                                                 \
		_Bool: json_arg_bool,                                                              \
		char: json_arg_int,                                                                \
		signed char: json_arg_int,                                                         \
		unsigned char: json_arg_uint,                                                      \
		short: json_arg_int,                                                               \
		unsigned short: json_arg_uint,                                                     \
		int: json_arg_int,                                                                 \
		unsigned int: json_arg_uint,                                                       \
		long: json_arg_int,                                                                \
		unsigned long: json_arg_uint,                                                      \
		long long: json_arg_int,                                                           \
		unsigned long long: json_arg_uint,                                                 \
		float: json_arg_number,                                                            \
		double: json_arg_number,                                                           \
		char *: json_arg_string,                                                           \
		const char *: json_arg_string,                                                     \
		struct json *: json_arg_node,                                                      \
		const struct json *: json_arg_node)(value)
#endif

#define json_set(root, path, value) json_set_value((root), (path), MORPH_JSON_ARG(value))
#define json_append(root, path, value) json_append_value((root), (path), MORPH_JSON_ARG(value))

#endif
