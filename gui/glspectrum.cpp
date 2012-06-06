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

#include <QMouseEvent>
#include "glspectrum.h"

GLSpectrum::GLSpectrum(QWidget* parent) :
	QGLWidget(parent),
	m_cursorState(CSNormal),
	m_changesPending(true),
	m_centerFrequency(100000000),
	m_sampleRate(500000),
	m_fftSize(512),
	m_waterfallHeight(100),
	m_leftMarginTextureAllocated(false),
	m_frequencyTextureAllocated(false),
	m_waterfallBuffer(NULL),
	m_waterfallTextureAllocated(false),
	m_waterfallTextureHeight(-1),
	m_histogramBuffer(NULL),
	m_histogram(NULL),
	m_histogramHoldoff(NULL),
	m_histogramTextureAllocated(false),
	m_displayChanged(false)
{
	setAutoFillBackground(false);
	setAttribute(Qt::WA_OpaquePaintEvent, true);
	setAttribute(Qt::WA_NoSystemBackground, true);
	setMouseTracking(true);

	setMinimumHeight(300);

	m_waterfallShare = 0.5;

	for(int i = 0; i <= 239; i++) {
		 QColor c;
		 c.setHsv(239 - i, 255, 15 + i);
		 ((quint8*)&m_waterfallPalette[i])[0] = c.red();
		 ((quint8*)&m_waterfallPalette[i])[1] = c.green();
		 ((quint8*)&m_waterfallPalette[i])[2] = c.blue();
		 ((quint8*)&m_waterfallPalette[i])[3] = c.alpha();
	 }

	m_histogramPalette[0] = m_waterfallPalette[0];
	for(int i = 1; i < 240; i++) {
		 QColor c;
		 c.setHsv(239 - i, 255 - ((i < 200) ? 0 : (i - 200) * 3), 150 + ((i < 100) ? i : 100));
		 ((quint8*)&m_histogramPalette[i])[0] = c.red();
		 ((quint8*)&m_histogramPalette[i])[1] = c.green();
		 ((quint8*)&m_histogramPalette[i])[2] = c.blue();
		 ((quint8*)&m_histogramPalette[i])[3] = c.alpha();
	}
	for(int i = 1; i < 16; i++) {
		QColor c;
		c.setHsv(270, 128, 48 + i * 4);
		((quint8*)&m_histogramPalette[i])[0] = c.red();
		((quint8*)&m_histogramPalette[i])[1] = c.green();
		((quint8*)&m_histogramPalette[i])[2] = c.blue();
		((quint8*)&m_histogramPalette[i])[3] = c.alpha();
	}
	m_histogramHoldoffBase = 4;
	m_histogramHoldoffCount = m_histogramHoldoffBase;
	m_histogramLateHoldoff = 20;

	m_timeScale.setFont(font());
	m_timeScale.setOrientation(Qt::Vertical);
	m_powerScale.setFont(font());
	m_powerScale.setOrientation(Qt::Vertical);
	m_frequencyScale.setFont(font());
	m_frequencyScale.setOrientation(Qt::Horizontal);

	connect(&m_timer, SIGNAL(timeout()), this, SLOT(tick()));
	m_timer.start(50);
}

GLSpectrum::~GLSpectrum()
{
	QMutexLocker mutexLocker(&m_mutex);

	m_changesPending = true;

	if(m_waterfallBuffer != NULL) {
		delete m_waterfallBuffer;
		m_waterfallBuffer = NULL;
	}
	if(m_waterfallTextureAllocated) {
		glDeleteTextures(1, &m_waterfallTexture);
		m_waterfallTextureAllocated = false;
	}
	if(m_histogramBuffer != NULL) {
		delete m_histogramBuffer;
		m_histogramBuffer = NULL;
	}
	if(m_histogram != NULL) {
		delete[] m_histogram;
		m_histogram = NULL;
	}
	if(m_histogramHoldoff != NULL) {
		delete[] m_histogramHoldoff;
		m_histogramHoldoff = NULL;
	}
	if(m_histogramTextureAllocated) {
		glDeleteTextures(1, &m_histogramTexture);
		m_histogramTextureAllocated = false;
	}
	if(m_leftMarginTextureAllocated) {
		deleteTexture(m_leftMarginTexture);
		m_leftMarginTextureAllocated = false;
	}
	if(m_frequencyTextureAllocated) {
		deleteTexture(m_frequencyTexture);
		m_frequencyTextureAllocated = false;
	}
}

