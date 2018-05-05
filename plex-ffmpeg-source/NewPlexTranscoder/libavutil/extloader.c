/*
 * copyright (c) 2016 Rodger Combs <rodger.combs@gmail.com>
 *
 * This file is part of FFmpeg.
 *
 * FFmpeg is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * FFmpeg is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with FFmpeg; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config.h"
#include "libavutil/avstring.h"
#include "libavutil/extlib.h"
#include "libavutil/log.h"
#include "libavutil/mem.h"
#include "libavutil/thread.h"
#include "libavutil/wchar_filename.h"
#include "libavformat/os_support.h"

#include <sys/types.h>
#include <dirent.h>

#ifdef _WIN32
#include <windows.h>
#define DL_TYPE HMODULE
DEF_FS_FUNCTION3(HMODULE, NULL, LoadLibrary, LoadLibraryW, LoadLibraryA)
#define DL_OPEN_FUNC(l) win32_LoadLibrary(l)
#define DL_LOAD_FUNC(l, s) GetProcAddress(l, s)
#define DL_CLOSE_FUNC(l) FreeLibrary(l)
static const char *win_error(char* buf, int size)
{
    DWORD err = GetLastError();
    int len = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ARGUMENT_ARRAY,
                             NULL, err, LANG_NEUTRAL,
                             buf, size, NULL);
    if (len >= 3)
        buf[len - 2] = 0;
    else
        snprintf(buf, size, "Win32 Error 0x%x", (unsigned int)err);
    return buf;
}
#define dlerror() win_error((char[1024]){0}, 1024)
struct ff_dirent {
    char *d_name;
};
typedef struct ff_DIR {
    HANDLE handle;
    WIN32_FIND_DATAW data;
    struct ff_dirent ent;
} ff_DIR;
static ff_DIR *ff_opendir(const char *name)
{
    wchar_t *wname = NULL;
    ff_DIR *dirp = av_mallocz(sizeof(*dirp));
    if (!dirp)
        return NULL;
    dirp->handle = INVALID_HANDLE_VALUE;
    (void)utf8towchar(name, &wname);
    if (wname) {
        wchar_t *wname2;
        size_t len = wcslen(wname);
        if (len > 1 && wname[len - 1] == L'\\')
            wname[--len] = 0;
        wname2 = av_malloc(len * sizeof(wchar_t) + 3);
        if (wname2) {
            memcpy(wname2, wname, sizeof(wchar_t) * len);
            wname2[len++] = L'\\';
            wname2[len++] = L'*';
            wname2[len++] = 0;
            dirp->handle = FindFirstFileW(wname2, &dirp->data);
            av_free(wname2);
        }
    }
    av_free(wname);
    if (dirp->handle == INVALID_HANDLE_VALUE)
        av_freep(&dirp);
    return dirp;
}
static struct ff_dirent *ff_readdir(ff_DIR *dirp)
{
    av_freep(&dirp->ent.d_name);
    if (!dirp->data.cFileName[0]) {
        if (!FindNextFileW(dirp->handle, &dirp->data))
            dirp->data.cFileName[0] = 0;
    }
    if (dirp->data.cFileName[0]) {
        (void)wchartoutf8(dirp->data.cFileName, &dirp->ent.d_name);
        dirp->data.cFileName[0] = 0;
    }
    return dirp->ent.d_name ? &dirp->ent : NULL;
}
static int ff_closedir(ff_DIR *dirp)
{
    FindClose(dirp->handle);
    av_freep(&dirp->ent.d_name);
    av_free(dirp);
    return 0;
}
#else
#include <dlfcn.h>
#define DL_TYPE void*
#define DL_OPEN_FUNC(l) dlopen(l, RTLD_NOW | RTLD_LOCAL)
#define DL_LOAD_FUNC(l, s) dlsym(l, s)
#define DL_CLOSE_FUNC(l) dlclose(l)
#define ff_DIR DIR
#define ff_dirent dirent
#define ff_opendir opendir
#define ff_readdir readdir
#define ff_closedir closedir
#endif

static void load_dso(FFLibrary* fflib, const char *path, int level)
{
    char *loaded = fflib->loaded_dso_list;
    char *new_loaded;
    AVInitLibrary init_library;
    DL_TYPE lib;

    // already loaded?
    if (loaded && strstr(loaded, path))
        return;

    new_loaded = av_asprintf("%s%s:", loaded ? loaded : "", path);
    if (!new_loaded)
        return;
    av_free(loaded);
    fflib->loaded_dso_list = new_loaded;

    av_log(NULL, AV_LOG_VERBOSE, "Loading external lib %s\n", path);

    lib = DL_OPEN_FUNC(path);
    if (!lib) {
        const char *err = dlerror();
        if (!err)
            err = "(unknown)";
        av_log(NULL, level, "Error loading external lib: %s\n", err);
    } else {
        init_library = (AVInitLibrary)DL_LOAD_FUNC(lib, "av_init_library");
        if (init_library) {
            if (init_library(fflib, level) < 0) {
                av_log(NULL, level, "Failed to initialize library.\n");
                DL_CLOSE_FUNC(lib);
            }
        } else {
            av_log(NULL, level, "External lib has no loading function.\n");
            DL_CLOSE_FUNC(lib);
        }
    }
}

static void load_dsos_from_directory(FFLibrary* lib, const char *path)
{
    ff_DIR *dir;
    struct ff_dirent *entry;

    dir = ff_opendir(path);
    if (!dir) {
        av_log(NULL, AV_LOG_ERROR, "Could not open directory '%s'\n", path);
        return;
    }

    while ((entry = ff_readdir(dir))) {
        char *dso = entry->d_name;
        if (!ff_strcaseendswith(dso, SLIBSUF))
            continue;

        dso = av_asprintf("%s%s", path, dso);
        if (!dso)
            continue;

        load_dso(lib, dso, AV_LOG_WARNING);

        av_free(dso);
    }

    ff_closedir(dir);
}

static AVOnce init_lib_lock = AV_ONCE_INIT;
static AVMutex lib_lock;

static void init_lib_lock_fn(void)
{
    pthread_mutex_init(&lib_lock, NULL);
}

static void ff_lock_lib(FFLibrary* lib)
{
    ff_thread_once(&init_lib_lock, init_lib_lock_fn);
    pthread_mutex_lock(&lib_lock);
}

static void ff_unlock_lib(FFLibrary* lib)
{
    pthread_mutex_unlock(&lib_lock);
}

int ff_is_master = 1;

// Goes through FFMPEG_EXTERNAL_LIBS and loads the libs there.
// Uses ff_library to add them to the internal state.
void avpriv_load_new_libs(FFLibrary* lib)
{
    char *paths_env;
    const char *paths;
    const char *cur = NULL;

    // Never load external libs from leaf libs.
    if (!ff_is_master)
        return;

    paths = paths_env = ff_getenv("FFMPEG_EXTERNAL_LIBS");
    if (!paths)
        return;

    ff_lock_lib(lib);

    av_log(NULL, AV_LOG_VERBOSE, "Rescanning for external libs: '%s'\n", paths);

    while (*paths && (cur = av_get_token(&paths, ":"))) {
        if (*paths)
            paths++;

        if (ff_strcaseendswith(cur, "/") || ff_strcaseendswith(cur, "\\")) {
            load_dsos_from_directory(lib, cur);
        } else {
            load_dso(lib, cur, AV_LOG_ERROR);
        }

        av_freep(&cur);
    }

    av_free(paths_env);

    ff_unlock_lib(lib);
}
