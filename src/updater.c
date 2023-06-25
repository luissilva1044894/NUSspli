/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2020-2022 V10lator <v10lator@myway.de>                    *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 3 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, If not, see <http://www.gnu.org/licenses/>.  *
 ***************************************************************************/

#include <wut-fixups.h>

#include <config.h>
#include <deinstaller.h>
#include <downloader.h>
#include <file.h>
#include <filesystem.h>
#include <input.h>
#include <installer.h>
#include <ioQueue.h>
#include <menu/update.h>
#include <menu/utils.h>
#include <notifications.h>
#include <osdefs.h>
#include <renderer.h>
#include <state.h>
#include <utils.h>

#include <coreinit/dynload.h>
#include <coreinit/filesystem_fsa.h>
#include <coreinit/mcp.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/memory.h>
#include <coreinit/thread.h>
#include <rpxloader/rpxloader.h>

#include <ioapi.h>
#include <unzip.h>

#include <file.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <jansson.h>

#define UPDATE_CHECK_URL    NAPI_URL "s?t="
#define UPDATE_DOWNLOAD_URL "https://github.com/V10lator/NUSspli/releases/download/v"
#define UPDATE_TEMP_FOLDER  NUSDIR_SD "NUSspli_temp/"
#define UPDATE_HBL_FOLDER   NUSDIR_SD "wiiu/apps/NUSspli"
#define UPDATE_AROMA_FOLDER NUSDIR_SD "wiiu/apps/"

#ifndef NUSSPLI_LITE
#define UPDATE_AROMA_FILE "NUSspli.wuhb"
#else
#define UPDATE_AROMA_FILE "NUSspli-Lite.wuhb"
#endif

#ifdef NUSSPLI_DEBUG
#define NUSSPLI_DLVER "-DEBUG"
#else
#define NUSSPLI_DLVER ""
#endif

static void showUpdateError(const char *msg)
{
    enableShutdown();
    showErrorFrame(msg);
}

static void showUpdateErrorf(const char *msg, ...)
{
    va_list va;
    va_start(va, msg);
    char newMsg[2048];
    vsnprintf(newMsg, 2048, msg, va);
    va_end(va);
    showUpdateError(newMsg);
}

bool updateCheck()
{
    if(!updateCheckEnabled())
        return false;

    RAMBUF *rambuf = allocRamBuf();
    if(rambuf == NULL)
        return false;

    const char *updateChkUrl =
#ifdef NUSSPLI_HBL
        UPDATE_CHECK_URL "h";
#else
#ifdef NUSSPLI_LITE
        UPDATE_CHECK_URL "l";
#else
        !isChannel() && isAroma() ? UPDATE_CHECK_URL "a" : UPDATE_CHECK_URL "c";
#endif
#endif

    bool ret = false;
    if(downloadFile(updateChkUrl, "JSON", NULL, FILE_TYPE_JSON | FILE_TYPE_TORAM, false, NULL, rambuf) == 0)
    {
        startNewFrame();
        textToFrame(0, 0, localise("Parsing JSON"));
        writeScreenLog(1);
        drawFrame();
        showFrame();

        json_t *json = json_loadb(rambuf->buf, rambuf->size, 0, NULL);
        if(json != NULL)
        {
            json_t *jsonObj = json_object_get(json, "s");
            if(jsonObj != NULL && json_is_integer(jsonObj))
            {
                switch(json_integer_value(jsonObj))
                {
                    case 0:
                        debugPrintf("Newest version!");
                        break;
                    case 1: // Update
                        const char *newVer = json_string_value(json_object_get(json, "v"));
                        ret = newVer != NULL;
                        if(ret)
#ifdef NUSSPLI_HBL
                            ret = updateMenu(newVer, NUSSPLI_TYPE_HBL);
#else
                            ret = updateMenu(newVer, isAroma() ? NUSSPLI_TYPE_AROMA : NUSSPLI_TYPE_CHANNEL);
#endif
                        break;
                    case 2: // Type deprecated, update to what the server suggests
                        const char *nv = json_string_value(json_object_get(json, "v"));
                        ret = nv != NULL;
                        if(ret)
                        {
                            int t = json_integer_value(json_object_get(json, "t"));
                            if(t)
                                ret = updateMenu(nv, t);
                            else
                                ret = false;
                        }
                        break;
                    case 3: // TODO
                    case 4:
                        showUpdateError(localise("Internal server error!"));
                        break;
                    default: // TODO
                        showUpdateErrorf("%s: %d", localise("Invalid state value"), json_integer_value(jsonObj));
                        break;
                }
            }
            else
                debugPrintf("Invalid JSON data");

            json_decref(json);
        }
        else
            debugPrintf("Invalid JSON data");
    }
    else
        debugPrintf("Error downloading %s", updateChkUrl);

    freeRamBuf(rambuf);
    return ret;
}

