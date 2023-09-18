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

#include <cfw.h>
#include <file.h>
#include <filesystem.h>
#include <menu/utils.h>
#include <thread.h>
#include <utils.h>

#include <coreinit/filesystem_fsa.h>
#include <mocha/mocha.h>

#include <romfs-wiiu.h>

#include <stdbool.h>

static FSAClientHandle handle;
static NUSDEV usb = NUSDEV_NONE;
static int64_t spaceMap[4] = { 0, 0, 0, 0 };
static OSThread *spaceThread = NULL;

static int spaceThreadMain(int argc, const char **argv)
{
    (void)argc;
    (void)argv;

    uint64_t freeSpace;
    FSAGetFreeSpaceSize(getFSAClient(), NUSDIR_USB1, &freeSpace);
    FSAGetFreeSpaceSize(getFSAClient(), NUSDIR_USB2, &freeSpace);
    FSAGetFreeSpaceSize(getFSAClient(), NUSDIR_MLC, &freeSpace);
    FSAGetFreeSpaceSize(getFSAClient(), NUSDIR_SD, &freeSpace);
    return 0;
}

void initFSSpace()
{
    spaceThread = startThread("NUSspli FS Initialiser", THREAD_PRIORITY_MEDIUM, 0x400, spaceThreadMain, 0, NULL, OS_THREAD_ATTRIB_AFFINITY_ANY);
}

bool initFS(bool validCfw)
{
#ifdef NUSSPLI_HBL
    romfsInit();
#endif

    if(FSAInit() == FS_ERROR_OK)
    {
        handle = FSAAddClient(NULL);
        if(handle)
        {
            if(!validCfw)
                return true;

            if(Mocha_UnlockFSClientEx(handle) == MOCHA_RESULT_SUCCESS)
            {
                if(dirExists(NUSDIR_USB1))
                    usb = NUSDEV_USB01;
                else if(dirExists(NUSDIR_USB2))
                    usb = NUSDEV_USB02;
                else
                    usb = NUSDEV_NONE;

                if(FSAMount(handle, "/vol/external01", "/vol/app_sd", FSA_MOUNT_FLAG_BIND_MOUNT, NULL, 0) == FS_ERROR_OK)
                {
                    if(FSAMount(handle, "/dev/slc01", "/vol/slc", FSA_MOUNT_FLAG_LOCAL_MOUNT, NULL, 0) == FS_ERROR_OK)
                        return true;

                    FSAUnmount(handle, "/vol/app_sd", FSA_UNMOUNT_FLAG_BIND_MOUNT);
                }
            }

            FSADelClient(handle);
        }

        FSAShutdown();
    }

    return false;
}

static void checkSpaceThread()
{
    if(spaceThread)
    {
        void *ovl = addErrorOverlay("Waiting for SD card...");
        stopThread(spaceThread, NULL);
        spaceThread = NULL;
        if(ovl != NULL)
            removeErrorOverlay(ovl);
    }
}

void deinitFS(bool validCfw)
{
    checkSpaceThread();

    if(validCfw)
    {
        FSAUnmount(handle, "/vol/app_sd", FSA_UNMOUNT_FLAG_BIND_MOUNT);
        FSAUnmount(handle, "/vol/slc", FSA_UNMOUNT_FLAG_NONE);
    }

    FSADelClient(handle);
    FSAShutdown();

#ifdef NUSSPLI_HBL
    romfsExit();
#endif
}

FSAClientHandle getFSAClient()
{
    return handle;
}

NUSDEV getUSB()
{
    return usb;
}

void claimSpace(NUSDEV dev, uint64_t size)
{
    spaceMap[dev] += size;
}

void freeSpace(NUSDEV dev, uint64_t size)
{
    spaceMap[dev] -= size;
}

uint64_t getFreeSpace(NUSDEV dev)
{
    checkSpaceThread();

    const char *nd = dev == NUSDEV_USB01 ? NUSDIR_USB1 : (dev == NUSDEV_USB02 ? NUSDIR_USB2 : (dev == NUSDEV_SD ? NUSDIR_SD : NUSDIR_MLC));
    uint64_t freeSpace;

    if(FSAGetFreeSpaceSize(getFSAClient(), (char *)nd, &freeSpace) != FS_ERROR_OK)
        return 0;

    freeSpace -= spaceMap[dev];
    return freeSpace;
}

bool checkFreeSpace(NUSDEV dev, uint64_t size)
{
    if(size > getFreeSpace(dev))
    {
        showNoSpaceOverlay(dev);
        return false;
    }

    return true;
}

uint64_t getSpace(NUSDEV dev)
{

    FSADeviceInfo info;
    const char *nd = dev == NUSDEV_USB01 ? NUSDIR_USB1 : (dev == NUSDEV_USB02 ? NUSDIR_USB2 : (dev == NUSDEV_SD ? NUSDIR_SD : NUSDIR_MLC));
    return FSAGetDeviceInfo(getFSAClient(), nd, &info) == FS_ERROR_OK ? info.deviceSizeInSectors * info.deviceSectorSize : 0;
}
