/* Webcamod, webcam capture plasmoid.
 * Copyright (C) 2011-2013  Gonzalo Exequiel Pedone
 *
 * Webcamod is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Webcamod is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Webcamod. If not, see <http://www.gnu.org/licenses/>.
 *
 * Email     : hipersayan DOT x AT gmail DOT com
 * Web-Site 1: http://github.com/hipersayanX/Webcamoid
 * Web-Site 2: http://kde-apps.org/content/show.php/Webcamoid?content=144796
 */

#include "audiooutputelement.h"

AudioOutputElement::AudioOutputElement(): QbElement()
{
    this->m_outputDevice = NULL;
    this->m_timeDrift = 0;
    this->m_streamId = -1;
    this->m_audioDeviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    this->m_convert = Qb::create("ACapsConvert");
    this->resetBufferSize();

    QObject::connect(this->m_convert.data(),
                     SIGNAL(oStream(const QbPacket &)),
                     this,
                     SLOT(processFrame(const QbPacket &)),
                     Qt::DirectConnection);

    QObject::connect(this,
                     SIGNAL(stateChanged(QbElement::ElementState)),
                     this->m_convert.data(),
                     SLOT(setState(QbElement::ElementState)));

    QObject::connect(&this->m_audioBuffer,
                     SIGNAL(cleared()),
                     this,
                     SLOT(releaseInput()));
}

AudioOutputElement::~AudioOutputElement()
{
    this->uninit();
}

int AudioOutputElement::bufferSize() const
{
    return this->m_bufferSize;
}

QString AudioOutputElement::inputCaps() const
{
    return this->m_inputCaps;
}

double AudioOutputElement::clock() const
{
    return this->hwClock() + this->m_timeDrift;
}

QbCaps AudioOutputElement::findBestOptions(const QbCaps &caps,
                                           const QAudioDeviceInfo &deviceInfo,
                                           QAudioFormat *bestOption) const
{
    QMap<AVSampleFormat, QAudioFormat::SampleType> formatToType;
    formatToType[AV_SAMPLE_FMT_NONE] = QAudioFormat::Unknown;
    formatToType[AV_SAMPLE_FMT_U8] = QAudioFormat::UnSignedInt;
    formatToType[AV_SAMPLE_FMT_S16] = QAudioFormat::SignedInt;
    formatToType[AV_SAMPLE_FMT_S32] = QAudioFormat::SignedInt;
    formatToType[AV_SAMPLE_FMT_FLT] = QAudioFormat::Float;
    formatToType[AV_SAMPLE_FMT_DBL] = QAudioFormat::Float;
    formatToType[AV_SAMPLE_FMT_U8P] = QAudioFormat::UnSignedInt;
    formatToType[AV_SAMPLE_FMT_S16P] = QAudioFormat::SignedInt;
    formatToType[AV_SAMPLE_FMT_S32P] = QAudioFormat::SignedInt;
    formatToType[AV_SAMPLE_FMT_FLTP] = QAudioFormat::Float;
    formatToType[AV_SAMPLE_FMT_DBLP] = QAudioFormat::Float;

    QAudioFormat bestAudioFormat;

    if (caps.mimeType() == "audio/x-raw") {
        AVSampleFormat sampleFormat = av_get_sample_fmt(caps.property("format")
                                                            .toString()
                                                            .toStdString()
                                                            .c_str());

        QAudioFormat preferredAudioFormat;
        preferredAudioFormat.setByteOrder(QAudioFormat::BigEndian);
        preferredAudioFormat.setChannelCount(caps.property("channels").toInt());
        preferredAudioFormat.setCodec("audio/pcm");
        preferredAudioFormat.setSampleRate(caps.property("rate").toInt());
        preferredAudioFormat.setSampleSize(8 * caps.property("bps").toInt());
        preferredAudioFormat.setSampleType(formatToType[sampleFormat]);

        bestAudioFormat = deviceInfo.nearestFormat(preferredAudioFormat);
    }
    else
        bestAudioFormat = deviceInfo.preferredFormat();

    AVSampleFormat oFormat = AV_SAMPLE_FMT_NONE;

    foreach (AVSampleFormat format, formatToType.keys(bestAudioFormat.sampleType()))
        if (av_get_bytes_per_sample(format) == (bestAudioFormat.sampleSize() >> 3)) {
            oFormat = format;

            break;
        }

    char layout[256];
    int64_t channelLayout = av_get_default_channel_layout(bestAudioFormat.channelCount());

    av_get_channel_layout_string(layout,
                                 sizeof(layout),
                                 bestAudioFormat.channelCount(),
                                 channelLayout);

    QbCaps oCaps(QString("audio/x-raw,"
                         "format=%1,"
                         "bps=%2,"
                         "channels=%3,"
                         "rate=%4,"
                         "layout=%5,"
                         "align=%6").arg(av_get_sample_fmt_name(oFormat))
                                    .arg(bestAudioFormat.sampleSize() >> 3)
                                    .arg(bestAudioFormat.channelCount())
                                    .arg(bestAudioFormat.sampleRate())
                                    .arg(layout)
                                    .arg(false));

    if (bestOption)
        *bestOption = bestAudioFormat;

    return oCaps;
}