typedef struct
{
    RAMBUF *rambuf;
    long index;
} ZIP_META;

static voidpf ZCALLBACK nus_zopen(voidpf opaque, const char *filename, int mode)
{
    // STUB
    (void)opaque;
    (void)mode;
    return (voidpf)filename;
}

static uLong ZCALLBACK nus_zread(voidpf opaque, voidpf stream, void *buf, uLong size)
{
    (void)opaque;
    ZIP_META *meta = (ZIP_META *)stream;
    OSBlockMove(buf, meta->rambuf->buf + meta->index, size, false);
    meta->index += size;
    return size;
}

static uLong ZCALLBACK nus_zwrite(voidpf opaque, voidpf stream, const void *buf, uLong size)
{
    // STUB
    (void)opaque;
    (void)stream;
    (void)buf;
    return size;
}

static long ZCALLBACK nus_ztell(voidpf opaque, voidpf stream)
{
    (void)opaque;
    return ((ZIP_META *)stream)->index;
}

static long ZCALLBACK nus_zseek(voidpf opaque, voidpf stream, uLong offset, int origin)
{
    (void)opaque;
    ZIP_META *meta = (ZIP_META *)stream;
    switch(origin)
    {
        case ZLIB_FILEFUNC_SEEK_CUR:
            meta->index += offset;
            break;
        case ZLIB_FILEFUNC_SEEK_END:
            meta->index = meta->rambuf->size + offset;
            break;
        case ZLIB_FILEFUNC_SEEK_SET:
            meta->index = offset;
            break;
    }

    return 0;
}

static int ZCALLBACK nus_zstub(voidpf opaque, voidpf stream)
{
    // STUB
    (void)opaque;
    (void)stream;
    return 0;
}

