// File/layer: core/src/absolute_path.h | internal, C++17, Qt-free,
//             header-only; no platform headers
// Testing: HARNESS: J2534QueryParamTest.RelativeDllPathsAreRefused and
//          J2534SurrogateTest.SurrogateExeOverrideIsRestricted (through
//          win_utf8.h); UNIT: SimEcuTest.TheUrisKeysAndRefusals (a
//          recording's path, sim_ecu.h)
//
// Windows' rule for an absolute path, kept apart from win_utf8.h so a
// header that judges a URI's path on every platform (sim_ecu.h) doesn't
// take <windows.h> with it.

#ifndef ECUIO_ABSOLUTE_PATH_H
#define ECUIO_ABSOLUTE_PATH_H

/* True for a drive-absolute ("C:\", "C:/") or UNC ("\\") path, in
 * UTF-8 or UTF-16. A relative path, or one rooted without a drive
 * ("\dir", "C:dir"), would be resolved against the current directory or
 * the DLL search order. */
template <typename C>
inline bool ecuio_is_absolute_path(const C* p)
{
    const bool drive = ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) &&
                       p[1] == ':' && (p[2] == '\\' || p[2] == '/');
    const bool unc = (p[0] == '\\' && p[1] == '\\');
    return drive || unc;
}

#endif /* ECUIO_ABSOLUTE_PATH_H */
