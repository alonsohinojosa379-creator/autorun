#ifndef LAUNCHER_DLLS_H
#define LAUNCHER_DLLS_H

#include <stddef.h>

/* The Windows DLLs on the card, from the DLL repository (horizon_dlls.h): a
 * check when the launcher starts, a screen to update and verify them, and the
 * question before a game starts on a card that has none. */
struct ui;
struct launcher_dlls;

/* Starts checking the repository in the background. */
struct launcher_dlls *launcher_dlls_create( struct ui *ui, const char *runtime_dir );
void launcher_dlls_tick( struct launcher_dlls *dlls );
void launcher_dlls_open( struct launcher_dlls *dlls );
/* 1 when the card can start a program; offers to download the DLLs first
 * when it cannot, and says whether that worked. */
int  launcher_dlls_ready( struct launcher_dlls *dlls );
/* What Settings shows beside the row: tone is enum ui_value_tone. */
const char *launcher_dlls_status( struct launcher_dlls *dlls, char *buffer, size_t size, int *tone );
void launcher_dlls_destroy( struct launcher_dlls *dlls );

#endif