void GLSpectrum::setCenterFrequency(quint64 frequency)
{
	m_centerFrequency = frequency;
	m_changesPending = true;
}

void GLSpectrum::setSampleRate(qint32 sampleRate)
{
	m_sampleRate = sampleRate;
	m_changesPending = true;
}

void GLSpectrum::newSpectrum(const std::vector<Real>& spectrum)
{
	QMutexLocker mutexLocker(&m_mutex);

	m_displayChanged = true;

	if(m_changesPending) {
		m_fftSize = spectrum.size();
		return;
	}

	if((int)spectrum.size() != m_fftSize) {
		m_fftSize = spectrum.size();
		m_changesPending = true;
		return;
	}

	updateWaterfall(spectrum);
	updateHistogram(spectrum);
}

void GLSpectrum::updateWaterfall(const std::vector<Real>& spectrum)
{
	if(m_waterfallBufferPos < m_waterfallBuffer->height()) {
		quint32* pix = (quint32*)m_waterfallBuffer->scanLine(m_waterfallBufferPos);

		for(int i = 0; i < m_fftSize; i++) {
			Real vr = 2.4 * (spectrum[i] + 99.0);
			int v = (int)vr;

			if(v > 239)
				v = 239;
			else if(v < 0)
				v = 0;

			*pix++ = m_waterfallPalette[(int)v];
		}

		m_waterfallBufferPos++;
	}
}

void GLSpectrum::updateHistogram(const std::vector<Real>& spectrum)
{
	quint8* b = m_histogram;
	quint8* h = m_histogramHoldoff;

	m_histogramHoldoffCount--;
	if(m_histogramHoldoffCount <= 0) {
		for(int i = 0; i < 100 * m_fftSize; i++) {
			if(*b > 20) {
				*b = *b - 1;
			} else if(*b > 0) {
				if(*h > 0) {
					*h = *h - 1;
				} else {
					*h = m_histogramLateHoldoff;
					*b = *b - 1;
				}
			}
			b++;
			h++;
		}
		m_histogramHoldoffCount = m_histogramHoldoffBase;
	}

	b = m_histogram;
	h = m_histogramHoldoff;
	for(size_t i = 0; i < spectrum.size(); i++) {
		Real vr = spectrum[i] + 99.0;
		int v = (int)vr;
		if((v >= 0) && (v <= 99)) {
			if(*(b + v) < 220)
				(*(b + v)) += 4;
			else if(*(b + v) < 239)
				(*(b + v)) += 1;
			//*h = m_lateHoldOff;
		}

		b += 100;
		h += 100;
	}
}

void GLSpectrum::initializeGL()
{
}

void GLSpectrum::resizeGL(int width, int height)
{
	glViewport(0, 0, width, height);

	m_changesPending = true;
}

