/* Copyright (C) 2005-2007, Thorvald Natvig <thorvald@natvig.com>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
     this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
     this list of conditions and the following disclaimer in the documentation
     and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
     contributors may be used to endorse or promote products derived from this
     software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "DXAudioOutput.h"
#include "MainWindow.h"
#include "Plugins.h"
#include "Player.h"
#include "Global.h"
#include "DXConfigDialog.h"

#undef FAILED
#define FAILED(Status) (static_cast<HRESULT>(Status)<0)

// #define MY_DEFERRED DS3D_DEFERRED
#define MY_DEFERRED DS3D_IMMEDIATE


#define NBLOCKS 50
#define MAX(a,b)        ( (a) > (b) ? (a) : (b) )
#define MIN(a,b)        ( (a) < (b) ? (a) : (b) )

class DXAudioOutputRegistrar : public AudioOutputRegistrar {
	public:
		DXAudioOutputRegistrar();
		virtual AudioOutput *create();
		virtual const QList<audioDevice> getDeviceChoices();
		virtual void setDeviceChoice(const QVariant &);

};

// Static singleton
static DXAudioOutputRegistrar airDX;

DXAudioOutputRegistrar::DXAudioOutputRegistrar() : AudioOutputRegistrar(QLatin1String("DirectSound")) {
}

AudioOutput *DXAudioOutputRegistrar::create() {
	return new DXAudioOutput();
}

typedef QPair<QString, GUID> dsDevice;

static BOOL CALLBACK DSEnumProc(LPGUID lpGUID, const WCHAR* lpszDesc,
                                const WCHAR*, void *ctx) {
	if (lpGUID) {
		QList<dsDevice> *l =reinterpret_cast<QList<dsDevice> *>(ctx);
		*l << dsDevice(QString::fromUtf16(reinterpret_cast<const ushort*>(lpszDesc)), *lpGUID);
	}

	return(true);
}

const QList<audioDevice> DXAudioOutputRegistrar::getDeviceChoices() {
	QList<dsDevice> qlOutput;

	qlOutput << dsDevice(DXConfigDialog::tr("Default DirectSound Voice Output"), DSDEVID_DefaultVoicePlayback);
	DirectSoundEnumerate(DSEnumProc, reinterpret_cast<void *>(&qlOutput));

	QList<audioDevice> qlReturn;

	const GUID *lpguid = NULL;

	if (! g.s.qbaDXOutput.isEmpty()) {
		lpguid = reinterpret_cast<LPGUID>(g.s.qbaDXOutput.data());
	} else {
		lpguid = &DSDEVID_DefaultVoicePlayback;
	}

	foreach(dsDevice d, qlOutput) {
		if (d.second == *lpguid) {
			qlReturn << audioDevice(d.first, QByteArray(reinterpret_cast<const char *>(&d.second), sizeof(GUID)));
		}
	}
	foreach(dsDevice d, qlOutput) {
		if (d.second != *lpguid) {
			qlReturn << audioDevice(d.first, QByteArray(reinterpret_cast<const char *>(&d.second), sizeof(GUID)));
		}
	}
	return qlReturn;
}

void DXAudioOutputRegistrar::setDeviceChoice(const QVariant &choice) {
	g.s.qbaDXOutput = choice.toByteArray();
}


DXAudioOutputPlayer::DXAudioOutputPlayer(DXAudioOutput *ao, AudioOutputPlayer *aopl) {
	bPlaying = false;
	dxAudio = ao;
	aop=aopl;

	pDSBOutput = NULL;
	pDSNotify = NULL;
	pDS3dBuffer = NULL;

	iByteSize = aop->iFrameSize * 2;

	hNotificationEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
}

void DXAudioOutputPlayer::setupAudioDevice() {
	DSBUFFERDESC dsbd;
	WAVEFORMATEX wfx;
	HRESULT hr;

	ZeroMemory(&wfx, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;
	wfx.nChannels = 1;
	wfx.nBlockAlign = 2;
	wfx.nSamplesPerSec = SAMPLE_RATE;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.wBitsPerSample = 16;

	ZeroMemory(&dsbd, sizeof(DSBUFFERDESC));
	dsbd.dwSize          = sizeof(DSBUFFERDESC);
	dsbd.dwFlags         = DSBCAPS_GLOBALFOCUS|DSBCAPS_GETCURRENTPOSITION2;
	dsbd.dwFlags	 |= DSBCAPS_CTRLPOSITIONNOTIFY;
	if (dxAudio->p3DListener)
		dsbd.dwFlags	 |= DSBCAPS_CTRL3D;
	dsbd.dwBufferBytes = aop->iFrameSize * 2 * NBLOCKS;
	dsbd.lpwfxFormat     = &wfx;
	if (dxAudio->p3DListener) {
		switch (g.s.a3dModel) {
			case Settings::None:
			case Settings::Panning:
				dsbd.guid3DAlgorithm = DS3DALG_NO_VIRTUALIZATION;
				break;
			case Settings::Light:
				dsbd.guid3DAlgorithm = DS3DALG_HRTF_LIGHT;
				break;
			case Settings::Full:
				dsbd.guid3DAlgorithm = DS3DALG_HRTF_FULL;
				break;
		}
	}

	// Create the DirectSound buffer
	if (FAILED(hr = dxAudio->pDS->CreateSoundBuffer(&dsbd, &pDSBOutput, NULL)))
		qFatal("DXAudioOutputPlayer: CreateSoundBuffer (Secondary): 0x%08lx", hr);

	DSBPOSITIONNOTIFY    aPosNotify[NBLOCKS];

	for (int i=0;i<NBLOCKS;i++) {
		aPosNotify[i].dwOffset = aop->iFrameSize * 2 * i;
		aPosNotify[i].hEventNotify = hNotificationEvent;
	}

	if (FAILED(hr = pDSBOutput->QueryInterface(IID_IDirectSoundNotify, reinterpret_cast<VOID**>(&pDSNotify))))
		qFatal("DXAudioOutputPlayer: QueryInterface (Notify)");

	if (FAILED(hr = pDSNotify->SetNotificationPositions(NBLOCKS, aPosNotify)))
		qFatal("DXAudioOutputPlayer: SetNotificationPositions");

	if (dxAudio->p3DListener) {
		if (FAILED(pDSBOutput->QueryInterface(IID_IDirectSound3DBuffer8, reinterpret_cast<void **>(&pDS3dBuffer))))
			qFatal("DXAudioOutputPlayer: QueryInterface (DirectSound3DBuffer)");

		pDS3dBuffer->SetMinDistance(g.s.fDXMinDistance, MY_DEFERRED);
		pDS3dBuffer->SetMaxDistance(g.s.fDXMaxDistance, MY_DEFERRED);
	}

	qWarning("DXAudioOutputPlayer: %s: New %dHz output buffer of %ld bytes", qPrintable(aop->qsName), SAMPLE_RATE, dsbd.dwBufferBytes);
}

DXAudioOutputPlayer::~DXAudioOutputPlayer() {
	qWarning("DXAudioOutputPlayer: %s: Removed", qPrintable(aop->qsName));
	if (pDS3dBuffer)
		pDS3dBuffer->Release();
	if (pDSNotify)
		pDSNotify->Release();
	if (pDSBOutput) {
		pDSBOutput->Stop();
		pDSBOutput->Release();
	}
	CloseHandle(hNotificationEvent);
}

bool DXAudioOutputPlayer::playFrames() {
	int playblock;
	int nowriteblock;
	DWORD dwPlayPosition, dwWritePosition;
	HRESULT             hr;

	LPVOID aptr1, aptr2;
	DWORD nbytes1, nbytes2;

	bool alive = true;

	DWORD dwApply = MY_DEFERRED;

	if (! pDSBOutput) {
		setupAudioDevice();

		if (FAILED(hr = pDSBOutput->Lock(0, 0, &aptr1, &nbytes1, &aptr2, &nbytes2, DSBLOCK_ENTIREBUFFER)))
			qFatal("DXAudioOutputPlayer: Initial Lock");

		dwBufferSize = nbytes1 + nbytes2;
		if (aptr1)
			ZeroMemory(aptr1, nbytes1);
		if (aptr2)
			ZeroMemory(aptr2, nbytes2);

		if (FAILED(hr = pDSBOutput->Unlock(aptr1, nbytes1, aptr2, nbytes2)))
			qFatal("DXAudioOutputPlayer: Initial Unlock");

		dwApply = DS3D_IMMEDIATE;

		dwLastWritePos = 0;
		dwLastPlayPos = 0;
		dwTotalPlayPos = 0;

		iLastwriteblock = (NBLOCKS - 1 + g.s.iDXOutputDelay) % NBLOCKS;
	}

	if (FAILED(hr = pDSBOutput->GetCurrentPosition(&dwPlayPosition, &dwWritePosition)))
		qFatal("DXAudioOutputPlayer: GetCurrentPosition");

	playblock = dwWritePosition / iByteSize;
	nowriteblock = (playblock + g.s.iDXOutputDelay + 1) % NBLOCKS;

	for (int block=(iLastwriteblock + 1) % NBLOCKS;alive && (block!=nowriteblock);block=(block + 1) % NBLOCKS) {

		// Apparantly, even high end cards can sometimes move the play cursor BACKWARDS in 3D mode.
		// If that happens, let's just say we're in synch.

		bool broken = false;
		for (int i=0;i<10;i++)
			if ((nowriteblock + i)%NBLOCKS == iLastwriteblock)
				broken = true;

		if (broken) {
			qWarning("DXAudioOutputPlayer: Playbackwards");
			iLastwriteblock = (nowriteblock + NBLOCKS - 1) % NBLOCKS;
			break;
		}

		iLastwriteblock = block;

		alive = aop->decodeNextFrame();
//		qWarning("Block %02d/%02d nowrite %02d, last %02d (Pos %08d / %08d, Del %d)", block, NBLOCKS, nowriteblock, iLastwriteblock, dwPlayPosition, dwWritePosition,g.s.iDXOutputDelay);
		if (! alive) {
			iMissingFrames++;
			// Give 5 seconds grace before killing off buffer, as it seems continously creating and destroying them
			// taxes cheap soundcards more then it should.
			if (iMissingFrames > 250) {
				pDSBOutput->Stop();
				bPlaying = false;
				return false;
			}
		} else {
			iMissingFrames = 0;
		}

		if (pDS3dBuffer) {
			bool center = g.bCenterPosition;
			DWORD mode;

			pDS3dBuffer->GetMode(&mode);
			if (! center) {
				if ((fabs(aop->fPos[0]) < 0.1) && (fabs(aop->fPos[1]) < 0.1) && (fabs(aop->fPos[2]) < 0.1))
					center = true;
				else if (! g.p->bValid)
					center = true;
			}
			if (center) {
				if (mode != DS3DMODE_DISABLE)
					pDS3dBuffer->SetMode(DS3DMODE_DISABLE, dwApply);
			} else {
				if (mode != DS3DMODE_NORMAL)
					pDS3dBuffer->SetMode(DS3DMODE_NORMAL, dwApply);
				pDS3dBuffer->SetPosition(aop->fPos[0], aop->fPos[1], aop->fPos[2], dwApply);
			}
		}

		if (FAILED(hr = pDSBOutput->Lock(block * iByteSize, iByteSize, &aptr1, &nbytes1, &aptr2, &nbytes2, 0)))
			qFatal("DXAudioOutput: Lock block %u (%d bytes)",block, iByteSize);
		if (aptr1 && nbytes1)
			CopyMemory(aptr1, aop->psBuffer, MIN(iByteSize, nbytes1));
		if (aptr2 && nbytes2)
			CopyMemory(aptr2, aop->psBuffer+(nbytes1/2), MIN(iByteSize-nbytes1, nbytes2));
		if (FAILED(hr = pDSBOutput->Unlock(aptr1, nbytes1, aptr2, nbytes2)))
			qFatal("DXAudioOutput: Unlock %p(%u) %p(%u)",aptr1,nbytes1,aptr2,nbytes2);

		// If we get another while we're working, we're already taking care of it.
		ResetEvent(hNotificationEvent);

		if (FAILED(hr = pDSBOutput->GetCurrentPosition(&dwPlayPosition, &dwWritePosition)))
			qFatal("DXAudioOutputPlayer: GetCurrentPosition");

		playblock = dwWritePosition / iByteSize;
		nowriteblock = (playblock + g.s.iDXOutputDelay + 1) % NBLOCKS;
	}


	if (! bPlaying) {
		if (FAILED(hr = pDSBOutput->Play(0, 0, DSBPLAY_LOOPING)))
			qFatal("DXAudioOutputPlayer: Play");
		bPlaying = true;
	}

	return true;
}


DXAudioOutput::DXAudioOutput() {
	HRESULT hr;
	DSBUFFERDESC        dsbdesc;
	WAVEFORMATEX wfx;
	WAVEFORMATEX wfxSet;

	pDS = NULL;

	bool failed = false;

	bOk = false;

	ZeroMemory(&dsbdesc, sizeof(DSBUFFERDESC));
	dsbdesc.dwSize  = sizeof(DSBUFFERDESC);
	dsbdesc.dwFlags = DSBCAPS_PRIMARYBUFFER;
	if (g.s.a3dModel != Settings::None)
		dsbdesc.dwFlags |= DSBCAPS_CTRL3D;

	ZeroMemory(&wfxSet, sizeof(wfxSet));
	wfxSet.wFormatTag = WAVE_FORMAT_PCM;

	ZeroMemory(&wfx, sizeof(wfx));
	wfx.wFormatTag = WAVE_FORMAT_PCM;

	wfx.nChannels = 1;
	wfx.nSamplesPerSec = SAMPLE_RATE;
	wfx.nBlockAlign = 2;
	wfx.nAvgBytesPerSec = wfx.nSamplesPerSec * wfx.nBlockAlign;
	wfx.wBitsPerSample = 16;

	pDS = NULL;
	pDSBPrimary = NULL;
	p3DListener = NULL;

	hNotificationEvent = CreateEvent(NULL, FALSE, FALSE, NULL);

	if (! g.s.qbaDXOutput.isEmpty()) {
		LPGUID lpguid = reinterpret_cast<LPGUID>(g.s.qbaDXOutput.data());
		if (FAILED(hr = DirectSoundCreate8(lpguid, &pDS, NULL))) {
			failed = true;
		}
	}

	if (! pDS && FAILED(hr = DirectSoundCreate8(&DSDEVID_DefaultVoicePlayback, &pDS, NULL)))
		qWarning("DXAudioOutput: DirectSoundCreate");
	else if (FAILED(hr = pDS->SetCooperativeLevel(g.mw->winId(), DSSCL_PRIORITY)))
		qWarning("DXAudioOutput: SetCooperativeLevel");
	else if (FAILED(hr = pDS->CreateSoundBuffer(&dsbdesc, &pDSBPrimary, NULL)))
		qWarning("DXAudioOutput: CreateSoundBuffer (Primary) : 0x%08lx", hr);
	else if (FAILED(hr = pDSBPrimary->SetFormat(&wfx)))
		qFatal("DXAudioOutput: SetFormat");
	else if (FAILED(hr = pDSBPrimary->GetFormat(&wfxSet, sizeof(wfxSet), NULL)))
		qFatal("DXAudioOutput: GetFormat");
	else if (g.s.a3dModel != Settings::None) {
		if (FAILED(hr = pDSBPrimary->QueryInterface(IID_IDirectSound3DListener8, reinterpret_cast<void **>(&p3DListener)))) {
			qWarning("DXAudioOutput: QueryInterface (DirectSound3DListener8): 0x%08lx",hr);
		} else {
			p3DListener->SetRolloffFactor(g.s.fDXRollOff, MY_DEFERRED);
			p3DListener->SetDopplerFactor(DS3D_MINDOPPLERFACTOR, MY_DEFERRED);
			p3DListener->CommitDeferredSettings();
			bOk = true;
		}
	} else {
		bOk = true;
	}

	if (! bOk) {
		QMessageBox::warning(NULL, tr("Mumble"), tr("Opening chosen DirectSound Output failed. No audio will be heard."), QMessageBox::Ok, QMessageBox::NoButton);
		return;
	}

	if (failed)
		QMessageBox::warning(NULL, tr("Mumble"), tr("Opening chosen DirectSound Output failed. Default device will be used."), QMessageBox::Ok, QMessageBox::NoButton);

	qWarning("DXAudioOutput: Primary buffer of %ld Hz, %d channels, %d bits",wfxSet.nSamplesPerSec,wfxSet.nChannels,wfxSet.wBitsPerSample);
	if (p3DListener)
		qWarning("DXAudioOutput: 3D mode active");

	bRunning = true;
}

DXAudioOutput::~DXAudioOutput() {
	bRunning = false;
	wipe();
	SetEvent(hNotificationEvent);
	wait();

	if (p3DListener)
		p3DListener->Release();
	if (pDSBPrimary)
		pDSBPrimary->Release();
	if (pDS)
		pDS->Release();
	CloseHandle(hNotificationEvent);
}

void DXAudioOutput::newPlayer(AudioOutputPlayer *aop) {
	DXAudioOutputPlayer *dxaop = new DXAudioOutputPlayer(this, aop);
	SetEvent(dxaop->hNotificationEvent);

	qhPlayers[aop] = dxaop;
}

void DXAudioOutput::updateListener() {
	if (! p3DListener)
		return;

	HRESULT hr;
	DS3DLISTENER li;
	Plugins *p = g.p;
	li.dwSize=sizeof(li);
	if (p->bValid && ! g.bCenterPosition && p3DListener) {
		// Only set this if we need to. If centerposition is on, or we don't have valid data,
		// the 3d mode for the buffers will be disabled, so don't bother with updates.
		p3DListener->SetPosition(p->fPosition[0], p->fPosition[1], p->fPosition[2], MY_DEFERRED);
		p3DListener->SetOrientation(p->fFront[0], p->fFront[1], p->fFront[2],
		                            p->fTop[0], p->fTop[1], p->fTop[2], MY_DEFERRED);
	}
	if (FAILED(hr =p3DListener->CommitDeferredSettings()))
		qWarning("DXAudioOutputPlayer: CommitDeferrredSettings failed 0x%08lx", hr);

	/*
		float a[3], b[3];
		p3DListener->GetOrientation((D3DVECTOR *) a, (D3DVECTOR *) b);
		qWarning("%f %f %f -- %f %f %f", a[0], a[1], a[2], b[0], b[1], b[2]);
	*/
}

