// ----------------------------------------------------------------------------
// rigMEM.cxx
//
// Copyright (C) 2007-2009
//		Dave Freese, W1HKJ
//
// This file is part of fldigi.
//
// Fldigi is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// Fldigi is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with fldigi.  If not, see <http://www.gnu.org/licenses/>.
// ----------------------------------------------------------------------------

#include <config.h>

#if defined(__OpenBSD__)
#  include <sys/types.h>
#endif

#if !defined(__WOE32__) && !defined(__APPLE__)
#  include <sys/ipc.h>
#  include <sys/msg.h>
#  include <sys/shm.h>
#endif
#include <sys/time.h>

#include <FL/Fl.H>
#include <FL/fl_ask.H>

#include "threads.h"
#include "qrunner.h"
#include "misc.h"
#include "configuration.h"

#include "rigMEM.h"
#include "fl_digi.h"
#include "main.h"
#include "trx.h"
#include "rigsupport.h"

#include "debug.h"

#include "dl_fldigi/hbtint.h"

using namespace std;

LOG_FILE_SOURCE(debug::LOG_RIGCONTROL);

/* ---------------------------------------------------------------------- */

#define PTT_OFF false
#define PTT_ON  true

static pthread_t rigMEM_thread;
static bool rigMEM_exit = false;
static bool rigMEM_enabled;

static void *rigMEM_loop(void *args);

static bool rigMEM_PTT = PTT_OFF;
static bool rig_qsy = false;
static bool TogglePTT = false;
static bool rigMEMisPTT = false;
static bool change_mode = false;

//static long long qsy_f;
//static long long qsy_fmid;
static long qsy_f;
static long qsy_fmid;
char szmode[80];

#if !defined(__WOE32__) && !defined(__APPLE__)
// Linux & *BSD interface to Kachina

struct ST_SHMEM {
	int  flag;
	long freq;
	long midfreq;
	char mode[4];
}sharedmem;


struct ST_SHMEM *freqflag;
struct ST_SHMEM noshare;

void *shared_memory = (void *)0;
int  shmid = -1;

key_t hash (char *str)
{
	key_t key = 1;
	unsigned int i;
	for (i = 0; i < strlen(str); i++)
		key *= str[i];
	return key;
}

bool rigMEM_init(void)
{
	rigMEM_enabled = false;
	noshare.freq = 7070000L;
	noshare.midfreq = 1000L;
	noshare.flag = 2;
	freqflag = &noshare;

	shmid = shmget ((key_t)1234, sizeof(ST_SHMEM), 0666 | IPC_CREAT);

	if (shmid < 0) {
		LOG_PERROR("shmget");
		 return false;
	}
 	shared_memory = shmat (shmid, (void *)0, 0);
 	if (shared_memory == (void *)-1) {
		LOG_PERROR("shmat");
		return false;
 	}
	freqflag = (struct ST_SHMEM *) shared_memory;

	rig_qsy = false;
	rigMEM_PTT = PTT_OFF;
	TogglePTT = false;
	rigMEMisPTT = false;

	freqflag->freq = 7070000L;
	freqflag->midfreq = 1000L;
	freqflag->flag = -1;
	wf->carrier((int)qsy_fmid);
	wf->rfcarrier(freqflag->freq);
	wf->movetocenter();
	show_frequency(freqflag->freq);

	if (pthread_create(&rigMEM_thread, NULL, rigMEM_loop, NULL) < 0) {
		LOG_PERROR("rigMEM init: pthread_create failed");
		return false;
	}

	rigMEM_enabled = true;

	init_rigMEM_RigDialog();

	return true;
}

void rigMEM_close(void)
{
	if (!rigMEM_enabled) return;

// tell the rigMEM thread to kill it self
	rigMEM_exit = true;

// and then wait for it to die
	pthread_join(rigMEM_thread, NULL);
	rigMEM_enabled = false;
	rigMEM_exit = false;

	wf->rfcarrier(0L);
	wf->USB(true);

}

static void *rigMEM_loop(void *args)
{
	SET_THREAD_ID(RIGCTL_TID);

	int sb = true;

	for (;;) {
	/* see if we are being canceled */
		if (rigMEM_exit)
			break;

		if (rig_qsy || change_mode) {
			if (rig_qsy) {
				freqflag->freq = qsy_f;
				if (qsy_fmid > 0) {
					if (active_modem->freqlocked() == true) {
						active_modem->set_freqlock(false);
						active_modem->set_freq((int)qsy_fmid);
						active_modem->set_freqlock(true);
					} else
						active_modem->set_freq((int)qsy_fmid);
					wf->carrier((int)qsy_fmid);
					wf->rfcarrier(freqflag->freq);
					wf->movetocenter();
				} else
					wf->rfcarrier(freqflag->freq);
				rig_qsy = false;
			}
			if (change_mode) {
				strncpy(freqflag->mode, szmode, 4);
				change_mode = false;
			}
			freqflag->flag = -2;
		} else if (TogglePTT) {
			if (rigMEM_PTT == PTT_ON)
				freqflag->flag = -3;
			else
				freqflag->flag = -4;
			TogglePTT = false;
			MilliSleep(20);
		}

		if (freqflag->flag >= 0) {
			freqflag->flag = -1; // read frequency
			sb = false;
		}
/* delay for 20 msec interval */
		MilliSleep(20);

/* if rigMEM control program running it will update the freqflag structure every 10 msec */
		if (freqflag->flag > -1) {
			if ((freqflag->flag & 0x10) == 0x10)
				rigMEMisPTT = true;
			if ((freqflag->flag & 0x0F) == 2)
				sb = false;
			else
				sb = true;

			if (wf->rfcarrier() != freqflag->freq)
				wf->rfcarrier(freqflag->freq);
			show_frequency(freqflag->freq);
			wf->USB(sb);
			REQ (&Fl_ComboBox::put_value, qso_opMODE, sb ? "USB" : "LSB");

			dl_fldigi::hbtint::rig_set_freq(freqflag->freq);
			dl_fldigi::hbtint::rig_set_mode(sb ? "USB" : "LSB");
		}
	}

	if (shmdt(shared_memory) == -1)
		LOG_PERROR("shmdt");

	shmid = -1;

	/* this will exit the rigMEM thread */
	return NULL;
}