void GLSpectrum::paintGL()
{
	if(!m_mutex.tryLock(2))
		return;

	if(m_changesPending)
		applyChanges();

	if(m_fftSize <= 0) {
		m_mutex.unlock();
		return;
	}

	glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
	glClear(GL_COLOR_BUFFER_BIT);

	glEnable(GL_TEXTURE_2D);

	glPushMatrix();
	glScalef(2.0, -2.0, 1.0);
	glTranslatef(-0.50, -0.5, 0);

	float left = (float)m_leftMargin / (float)width();
	float right = (float)(width() - m_rightMargin) / (float)width();
	float top = (float)m_topMargin / (float)height();
	float bottom = (float)(height() - m_bottomMargin) / (float)height();
	float waterfallBottom = (float)(m_topMargin + m_waterfallHeight) / (float)height();
	float histogramTop = (float)m_histogramTop / (float)height();

	{
		glBindTexture(GL_TEXTURE_2D, m_waterfallTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

		for(int i = 0; i < m_waterfallBufferPos; i++) {
			glTexSubImage2D(GL_TEXTURE_2D, 0, 0, m_waterfallTexturePos, m_fftSize, 1, GL_RGBA, GL_UNSIGNED_BYTE, m_waterfallBuffer->scanLine(i));
			m_waterfallTexturePos = (m_waterfallTexturePos + 1) % m_waterfallTextureHeight;
		}
		m_waterfallBufferPos = 0;

		float prop_y = m_waterfallTexturePos / (m_waterfallTextureHeight - 1.0);
		float off = 1.0 / (m_waterfallTextureHeight - 1.0);
		glBegin(GL_QUADS);
		glTexCoord2f(0, prop_y + 1 - off);
		glVertex2f(left, waterfallBottom);
		glTexCoord2f(1.0, prop_y + 1 - off);
		glVertex2f(right, waterfallBottom);
		glTexCoord2f(1.0, prop_y);
		glVertex2f(right, top);
		glTexCoord2f(0, prop_y);
		glVertex2f(left, top);
		glEnd();
	}

	{
		quint32* pix;
		quint8* bs = m_histogram;
		for(int y = 0; y < 100; y++) {
			quint8* b = bs;
			pix = (quint32*)m_histogramBuffer->scanLine(99 - y);
			for(int x = 0; x < m_fftSize; x++) {
				*pix = m_histogramPalette[*b];
				pix++;
				b += 100;
			}
			bs++;
		}

		glBindTexture(GL_TEXTURE_2D, m_histogramTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_fftSize, 100, GL_RGBA, GL_UNSIGNED_BYTE, m_histogramBuffer->scanLine(0));

		glBegin(GL_QUADS);
		glTexCoord2f(0, 1);
		glVertex2f(left, bottom);
		glTexCoord2f(1, 1);
		glVertex2f(right, bottom);
		glTexCoord2f(1, 0);
		glVertex2f(right, histogramTop);
		glTexCoord2f(0, 0);
		glVertex2f(left, histogramTop);
		glEnd();
	}

	{
		glBindTexture(GL_TEXTURE_2D, m_leftMarginTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);

		glBegin(GL_QUADS);
		glTexCoord2f(0, 0);
		glVertex2f(0, 1);
		glTexCoord2f(1, 0);
		glVertex2f(left, 1);
		glTexCoord2f(1, 1);
		glVertex2f(left, 0);
		glTexCoord2f(0, 1);
		glVertex2f(0, 0);
		glEnd();

	}

	{
		glBindTexture(GL_TEXTURE_2D, m_frequencyTexture);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

		glBegin(GL_QUADS);
		glTexCoord2f(0, 0);
		glVertex2f(0, (float)(m_frequencyScaleTop + m_frequencyScaleHeight) / (float)height());
		glTexCoord2f(1, 0);
		glVertex2f(1, (float)(m_frequencyScaleTop + m_frequencyScaleHeight) / (float)height());
		glTexCoord2f(1, 1);
		glVertex2f(1, (float)m_frequencyScaleTop / (float)height());
		glTexCoord2f(0, 1);
		glVertex2f(0, (float)m_frequencyScaleTop / (float)height());
		glEnd();
		glDisable(GL_BLEND);
	}

	glDisable(GL_TEXTURE_2D);

	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glLineWidth(1.0f);

		glColor4f(1, 1, 1, 0.3);

		glBegin(GL_LINE_LOOP);
		glVertex2f(left + 1.0 / width(), top);
		glVertex2f(right, top);
		glVertex2f(right, waterfallBottom - 1.0 / height());
		glVertex2f(left + 1.0 / width(), waterfallBottom - 1.0 / height());
		glEnd();

		glBegin(GL_LINE_LOOP);
		glVertex2f(left + 1.0 / width(), histogramTop);
		glVertex2f(right, histogramTop);
		glVertex2f(right, bottom - 1.0 / height());
		glVertex2f(left + 1.0 / width(), bottom - 1.0 / height());
		glEnd();

		glColor4f(1, 1, 1, 0.05);

		const ScaleEngine::TickList* tickList = &m_timeScale.getTickList();
		const ScaleEngine::Tick* tick;
		for(int i = 0; i < tickList->count(); i++) {
			tick = &(*tickList)[i];
			if(tick->major) {
				if(tick->textSize > 0) {
					float y = top + (tick->pos + 1) / (float)height();
					glBegin(GL_LINE_LOOP);
					glVertex2f(left - 3.0 / width(), y);
					glVertex2f(right, y);
					glEnd();
				}

			}
		}
		tickList = &m_powerScale.getTickList();
		for(int i= 0; i < tickList->count(); i++) {
			tick = &(*tickList)[i];
			if(tick->major) {
				if(tick->textSize > 0) {
					float y = bottom - (tick->pos + 1) / height();
					glBegin(GL_LINE_LOOP);
					glVertex2f(left - 3.0 / width(), y);
					glVertex2f(right, y);
					glEnd();
				}
			}
		}

		tickList = &m_frequencyScale.getTickList();
		for(int i= 0; i < tickList->count(); i++) {
			tick = &(*tickList)[i];
			if(tick->major) {
				if(tick->textSize > 0) {
					float x = left + (float)tick->pos / width();
					glBegin(GL_LINE_LOOP);
					glVertex2f(x, top);
					glVertex2f(x, bottom);
					glEnd();
				}
			}
		}

		glDisable(GL_BLEND);
	}

	glPopMatrix();

	m_mutex.unlock();
}