static bool unzipUpdate(RAMBUF *rambuf)
{
    ZIP_META meta = { .rambuf = rambuf, .index = 0 };
    zlib_filefunc_def rbfd = {
        .zopen_file = nus_zopen,
        .zread_file = nus_zread,
        .zwrite_file = nus_zwrite,
        .ztell_file = nus_ztell,
        .zseek_file = nus_zseek,
        .zclose_file = nus_zstub,
        .zerror_file = nus_zstub,
        .opaque = NULL,
    };
    bool ret = false;
    unzFile zip = unzOpen2((const char *)&meta, &rbfd);
    if(zip != NULL)
    {
        unz_global_info zipInfo;
        if(unzGetGlobalInfo(zip, &zipInfo) == UNZ_OK)
        {
            uint8_t *buf = MEMAllocFromDefaultHeap(IO_BUFSIZE);
            if(buf != NULL)
            {
                unz_file_info zipFileInfo;
                char *zipFileName = getStaticPathBuffer(1);
                char *path = getStaticPathBuffer(2);
                char *fileName = getStaticPathBuffer(3);
                strcpy(fileName, UPDATE_TEMP_FOLDER);
                char *fnp = fileName + strlen(UPDATE_TEMP_FOLDER);
                char *needle;
                char *lastSlash;
                char *lspp;
                FSAFileHandle file;
                int extracted;
                ret = true;

                do
                {
                    if(unzGetCurrentFileInfo(zip, &zipFileInfo, zipFileName, FS_MAX_PATH - strlen(UPDATE_TEMP_FOLDER) - 1, NULL, 0, NULL, 0) == UNZ_OK)
                    {
                        if(unzOpenCurrentFile(zip) == UNZ_OK)
                        {
                            needle = strchr(zipFileName, '/');
                            if(needle != NULL)
                            {
                                lastSlash = needle;
                                lspp = needle + 1;
                                needle = strchr(lspp, '/');
                                while(needle != NULL)
                                {
                                    lastSlash = needle;
                                    lspp = needle + 1;
                                    needle = strchr(lspp, '/');
                                }

                                if(lastSlash[1] == '\0')
                                {
                                    unzCloseCurrentFile(zip);
                                    continue;
                                }

                                lastSlash[0] = '\0';
                                strcpy(path, zipFileName);
                                strcat(path, "/");
                                strcpy(fnp, path);
                                strcpy(zipFileName, lastSlash + 1);

                                if(!createDirRecursive(fileName))
                                {
                                    showUpdateErrorf("%s: %s", localise("Error creating directory"), prettyDir(fileName));
                                    ret = false;
                                }
                            }
                            else
                                path[0] = '\0';

                            if(ret)
                            {
                                sprintf(fnp, "%s%s", path, zipFileName);
                                file = openFile(fileName, "w", 0);
                                if(file != 0)
                                {
                                    while(ret)
                                    {
                                        extracted = unzReadCurrentFile(zip, buf, IO_BUFSIZE);
                                        if(extracted < 0)
                                        {
                                            showUpdateErrorf("%s: %s", localise("Error extracting file"), prettyDir(fileName));
                                            ret = false;
                                            break;
                                        }

                                        if(extracted != 0)
                                        {
                                            if(addToIOQueue(buf, 1, extracted, file) != (size_t)extracted)
                                            {
                                                showUpdateErrorf("%s: %s", localise("Error writing file"), prettyDir(fileName));
                                                ret = false;
                                                break;
                                            }
                                        }
                                        else
                                            break;
                                    }

                                    addToIOQueue(NULL, 0, 0, file);
                                }
                                else
                                {
                                    showUpdateErrorf("%s: %s", localise("Error opening file"), prettyDir(fileName));
                                    ret = false;
                                }
                            }

                            unzCloseCurrentFile(zip);
                        }
                        else
                        {
                            showUpdateError(localise("Error opening zip file"));
                            ret = false;
                        }
                    }
                    else
                    {
                        showUpdateError(localise("Error extracting zip"));
                        ret = false;
                    }
                } while(ret && unzGoToNextFile(zip) == UNZ_OK);

                MEMFreeToDefaultHeap(buf);
            }
        }
        else
            showUpdateError(localise("Error getting zip info"));

        unzClose(zip);
    }
    else
        showUpdateError(localise("Error opening zip!"));

    return ret;
}

static inline void showUpdateFrame()
{
    startNewFrame();
    textToFrame(0, 0, localise("Updating, please wait..."));
    writeScreenLog(1);
    drawFrame();
    showFrame();
}