double AudioOutputElement::hwClock() const
{
    if (!this->m_audioOutput)
        return 0;

    int bytesInBuffer = this->m_audioOutput->bufferSize()
                        - this->m_audioOutput->bytesFree();

    int sampleSize = this->m_audioOutput->format().sampleSize();
    int channels = this->m_audioOutput->format().channelCount();
    int sampleRate = this->m_audioOutput->format().sampleRate();

    double usInBuffer = 8.0 * bytesInBuffer
                        / (sampleSize
                           * channels
                           * sampleRate);

    double pts = this->m_audioOutput->processedUSecs()
                 / 1.0e6
                 - usInBuffer;

    return pts;
}

bool AudioOutputElement::init()
{
    QAudioDeviceInfo audioDeviceInfo = QAudioDeviceInfo::defaultOutputDevice();
    QAudioFormat outputFormat;

    QbCaps bestCaps = this->findBestOptions(this->m_inputCaps,
                                            audioDeviceInfo,
                                            &outputFormat);

    this->m_convert->setProperty("caps", bestCaps.toString());

    this->m_audioOutput = AudioOutputPtr(new QAudioOutput(audioDeviceInfo,
                                                          outputFormat));

    if (this->m_audioOutput) {
        this->m_timeDrift = 0;
        int bps = bestCaps.property("bps").toInt();
        int channels = bestCaps.property("channels").toInt();
        qint64 bufferSize = bps * channels * this->m_bufferSize;

        this->m_audioOutput->setBufferSize(bufferSize);
        this->m_audioBuffer.setMaxSize(bufferSize);
        this->m_audioBuffer.open(QIODevice::ReadWrite);
        this->m_audioOutput->start(&this->m_audioBuffer);
    }

    return this->m_outputDevice? true: false;
}

void AudioOutputElement::uninit()
{
    this->m_mutex.lock();
    this->m_bufferEmpty.wakeAll();
    this->m_mutex.unlock();

    if (this->m_audioOutput) {
        this->m_audioOutput->stop();
        this->m_audioOutput.clear();
        this->m_outputDevice = NULL;
    }

    this->m_audioBuffer.close();
}

void AudioOutputElement::stateChange(QbElement::ElementState from,
                                     QbElement::ElementState to)
{
    if (from == QbElement::ElementStateNull
        && to == QbElement::ElementStatePaused)
        this->init();
    else if (from == QbElement::ElementStatePaused
             && to == QbElement::ElementStateNull)
        this->uninit();
}

void AudioOutputElement::setBufferSize(int bufferSize)
{
    this->m_bufferSize = bufferSize;
}

void AudioOutputElement::setInputCaps(const QString &inputCaps)
{
    this->m_inputCaps = inputCaps;
}

void AudioOutputElement::resetBufferSize()
{
    this->setBufferSize(32024);
}

void AudioOutputElement::resetInputCaps()
{
    this->m_inputCaps = "";
}

void AudioOutputElement::processFrame(const QbPacket &packet)
{
    this->m_audioBuffer.write(const_cast<char *>(packet.buffer().data()), packet.bufferSize());
}

void AudioOutputElement::releaseInput()
{
    this->m_mutex.lock();
    this->m_bufferEmpty.wakeAll();
    this->m_mutex.unlock();
}

void AudioOutputElement::iStream(const QbPacket &packet)
{
    if (packet.caps().mimeType() != "audio/x-raw"
        || !this->m_audioOutput)
        return;

    if (packet.id() != this->m_streamId) {
        this->m_mutex.lock();

        if (this->m_audioBuffer.size() > 0)
            this->m_bufferEmpty.wait(&this->m_mutex);

        this->m_mutex.unlock();
        this->m_streamId = packet.id();

        this->m_timeDrift = packet.pts()
                            * packet.timeBase().value()
                            - this->hwClock();
    }

    emit this->elapsedTime(this->clock());

    this->m_convert->iStream(packet);
}
