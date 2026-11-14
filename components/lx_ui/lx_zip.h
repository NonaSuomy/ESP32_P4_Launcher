#pragma once
#include <stdbool.h>
#include <stddef.h>
int lx_zip_extract_first(const char *archive, const char *extensions,
                         volatile bool *abort, char *out_path, size_t out_size);
int lx_zip_resolve_first(const char *archive, const char *extensions,
                         char *out_path, size_t out_size);
