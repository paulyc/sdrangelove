///////////////////////////////////////////////////////////////////////////////////
// Copyright (C) 2012 maintech GmbH, Otto-Hahn-Str. 15, 97204 Hoechberg, Germany //
// written by Christian Daniel                                                   //
//                                                                               //
// This program is free software; you can redistribute it and/or modify          //
// it under the terms of the GNU General Public License as published by          //
// the Free Software Foundation as version 3 of the License, or                  //
//                                                                               //
// This program is distributed in the hope that it will be useful,               //
// but WITHOUT ANY WARRANTY; without even the implied warranty of                //
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the                  //
// GNU General Public License V3 for more details.                               //
//                                                                               //
// You should have received a copy of the GNU General Public License             //
// along with this program. If not, see <http://www.gnu.org/licenses/>.          //
///////////////////////////////////////////////////////////////////////////////////

#include <string.h>
#include <QAudioFormat>
#include <QAudioDeviceInfo>
#include <QAudioOutput>
#include "audio/audiooutput.h"
#include "audio/audiofifo.h"

AudioOutput::AudioOutput() :
	m_mutex(),
	m_error(),
	m_deviceName(),
	m_rate(0),
	m_audioOutput(NULL),
	m_audioFifos()
{
}

AudioOutput::~AudioOutput()
{
	stop();

	QMutexLocker mutexLocker(&m_mutex);
	for(AudioFifos::iterator it = m_audioFifos.begin(); it != m_audioFifos.end(); ++it)
		delete *it;
	m_audioFifos.clear();
}

void AudioOutput::configure(const QString& deviceName, uint rate)
{
	QMutexLocker mutexLocker(&m_mutex);

	m_deviceName = deviceName;
	m_rate = rate;
}

bool AudioOutput::start()
{
	QMutexLocker mutexLocker(&m_mutex);

	QAudioFormat format;
	QAudioDeviceInfo devInfo(QAudioDeviceInfo::defaultOutputDevice());

	if(!m_deviceName.isEmpty()) {
		QList<QAudioDeviceInfo> devices = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
		bool found = false;
		for(int i = 0; i < devices.count(); ++i) {
			if(devices[i].deviceName() == m_deviceName) {
				devInfo = devices[i];
				found = true;
				break;
			}
		}
		if(!found) {
			m_error = tr("Audio output device %1 not available").arg(m_deviceName);
			return false;
		}
	}

	format.setSampleRate(m_rate);
	format.setChannelCount(2);
	format.setSampleSize(16);
	format.setCodec("audio/pcm");
	format.setByteOrder(QAudioFormat::LittleEndian);
	format.setSampleType(QAudioFormat::SignedInt);

	if(!devInfo.isFormatSupported(format)) {
		qDebug("default format not supported - try to use nearest");
		format = devInfo.nearestFormat(format);
	}
	qDebug("Audio output %s using samplerate %d", qPrintable(devInfo.deviceName()), format.sampleRate());

	if(format.sampleSize() != 16) {
		m_error = tr("Audio output %1 doesn't support 16 bit samples").arg(devInfo.deviceName());
		return false;
	}

	m_audioOutput = new QAudioOutput(devInfo, format);

	QIODevice::open(QIODevice::ReadOnly);

	//m_audioOutput->setBufferSize(3 * 4096);
	m_audioOutput->start(this);

	for(AudioFifos::iterator it = m_audioFifos.begin(); it != m_audioFifos.end(); ++it)
		(*it)->setSampleRate(m_audioOutput->format().sampleRate());

	return true;
}

void AudioOutput::stop()
{
	QMutexLocker mutexLocker(&m_mutex);

	for(AudioFifos::iterator it = m_audioFifos.begin(); it != m_audioFifos.end(); ++it)
		(*it)->setSampleRate(0);

	if(m_audioOutput != NULL) {
		m_audioOutput->stop();
		delete m_audioOutput;
		m_audioOutput = NULL;
	}
	QIODevice::close();
}

void AudioOutput::addFifo(AudioFifo* audioFifo)
{
	QMutexLocker mutexLocker(&m_mutex);

	if(m_audioOutput == NULL)
		audioFifo->setSampleRate(0);
	else audioFifo->setSampleRate(m_audioOutput->format().sampleRate());

	m_audioFifos.push_back(audioFifo);
}

void AudioOutput::removeFifo(AudioFifo* audioFifo)
{
	QMutexLocker mutexLocker(&m_mutex);

	audioFifo->setSampleRate(0);
	m_audioFifos.remove(audioFifo);
}

quint32 AudioOutput::getCurrentRate()
{
	QMutexLocker mutexLocker(&m_mutex);

	if(m_audioOutput == NULL)
		return 0;
	else return m_audioOutput->format().sampleRate();
}

bool AudioOutput::open(OpenMode mode)
{
	Q_UNUSED(mode);
	return false;
}

qint64 AudioOutput::readData(char* data, qint64 maxLen)
{
	QMutexLocker mutexLocker(&m_mutex);

	maxLen -= maxLen % 4;
	int framesPerBuffer = maxLen / 4;

	if((int)m_mixBuffer.size() < framesPerBuffer * 2) {
		m_mixBuffer.resize(framesPerBuffer * 2); // allocate 2 qint32 per frame (stereo)
		if(m_mixBuffer.size() != framesPerBuffer * 2) {
			qDebug("KAPUTT");
			return 0;
		}
	}
	memset(&m_mixBuffer[0], 0x00, 2 * framesPerBuffer * sizeof(m_mixBuffer[0])); // start with silence

	// sum up a block from all fifos
	for(AudioFifos::iterator it = m_audioFifos.begin(); it != m_audioFifos.end(); ++it) {
		if((*it)->isStopped())
			continue;

		// use outputBuffer as temp - yes, one memcpy could be saved
		uint samples = (*it)->read((quint8*)data, framesPerBuffer, 1);
		const qint16* src = (const qint16*)data;
		std::vector<qint32>::iterator dst = m_mixBuffer.begin();
		for(uint i = 0; i < samples; i++) {
			*dst += *src;
			++src;
			++dst;
			*dst += *src;
			++src;
			++dst;
		}
	}

	// convert to int16 and do saturation
	std::vector<qint32>::const_iterator src = m_mixBuffer.begin();
	qint16* dst = (qint16*)data;
	for(int i = 0; i < framesPerBuffer; ++i) {
		// left channel
		qint32 s = *src++;
		if(s < -32768)
			s = -32768;
		else if(s > 32767)
			s = 32767;
		*dst++ = s;
		// right channel
		s = *src++;
		if(s < -32768)
			s = -32768;
		else if(s > 32767)
			s = 32767;
		*dst++ = s;
	}

	return framesPerBuffer * 4;
}

qint64 AudioOutput::writeData(const char* data, qint64 len)
{
	Q_UNUSED(data);
	Q_UNUSED(len);
	return 0;
}
