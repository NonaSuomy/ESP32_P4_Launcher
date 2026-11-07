/*
 * The Store: a launcher screen (the first tile on the home screen) that finds,
 * installs, updates and removes apps. Runs on the home task.
 */
#pragma once
#include <stdbool.h>

void store_build(void);                 /* once, after the home screen */
void store_open(const char *id);        /* the front page, or (id) that app's page */
void store_tick(void);                  /* from the home task loop */
bool store_back(void);                  /* B / Esc: true when a Store screen took it */
bool store_focus(void);                 /* on a Store screen: its focus list is set again */
bool store_installing(void);            /* an app is being written to the card */
bool store_take_changed(void);          /* apps were installed or removed since the last call */
int store_updates(void);                /* installed apps with a newer version online */
