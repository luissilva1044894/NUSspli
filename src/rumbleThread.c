/***************************************************************************
 * This file is part of NUSspli.                                           *
 * Copyright (c) 2020-2022 V10lator <v10lator@myway.de>                    *
 *                                                                         *
 * This program is free software; you can redistribute it and/or modify    *
 * it under the terms of the GNU General Public License as published by    *
 * the Free Software Foundation; either version 2 of the License, or       *
 * (at your option) any later version.                                     *
 *                                                                         *
 * This program is distributed in the hope that it will be useful,         *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of          *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the           *
 * GNU General Public License for more details.                            *
 *                                                                         *
 * You should have received a copy of the GNU General Public License along *
 * with this program; if not, write to the Free Software Foundation, Inc., *
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.             *
 ***************************************************************************/

#include <wut-fixups.h>

#include <stdlib.h>

#include <coreinit/memdefaultheap.h>
#include <coreinit/messagequeue.h>
#include <coreinit/thread.h>
#include <nn/acp/drcled_c.h>
#include <padscore/wpad.h>
#include <vpad/input.h>

#include <messages.h>
#include <osdefs.h>
#include <thread.h>
#include <utils.h>

#define RUMBLE_STACK_SIZE 0x400
#define RUMBLE_QUEUE_SIZE 2

static OSThread *rumbleThread;
static const uint8_t pattern[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static OSMessageQueue rumble_queue;
static OSMessage rumble_msg[2];
static uint32_t pId;

static int rumbleThreadMain(int argc, const char **argv)
{
	int i;
	OSMessage msg;

	do
	{
		OSReceiveMessage(&rumble_queue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
		if(msg.message == NUSSPLI_MESSAGE_NONE)
		{
			i = 3;
			for(; i > 1; i--)
				VPADControlMotor(VPAD_CHAN_0, (uint8_t *)pattern, 120);

			for(; i > -1; i--)
			{
				for(WPADChan j = 0; j < 4; ++j)
					WPADControlMotor(j, i);

				OSSleepTicks(OSSecondsToTicks(i));
			}

			VPADStopMotor(VPAD_CHAN_0);
		}
	}
	while(msg.message != NUSSPLI_MESSAGE_EXIT);

	return 0;
}

bool initRumble()
{
	pId = GetPersistentId();
    OSInitMessageQueueEx(&rumble_queue, rumble_msg, RUMBLE_QUEUE_SIZE, "NUSspli rumble queue");
    rumbleThread = startThread("NUSspli Rumble", THREAD_PRIORITY_LOW, RUMBLE_STACK_SIZE, rumbleThreadMain, 0, NULL, OS_THREAD_ATTRIB_AFFINITY_ANY);
    return rumbleThread != NULL;
}

void deinitRumble()
{
	if(rumbleThread != NULL)
	{
		OSMessage msg = { . message = NUSSPLI_MESSAGE_EXIT };
		OSSendMessage(&rumble_queue, &msg, OS_MESSAGE_FLAGS_BLOCKING);
		stopThread(rumbleThread, NULL);
	}
}

void startRumble()
{
	OSMessage msg = { .message = NUSSPLI_MESSAGE_NONE };
	OSSendMessage(&rumble_queue, &msg, OS_MESSAGE_FLAGS_NONE);
	ACPTurnOnDrcLed(pId, 1);
}