void DXAudioOutput::removeBuffer(AudioOutputPlayer *aop) {
	DXAudioOutputPlayer *dxaop=qhPlayers.take(aop);

	AudioOutput::removeBuffer(aop);
	if (dxaop)
		delete dxaop;
}

void DXAudioOutput::run() {
	DXAudioOutputPlayer *dxaop;
	HANDLE handles[MAXIMUM_WAIT_OBJECTS];
	DWORD count = 0;
	DWORD hit;

	LARGE_INTEGER ticksPerSecond;
	LARGE_INTEGER ticksPerFrame;
	LARGE_INTEGER ticksNow;
LARGE_INTEGER ticksNext = {QuadPart:
	                           0L
	                          };

	bool found;
	bool alive;

	QueryPerformanceFrequency(&ticksPerSecond);
	ticksPerFrame.QuadPart = ticksPerSecond.QuadPart / 50;

	while (bRunning) {
		// Ok, so I thought about it..
		// We might optimize this by having a QHash<hEvent, AOP *>, but..
		// .. most of the time, there will be one or maybe two AOPs active.
		// The overhead of updating the cache is likely to outstrip the
		// benefit.

		count = 0;

		qrwlOutputs.lockForRead();
		foreach(dxaop, qhPlayers) {
			handles[count++] = dxaop->hNotificationEvent;
		}
		handles[count++] = hNotificationEvent;
		qrwlOutputs.unlock();

		hit = WaitForMultipleObjects(count, handles, false, 20);

		found = false;

		QueryPerformanceCounter(&ticksNow);
		if (ticksNow.QuadPart > ticksNext.QuadPart) {
			ticksNext.QuadPart = ticksNow.QuadPart + ticksPerFrame.QuadPart;
			updateListener();
		}

		qrwlOutputs.lockForRead();
		if (hit >= WAIT_OBJECT_0 && hit < WAIT_OBJECT_0 + count - 1) {
			foreach(dxaop, qhPlayers) {
				if (handles[hit - WAIT_OBJECT_0] == dxaop->hNotificationEvent) {
					found = true;
					alive = dxaop->playFrames();
					break;
				}
			}
		}
		qrwlOutputs.unlock();

		if (found && ! alive)
			removeBuffer(dxaop->aop);
	}
}