void update(const char *newVersion, NUSSPLI_TYPE type)
{
    RAMBUF *rambuf = allocRamBuf();
    if(rambuf == NULL)
        return;

    disableShutdown();
    showUpdateFrame();
    removeDirectory(UPDATE_TEMP_FOLDER);
    FSError err = createDirectory(UPDATE_TEMP_FOLDER);
    if(err != FS_ERROR_OK)
    {
        char *toScreen = getToFrameBuffer();
        strcpy(toScreen, localise("Error creating temporary directory!"));
        strcat(toScreen, "\n\n");
        strcat(toScreen, translateFSErr(err));

        showUpdateError(toScreen);
        goto updateError;
    }

    char *path = getStaticPathBuffer(2);
    strcpy(path, UPDATE_DOWNLOAD_URL);
    strcpy(path + strlen(UPDATE_DOWNLOAD_URL), newVersion);
    strcat(path, "/NUSspli-");
    strcat(path, newVersion);

    switch(type)
    {
        case NUSSPLI_TYPE_AROMA:
#ifndef NUSSPLI_LITE
            strcat(path, "-Aroma");
#else
            strcat(path, "-Lite");
#endif
            break;
        case NUSSPLI_TYPE_CHANNEL:
            strcat(path, "-Channel");
            break;
        case NUSSPLI_TYPE_HBL:
            strcat(path, "-HBL");
            break;
        default:
            showUpdateError("Internal error!");
            goto updateError;
    }

    strcat(path, NUSSPLI_DLVER ".zip");

    if(downloadFile(path, "NUSspli.zip", NULL, FILE_TYPE_JSON | FILE_TYPE_TORAM, false, NULL, rambuf) != 0)
    {
        showUpdateErrorf("%s %s", localise("Error downloading"), path);
        goto updateError;
    }

    showUpdateFrame();

    if(!unzipUpdate(rambuf))
        goto updateError;

    bool toUSB = getUSB() != NUSDEV_NONE;

    // Uninstall currently running type/version
#ifdef NUSSPLI_HBL
    err = removeDirectory(UPDATE_HBL_FOLDER);
    if(err != FS_ERROR_OK)
    {
        showUpdateErrorf("%s: %s", localise("Error removing directory"), translateFSErr(err));
        goto updateError;
    }
#else
    if(isChannel())
    {
        MCPTitleListType ownInfo __attribute__((__aligned__(0x40)));
        MCPError e = MCP_GetOwnTitleInfo(mcpHandle, &ownInfo);
        if(e != 0)
        {
            showUpdateErrorf("%s: %#010x", localise("Error getting own title info"), e);
            goto updateError;
        }

        if(toUSB)
            toUSB = ownInfo.indexedDevice[0] == 'u';

        deinstall(&ownInfo, "NUSspli v" NUSSPLI_VERSION, true, false);
    }
    else if(isAroma())
    {
        RPXLoaderStatus rs = RPXLoader_GetPathOfRunningExecutable(path + strlen(NUSDIR_SD), FS_MAX_PATH - strlen(NUSDIR_SD));
        if(rs == RPX_LOADER_RESULT_SUCCESS)
        {
            rs = RPXLoader_UnmountCurrentRunningBundle();
            if(rs == RPX_LOADER_RESULT_SUCCESS)
            {
                OSBlockMove(path, NUSDIR_SD, strlen(NUSDIR_SD), false);
                err = FSARemove(getFSAClient(), path);
                if(err != FS_ERROR_OK)
                {
                    showUpdateErrorf("%s: %s", localise("Error removing file"), translateFSErr(err));
                    goto updateError;
                }
            }
            else
                goto aromaError;
        }
        else
        {
        aromaError:
            showUpdateErrorf("%s: %s", localise("Aroma error"), RPXLoader_GetStatusStr(rs));
            goto updateError;
        }
    }
#endif

    // Install new type/version
    flushIOQueue();
    switch(type)
    {
        case NUSSPLI_TYPE_AROMA:
            char *path2 = getStaticPathBuffer(0);
            strcpy(path2, UPDATE_TEMP_FOLDER UPDATE_AROMA_FILE);

            if(isChannel() || !isAroma())
                strcpy(path, UPDATE_AROMA_FOLDER UPDATE_AROMA_FILE);

            err = FSARename(getFSAClient(), path2, path);
            if(err != FS_ERROR_OK)
            {
                showUpdateErrorf("%s: %s", localise("Error moving file"), translateFSErr(err));
                goto updateError;
            }
            break;
        case NUSSPLI_TYPE_CHANNEL:
            strcpy(path, UPDATE_TEMP_FOLDER);
            strcpy(path + strlen(UPDATE_TEMP_FOLDER), "NUSspli");

            install("Update", false, NUSDEV_SD, path, toUSB, true, NULL);
            break;
        case NUSSPLI_TYPE_HBL:
            err = moveDirectory(UPDATE_TEMP_FOLDER "NUSspli", UPDATE_HBL_FOLDER);
            if(err != FS_ERROR_OK)
            {
                showUpdateErrorf("%s: %s", localise("Error moving directory"), translateFSErr(err));
                goto updateError;
            }
            break;
    }

    freeRamBuf(rambuf);
    removeDirectory(UPDATE_TEMP_FOLDER);
    enableShutdown();
    showFinishedScreen("Update", FINISHING_OPERATION_INSTALL);
    return;

updateError:
    freeRamBuf(rambuf);
    removeDirectory(UPDATE_TEMP_FOLDER);
    enableShutdown();
}
