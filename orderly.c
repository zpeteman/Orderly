// orderly.c
// Build: cl /W4 orderly.c ole32.lib shell32.lib
// Or with MinGW: gcc -municode -o orderly.exe orderly.c -lole32 -lshell32

#define UNICODE
#define _UNICODE

#include <windows.h>
#include <shlobj.h>   // SHGetKnownFolderPath
#include <stdio.h>
#include <wchar.h>
#include <string.h>
#include <stdbool.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// Helper: get known folder path (Downloads, Documents, Pictures, Videos)
bool get_known_folder(REFKNOWNFOLDERID id, wchar_t outPath[MAX_PATH]) {
    PWSTR path = NULL;
    HRESULT hr = SHGetKnownFolderPath(id, 0, NULL, &path);
    if (SUCCEEDED(hr) && path) {
        wcsncpy_s(outPath, MAX_PATH, path, _TRUNCATE);
        CoTaskMemFree(path);
        return true;
    }
    if (path) CoTaskMemFree(path);
    return false;
}

// Helper: create a directory if it doesn't exist
void ensure_dir(const wchar_t *path) {
    DWORD attr = GetFileAttributesW(path);
    if (attr == INVALID_FILE_ATTRIBUTES || !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        // try to create (ignore errors if it already exists concurrently)
        CreateDirectoryW(path, NULL);
    }
}

// Helper: lowercase extension
void to_lower_w(wchar_t *s) {
    for (; *s; ++s) *s = towlower(*s);
}

// Return category: "Documents", "Pictures", "Videos", or NULL if unknown
const wchar_t* classify_extension(const wchar_t *ext) {
    if (!ext || wcslen(ext) < 2) return NULL; // no extension or single dot

    // ext starts with '.', e.g. ".jpg"
    // convert to lowercase local buffer
    wchar_t low[64];
    wcsncpy_s(low, sizeof(low)/sizeof(low[0]), ext, _TRUNCATE);
    to_lower_w(low);

    // images
    const wchar_t* images[] = {L".jpg",L".jpeg",L".png",L".gif",L".bmp",L".webp",L".heic", NULL};
    // videos
    const wchar_t* videos[] = {L".mp4",L".mkv",L".mov",L".avi",L".webm",L".flv", NULL};
    // documents
    const wchar_t* docs[] = {L".pdf",L".doc",L".docx",L".xls",L".xlsx",L".ppt",L".pptx",L".txt",L".odt", NULL};
    // archives (treat as Documents group? we'll leave them as unknown per user request)
    // executables: leave as unknown (we won't move installers automatically)
    // choose strict classification: only images -> Pictures, videos -> Videos, docs -> Documents

    for (int i=0; images[i]; ++i) if (wcscmp(low, images[i])==0) return L"Pictures";
    for (int i=0; videos[i]; ++i) if (wcscmp(low, videos[i])==0) return L"Videos";
    for (int i=0; docs[i]; ++i) if (wcscmp(low, docs[i])==0) return L"Documents";
    return NULL;
}

// Build a path string safely
void path_join(const wchar_t *a, const wchar_t *b, wchar_t out[MAX_PATH]) {
    swprintf_s(out, MAX_PATH, L"%s\\%s", a, b);
}

// If dest exists, add " (n)" before extension
void make_unique_dest(wchar_t dest[MAX_PATH]) {
    if (GetFileAttributesW(dest) == INVALID_FILE_ATTRIBUTES) {
        // doesn't exist, ok
        return;
    }
    // split name and ext
    wchar_t base[MAX_PATH];
    wchar_t ext[MAX_PATH];
    wchar_t dir[MAX_PATH];

    // find last backslash
    wchar_t *lastSlash = wcsrchr(dest, L'\\');
    if (!lastSlash) return; // odd case
    size_t dirLen = lastSlash - dest;
    wcsncpy_s(dir, MAX_PATH, dest, dirLen);
    dir[dirLen] = 0;

    wchar_t *name = lastSlash + 1;
    wchar_t *dot = wcsrchr(name, L'.');
    if (dot) {
        wcsncpy_s(ext, MAX_PATH, dot, _TRUNCATE);
        size_t nameLen = dot - name;
        wcsncpy_s(base, MAX_PATH, name, nameLen);
        base[nameLen] = 0;
    } else {
        ext[0] = 0;
        wcsncpy_s(base, MAX_PATH, name, _TRUNCATE);
    }

    // try suffixes (1..999)
    for (int i = 1; i < 1000; ++i) {
        wchar_t candidate[MAX_PATH];
        if (ext[0])
            swprintf_s(candidate, MAX_PATH, L"%s\\%s (%d)%s", dir, base, i, ext);
        else
            swprintf_s(candidate, MAX_PATH, L"%s\\%s (%d)", dir, base, i);
        if (GetFileAttributesW(candidate) == INVALID_FILE_ATTRIBUTES) {
            // update dest
            wcsncpy_s(dest, MAX_PATH, candidate, _TRUNCATE);
            return;
        }
    }
    // if all fail, leave original (MoveFile will fail later)
}

