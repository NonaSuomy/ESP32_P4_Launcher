#pragma once
#include "papp/papp.h"
/* CODC decoder tests on the media in /sd/codec_test (dec_tests.c) */
void dec_tests(const papp_if_codc_t *c, papp_surface_t *s, int *fails, int *tests);