void GLSpectrum::applyChanges()
{
	if(m_fftSize <= 0)
		return;

	QFontMetrics fm(font());

	m_topMargin = fm.ascent() * 1.5;
	m_bottomMargin = fm.ascent() * 1.5;

	m_waterfallHeight = height() * m_waterfallShare;
	if(m_waterfallHeight < 0)
		m_waterfallHeight = 0;
	m_frequencyScaleHeight = fm.height() * 2;
	m_frequencyScaleTop = m_topMargin + m_waterfallHeight;
	m_histogramTop = m_topMargin + m_waterfallHeight + m_frequencyScaleHeight;
	if(m_frequencyScaleHeight < 0)
		m_frequencyScaleHeight = 0;
	if(m_histogramTop < 0)
		m_histogramTop = 0;

	m_timeScale.setSize(m_waterfallHeight);
	m_timeScale.setRange(Unit::Time, (m_waterfallHeight * m_fftSize) / -(float)m_sampleRate, 0);
	m_powerScale.setSize(height() - m_histogramTop - m_bottomMargin);
	m_powerScale.setRange(Unit::Decibel, -100, 0);

	int M = fm.width("-");
	m_leftMargin = m_timeScale.getScaleWidth() + 2 * M;
	m_rightMargin = fm.width("000");

	m_frequencyScale.setSize(width() - m_leftMargin - m_rightMargin);
	m_frequencyScale.setRange(Unit::Frequency, m_centerFrequency - m_sampleRate / 2, m_centerFrequency + m_sampleRate / 2);

	{
		m_leftMarginPixmap = QPixmap(m_leftMargin, height());
		m_leftMarginPixmap.fill(Qt::black);
		{
			QPainter painter(&m_leftMarginPixmap);
			painter.setPen(QColor(0xf0, 0xf0, 0xff));
			const ScaleEngine::TickList* tickList = &m_timeScale.getTickList();
			const ScaleEngine::Tick* tick;
			for(int i = 0; i < tickList->count(); i++) {
				tick = &(*tickList)[i];
				if(tick->major) {
					if(tick->textSize > 0)
						painter.drawText(QPointF(m_leftMargin - M - tick->textSize, m_topMargin + fm.ascent() + tick->textPos), tick->text);
				}
			}
			tickList = &m_powerScale.getTickList();
			for(int i = 0; i < tickList->count(); i++) {
				tick = &(*tickList)[i];
				if(tick->major) {
					if(tick->textSize > 0)
						painter.drawText(QPointF(m_leftMargin - M - tick->textSize, height() - m_bottomMargin - tick->textPos - 1), tick->text);
				}
			}
		}
		if(m_leftMarginTextureAllocated)
			deleteTexture(m_leftMarginTexture);
		m_leftMarginTexture = bindTexture(m_leftMarginPixmap);
		m_leftMarginTextureAllocated = true;
	}
	{
		m_frequencyPixmap = QPixmap(width(), m_frequencyScaleHeight);
		m_frequencyPixmap.fill(Qt::transparent);
		{
			QPainter painter(&m_frequencyPixmap);
			painter.setPen(Qt::NoPen);
			painter.setBrush(Qt::black);
			painter.drawRect(m_leftMargin, 0, width() - m_leftMargin, m_frequencyScaleHeight);
			painter.setPen(QColor(0xf0, 0xf0, 0xff));
			const ScaleEngine::TickList* tickList = &m_frequencyScale.getTickList();
			const ScaleEngine::Tick* tick;
			for(int i = 0; i < tickList->count(); i++) {
				tick = &(*tickList)[i];
				if(tick->major) {
					if(tick->textSize > 0)
						painter.drawText(QPointF(m_leftMargin + tick->textPos, fm.height() + fm.ascent() / 2 - 1), tick->text);
				}
			}

		}
		if(m_frequencyTextureAllocated)
			deleteTexture(m_frequencyTexture);
		m_frequencyTexture = bindTexture(m_frequencyPixmap);
		m_frequencyTextureAllocated = true;
	}

	if(!m_waterfallTextureAllocated) {
		glGenTextures(1, &m_waterfallTexture);
		m_waterfallTextureAllocated = true;
	}
	if(!m_histogramTextureAllocated) {
		glGenTextures(1, &m_histogramTexture);
		m_histogramTextureAllocated = true;
	}

	bool fftSizeChanged;
	if(m_waterfallBuffer != NULL)
		fftSizeChanged = m_waterfallBuffer->width() != m_fftSize;
	else fftSizeChanged = true;
	bool windowSizeChanged = m_waterfallTextureHeight != m_waterfallHeight;

	if(fftSizeChanged) {
		if(m_waterfallBuffer != NULL) {
			delete m_waterfallBuffer;
			m_waterfallBuffer = NULL;
		}
		m_waterfallBuffer = new QImage(m_fftSize, 256, QImage::Format_ARGB32);
		if(m_waterfallBuffer != NULL) {
			m_waterfallBuffer->fill(qRgb(0x00, 0x00, 0x00));
			m_waterfallBufferPos = 0;
		} else {
			m_fftSize = 0;
			m_changesPending = true;
			return;
		}

		if(m_histogramBuffer != NULL) {
			delete m_histogramBuffer;
			m_histogramBuffer = NULL;
		}
		if(m_histogram != NULL) {
			delete[] m_histogram;
			m_histogram = NULL;
		}
		if(m_histogramHoldoff != NULL) {
			delete[] m_histogramHoldoff;
			m_histogramHoldoff = NULL;
		}

		m_histogramBuffer = new QImage(m_fftSize, 100, QImage::Format_RGB32);
		if(m_histogramBuffer != NULL) {
			m_histogramBuffer->fill(qRgb(0x00, 0x00, 0x00));
		} else {
			m_fftSize = 0;
			m_changesPending = true;
			return;
		}

		m_histogram = new quint8[100 * m_fftSize];
		memset(m_histogram, 0x00, 100 * m_fftSize);
		m_histogramHoldoff = new quint8[100 * m_fftSize];
		memset(m_histogramHoldoff, 0x07, 100 * m_fftSize);

		quint8* data = new quint8[m_fftSize * 100 * 4];
		memset(data, 0x00, m_fftSize * 100 * 4);
		glBindTexture(GL_TEXTURE_2D, m_histogramTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_fftSize, 100, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		delete[] data;
	}

	if(fftSizeChanged || windowSizeChanged) {
		m_waterfallTextureHeight = m_waterfallHeight;
		quint8* data = new quint8[m_fftSize * m_waterfallTextureHeight * 4];
		memset(data, 0x00, m_fftSize * m_waterfallTextureHeight * 4);
		glBindTexture(GL_TEXTURE_2D, m_waterfallTexture);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_fftSize, m_waterfallTextureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		delete[] data;
		m_waterfallTexturePos = 0;
	}

	m_changesPending = false;
}

void GLSpectrum::mouseMoveEvent(QMouseEvent* event)
{
	if(m_cursorState == CSSplitterMoving) {
		Real newShare = (Real)(event->y() - m_bottomMargin - m_topMargin) / (Real)height();
		if(newShare < 0.1)
			newShare = 0.1;
		else if(newShare > 0.8)
			newShare = 0.8;
		m_waterfallShare = newShare;
		m_changesPending = true;
		update();
		return;
	}

	if(((event->x() > m_leftMargin) && (event->x() < width() - m_rightMargin)) &&
		((event->y() > m_frequencyScaleTop) && (event->y() <= m_frequencyScaleTop + m_frequencyScaleHeight))) {
		if(m_cursorState == CSNormal) {
			setCursor(Qt::SizeVerCursor);
			m_cursorState = CSSplitter;
		}
	} else {
		if(m_cursorState == CSSplitter) {
			setCursor(Qt::ArrowCursor);
			m_cursorState = CSNormal;
		}
	}
}

void GLSpectrum::mousePressEvent(QMouseEvent* event)
{
	if(event->button() != 1)
		return;

	if(m_cursorState == CSSplitter) {
		grabMouse();
		m_cursorState = CSSplitterMoving;
	}
}

void GLSpectrum::mouseReleaseEvent(QMouseEvent*)
{
	if(m_cursorState == CSSplitterMoving) {
		releaseMouse();
		m_cursorState = CSSplitter;
	}
}

void GLSpectrum::tick()
{
	if(m_displayChanged) {
		m_displayChanged = false;
		update();
	}
}