int wmain(void) {
    // Initialize COM for SHGetKnownFolderPath
    HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        fwprintf(stderr, L"Failed to initialize COM.\n");
        return 1;
    }

    wchar_t downloads[MAX_PATH];
    wchar_t documents[MAX_PATH];
    wchar_t pictures[MAX_PATH];
    wchar_t videos[MAX_PATH];

    if (!get_known_folder(FOLDERID_Downloads, downloads)) {
        fwprintf(stderr, L"Cannot locate Downloads folder. Exiting.\n");
        CoUninitialize();
        return 1;
    }
    if (!get_known_folder(FOLDERID_Documents, documents)) {
        fwprintf(stderr, L"Cannot locate Documents folder. Exiting.\n");
        CoUninitialize();
        return 1;
    }
    if (!get_known_folder(FOLDERID_Pictures, pictures)) {
        fwprintf(stderr, L"Cannot locate Pictures folder. Exiting.\n");
        CoUninitialize();
        return 1;
    }
    if (!get_known_folder(FOLDERID_Videos, videos)) {
        fwprintf(stderr, L"Cannot locate Videos folder. Exiting.\n");
        CoUninitialize();
        return 1;
    }

    // Prepare Recent Downloads subfolders
    wchar_t docsRecent[MAX_PATH], picsRecent[MAX_PATH], vidsRecent[MAX_PATH];
    path_join(documents, L"Recent Downloads", docsRecent);
    path_join(pictures, L"Recent Downloads", picsRecent);
    path_join(videos, L"Recent Downloads", vidsRecent);

    ensure_dir(docsRecent);
    ensure_dir(picsRecent);
    ensure_dir(vidsRecent);

    // scan Downloads
    wchar_t searchPattern[MAX_PATH];
    path_join(downloads, L"*", searchPattern);

    WIN32_FIND_DATAW ffd;
    HANDLE hFind = FindFirstFileW(searchPattern, &ffd);
    if (hFind == INVALID_HANDLE_VALUE) {
        fwprintf(stderr, L"Failed to open Downloads folder or it is empty.\n");
        CoUninitialize();
        return 1;
    }

    int moved = 0;
    int skipped = 0;
    int errors = 0;

    do {
        // skip directories (including "." and "..")
        if (ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;

        // filename
        wchar_t *name = ffd.cFileName;

        // find extension
        wchar_t *dot = wcsrchr(name, L'.');
        const wchar_t *category = classify_extension(dot ? dot : L"");
        if (!category) {
            // leave unknown files in Downloads
            skipped++;
            continue;
        }

        // choose destination base
        wchar_t destDir[MAX_PATH];
        if (wcscmp(category, L"Documents") == 0) wcsncpy_s(destDir, MAX_PATH, docsRecent, _TRUNCATE);
        else if (wcscmp(category, L"Pictures") == 0) wcsncpy_s(destDir, MAX_PATH, picsRecent, _TRUNCATE);
        else if (wcscmp(category, L"Videos") == 0) wcsncpy_s(destDir, MAX_PATH, vidsRecent, _TRUNCATE);
        else { skipped++; continue; }

        // construct source and dest paths
        wchar_t src[MAX_PATH], dest[MAX_PATH];
        path_join(downloads, name, src);
        path_join(destDir, name, dest);

        // make destination unique if needed
        make_unique_dest(dest);

        // Move file (use MoveFileEx to allow overwriting? we avoid overwrite by unique naming)
        if (MoveFileW(src, dest)) {
            wprintf(L"Moved: %s -> %s\n", name, dest);
            moved++;
        } else {
            DWORD err = GetLastError();
            fwprintf(stderr, L"Error moving %s (err %u)\n", name, err);
            errors++;
        }

    } while (FindNextFileW(hFind, &ffd) != 0);

    FindClose(hFind);
    CoUninitialize();

    wprintf(L"\nDone. Moved: %d, Skipped (unknown): %d, Errors: %d\n", moved, skipped, errors);
    return 0;
}