#elif defined(__WOE32__)
//===================================================
// Windows interface to Kachina / rigCAT
//

FILE *IOout;
FILE *IOin;
int  IOflag;
long IOfreq = 7070000L;
long IOmidfreq = 1000L;

bool rigMEM_init(void)
{
	rigMEM_enabled = false;

	rig_qsy = false;
	rigMEM_PTT = PTT_OFF;
	TogglePTT = false;
	rigMEMisPTT = false;
	qsy_f = wf->rfcarrier();
	qsy_fmid = 0;

	if (pthread_create(&rigMEM_thread, NULL, rigMEM_loop, NULL) < 0) {
		LOG_PERROR("pthread_create");
		return false;
	}
	rigMEM_enabled = true;

	init_rigMEM_RigDialog();

	return true;
}

void rigMEM_close(void)
{
	if (!rigMEM_enabled) return;

// delete the ptt control file so Kachina can work stand alone
	remove("c:/RIGCTL/ptt");

// tell the rigMEM thread to kill it self
	rigMEM_exit = true;

// and then wait for it to die
	pthread_join(rigMEM_thread, NULL);
	LOG_DEBUG("rigMEM down");
	rigMEM_enabled = false;
	rigMEM_exit = false;

	wf->rfcarrier(0L);
	wf->USB(true);

}
static void *rigMEM_loop(void *args)
{
	SET_THREAD_ID(RIGCTL_TID);

	int sb = true;

	for (;;) {
	/* see if we are being canceled */
		if (rigMEM_exit)
			break;

		if (TogglePTT || rig_qsy || change_mode) {
			IOout = fopen("c:/RIGCTL/ptt", "we");
			if (IOout) {
				LOG_VERBOSE("sent %d, %c, %s",
					 (int)qsy_f,
					 rigMEM_PTT == PTT_ON ? 'X' : 'R',
					 szmode);
				fprintf(IOout,"%c\n", rigMEM_PTT == PTT_ON ? 'X' : 'R');
				fprintf(IOout,"%d\n", (int)qsy_f);
				fprintf(IOout,"%s\n", szmode);
				fclose(IOout);
				if (TogglePTT) TogglePTT = false;
				if (rig_qsy) rig_qsy = false;
				if (change_mode) change_mode = false;
				show_frequency(qsy_f);
			}
		}

		IOin = fopen("c:/RIGCTL/rig", "re");
		if (IOin) {
			fscanf(IOin, "%ld\n", &IOfreq);
			fscanf(IOin, "%s", szmode);
			fclose(IOin);
			remove("c:/RIGCTL/rig");
			LOG_VERBOSE("rcvd %d, %s", (int)IOfreq, szmode);

			wf->rfcarrier(IOfreq);
			show_frequency(IOfreq);
			qsy_f = IOfreq;
			sb = (strcmp(szmode, "USB") == 0);
			wf->USB(sb);
			REQ (&Fl_ComboBox::put_value, qso_opMODE, sb ? "USB" : "LSB");
		}

// delay for 50 msec interval
		MilliSleep(50);

	}

	/* this will exit the rigMEM thread */
	return NULL;
}

#elif defined(__APPLE__)
// FIXME: we should be using an IPC mechanism that works on all Unix variants,
// or not compile rigMEM at all on OS X.

bool rigMEM_init(void) { return false; }
void rigMEM_close(void) { }
static void *rigMEM_loop(void *args) { return NULL; }

#endif

bool rigMEM_active(void)
{
	return (rigMEM_enabled);
}

bool rigMEM_CanPTT(void)
{
	return rigMEMisPTT;
}

void setrigMEM_PTT (bool on)
{
  rigMEM_PTT = on;
  TogglePTT = true;
}

void rigMEM_set_qsy(long long f)
{
	if (!rigMEM_enabled)
		return;

	wf->rfcarrier(f);
	wf->movetocenter();

	qsy_f = f;
	rig_qsy = true;
}

void rigMEM_set_freq(long long f)
{
	if (!rigMEM_enabled)
		return;

	qsy_f = f;
	wf->rfcarrier(f);
	rig_qsy = true;

}

void rigMEM_setmode(const string& s)
{
	strncpy(szmode, s.c_str(), 4);
	wf->USB( strcmp(szmode,"USB") == 0 );
	change_mode = true;
	return;
}

/* ---------------------------------------------------------------------- */

