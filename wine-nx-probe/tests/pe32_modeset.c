/* Display mode changes the way DXVK's d3d9 makes them for a fullscreen device,
 * each step reported to autorun_runtime.log as a [MODESET] line.
 *
 * Guitar Hero III asks for 1024x768@60 fullscreen and DXVK's
 * EnterFullscreenMode fails to change the display mode, which ends the game.
 * DXVK takes the window's monitor, reads its device name with GetMonitorInfoW
 * and calls ChangeDisplaySettingsExW(name, mode, NULL, CDS_FULLSCREEN, NULL),
 * then again without the refresh rate. This lists what that path sees -- the
 * monitors, the device name, every mode the device offers, the current one --
 * and makes the same calls, plus CDS_TEST and the NULL-device form, for
 * 1024x768 and for the screen's own 1280x720. The display is put back with
 * ChangeDisplaySettingsExW(NULL, NULL) at the end. */
#include <windows.h>
#include <winternl.h>

__declspec(dllimport) NTSTATUS NTAPI NtDisplayString( const UNICODE_STRING *str );

/* Built without a C runtime; the compiler still expects these for structures. */
void *memset( void *dst, int c, size_t n )
{
    unsigned char *p = dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}

static void append( WCHAR *out, unsigned int *n, const char *text )
{
    while (*text && *n < 380) out[(*n)++] = (unsigned char)*text++;
}

static void append_wide( WCHAR *out, unsigned int *n, const WCHAR *text )
{
    while (text && *text && *n < 380) out[(*n)++] = *text++;
}

static void append_number( WCHAR *out, unsigned int *n, long value )
{
    char digits[12];
    unsigned long v = value < 0 ? -(unsigned long)value : (unsigned long)value;
    int i = 0;

    if (value < 0) append( out, n, "-" );
    do digits[i++] = '0' + v % 10; while ((v /= 10) && i < 11);
    while (i && *n < 380) out[(*n)++] = digits[--i];
}

static void flush( WCHAR *out, unsigned int n )
{
    UNICODE_STRING str;

    str.Buffer = out;
    str.Length = (USHORT)(n * sizeof(WCHAR));
    str.MaximumLength = str.Length;
    NtDisplayString( &str );
}

/* "[MODESET] <what> <name>: <number>" */
static void report( const char *what, const WCHAR *name, long value )
{
    WCHAR line[400];
    unsigned int n = 0;

    append( line, &n, "[MODESET] " );
    append( line, &n, what );
    if (name) { append( line, &n, " " ); append_wide( line, &n, name ); }
    append( line, &n, ": " );
    append_number( line, &n, value );
    flush( line, n );
}

static void report_mode( const char *what, const DEVMODEW *mode )
{
    WCHAR line[400];
    unsigned int n = 0;

    append( line, &n, "[MODESET] " );
    append( line, &n, what );
    append( line, &n, " " );
    append_number( line, &n, mode->dmPelsWidth );
    append( line, &n, "x" );
    append_number( line, &n, mode->dmPelsHeight );
    append( line, &n, "x" );
    append_number( line, &n, mode->dmBitsPerPel );
    append( line, &n, "@" );
    append_number( line, &n, mode->dmDisplayFrequency );
    append( line, &n, " fields=" );
    append_number( line, &n, mode->dmFields );
    append( line, &n, " flags=" );
    append_number( line, &n, mode->dmDisplayFlags );
    append( line, &n, " at " );
    append_number( line, &n, mode->dmPosition.x );
    append( line, &n, "," );
    append_number( line, &n, mode->dmPosition.y );
    flush( line, n );
}

static BOOL CALLBACK count_monitor( HMONITOR monitor, HDC hdc, RECT *rect, LPARAM param )
{
    MONITORINFOEXW info;

    (void)hdc;
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW( monitor, (MONITORINFO *)&info ))
    {
        report( "monitor", info.szDevice, (long)(ULONG_PTR)monitor );
        report( "  width", NULL, rect->right - rect->left );
        report( "  height", NULL, rect->bottom - rect->top );
        report( "  primary", NULL, !!(info.dwFlags & MONITORINFOF_PRIMARY) );
    }
    else report( "monitor, GetMonitorInfoW failed, error", NULL, GetLastError() );
    (*(int *)param)++;
    return TRUE;
}

/* What DXVK's setWindowMode does, then the variants. */
static void try_mode( const WCHAR *device, DWORD width, DWORD height )
{
    DEVMODEW mode;
    LONG status;

    memset( &mode, 0, sizeof(mode) );
    mode.dmSize = sizeof(mode);
    mode.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL | DM_DISPLAYFREQUENCY;
    mode.dmPelsWidth = width;
    mode.dmPelsHeight = height;
    mode.dmBitsPerPel = 32;
    mode.dmDisplayFrequency = 60;
    report_mode( "ask", &mode );

    status = ChangeDisplaySettingsExW( device, &mode, NULL, CDS_TEST, NULL );
    report( "  CDS_TEST on", device, status );
    status = ChangeDisplaySettingsExW( device, &mode, NULL, CDS_FULLSCREEN, NULL );
    report( "  CDS_FULLSCREEN with refresh on", device, status );
    mode.dmFields &= ~DM_DISPLAYFREQUENCY;
    status = ChangeDisplaySettingsExW( device, &mode, NULL, CDS_FULLSCREEN, NULL );
    report( "  CDS_FULLSCREEN without refresh on", device, status );
    status = ChangeDisplaySettingsExW( NULL, &mode, NULL, CDS_FULLSCREEN, NULL );
    report( "  CDS_FULLSCREEN on the NULL device", NULL, status );

    memset( &mode, 0, sizeof(mode) );
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW( device, ENUM_CURRENT_SETTINGS, &mode )) report_mode( "  now", &mode );
    else report( "  now, EnumDisplaySettingsW failed, error", NULL, GetLastError() );
    status = ChangeDisplaySettingsExW( NULL, NULL, NULL, 0, NULL );
    report( "  restored", NULL, status );
}

void __stdcall start( void )
{
    MONITORINFOEXW info;
    HMONITOR monitor;
    DEVMODEW mode;
    POINT origin = { 0, 0 };
    int monitors = 0;
    DWORD i;

    report( "start, 0 means DISP_CHANGE_SUCCESSFUL, -2 BADMODE, -5 BADPARAM, -1 RESTART, -3 NOTUPDATED", NULL, 0 );
    EnumDisplayMonitors( NULL, NULL, count_monitor, (LPARAM)&monitors );
    report( "monitors", NULL, monitors );

    /* DXVK's monitor for a window on the primary display. */
    monitor = MonitorFromPoint( origin, MONITOR_DEFAULTTOPRIMARY );
    info.cbSize = sizeof(info);
    info.szDevice[0] = 0;
    if (!GetMonitorInfoW( monitor, (MONITORINFO *)&info ))
        report( "primary, GetMonitorInfoW failed, error", NULL, GetLastError() );
    report( "primary device", info.szDevice, (long)(ULONG_PTR)monitor );

    memset( &mode, 0, sizeof(mode) );
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW( info.szDevice, ENUM_CURRENT_SETTINGS, &mode )) report_mode( "current", &mode );
    else report( "current, EnumDisplaySettingsW failed, error", NULL, GetLastError() );
    for (i = 0; i < 64; i++)
    {
        memset( &mode, 0, sizeof(mode) );
        mode.dmSize = sizeof(mode);
        if (!EnumDisplaySettingsW( info.szDevice, i, &mode )) break;
        report_mode( "mode", &mode );
    }
    report( "modes", NULL, i );

    try_mode( info.szDevice, 1024, 768 );
    try_mode( info.szDevice, 1280, 720 );
    report( "done", NULL, 0 );
    ExitProcess( 0 );
}
