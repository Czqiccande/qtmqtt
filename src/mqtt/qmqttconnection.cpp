/******************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the QtMqtt module.
**
** $QT_BEGIN_LICENSE:COMM$
**
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** $QT_END_LICENSE$
**
******************************************************************************/

#include "qmqttconnection_p.h"
#include "qmqttcontrolpacket_p.h"
#include "qmqttsubscription_p.h"

#include <QtCore/QLoggingCategory>
#include <QtNetwork/QSslSocket>
#include <QtNetwork/QTcpSocket>
#if QT_VERSION >= QT_VERSION_CHECK(5, 10, 0)
#include <QtCore/QRandomGenerator>
#endif

#include <limits>
#include <cstdint>

QT_BEGIN_NAMESPACE

Q_LOGGING_CATEGORY(lcMqttConnection, "qt.mqtt.connection")
Q_LOGGING_CATEGORY(lcMqttConnectionVerbose, "qt.mqtt.connection.verbose");

QMqttConnection::QMqttConnection(QObject *parent) : QObject(parent)
{
    m_pingTimer.setSingleShot(false);
    m_pingTimer.connect(&m_pingTimer, &QTimer::timeout, this, &QMqttConnection::sendControlPingRequest);
}

QMqttConnection::~QMqttConnection()
{
    if (m_internalState == BrokerConnected)
        sendControlDisconnect();

    if (m_ownTransport && m_transport)
        delete m_transport;
}

void QMqttConnection::setTransport(QIODevice *device, QMqttClient::TransportType transport)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << device << " Type:" << transport;

    if (m_transport) {
        disconnect(m_transport, &QIODevice::aboutToClose, this, &QMqttConnection::transportConnectionClosed);
        disconnect(m_transport, &QIODevice::readyRead, this, &QMqttConnection::transportReadReady);
        if (m_ownTransport)
            delete m_transport;
    }

    m_transport = device;
    m_transportType = transport;
    m_ownTransport = false;

    connect(m_transport, &QIODevice::aboutToClose, this, &QMqttConnection::transportConnectionClosed);
    connect(m_transport, &QIODevice::readyRead, this, &QMqttConnection::transportReadReady);
}

QIODevice *QMqttConnection::transport() const
{
    return m_transport;
}

bool QMqttConnection::ensureTransport(bool createSecureIfNeeded)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << m_transport;

    if (m_transport)
        return true;

    // We are asked to create a transport layer
    if (m_client->hostname().isEmpty() || m_client->port() == 0) {
        qWarning("Trying to create a transport layer, but no hostname is specified");
        return false;
    }
    auto socket =
#ifndef QT_NO_SSL
            createSecureIfNeeded ? new QSslSocket() :
#endif
                                   new QTcpSocket();
    m_transport = socket;
    m_ownTransport = true;
    m_transportType = createSecureIfNeeded ? QMqttClient::SecureSocket : QMqttClient::AbstractSocket;

    connect(socket, &QAbstractSocket::disconnected, this, &QMqttConnection::transportConnectionClosed);
    connect(m_transport, &QIODevice::aboutToClose, this, &QMqttConnection::transportConnectionClosed);
    connect(m_transport, &QIODevice::readyRead, this, &QMqttConnection::transportReadReady);
    return true;
}

bool QMqttConnection::ensureTransportOpen(const QString &sslPeerName)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << m_transportType;

    if (m_transportType == QMqttClient::IODevice) {
        if (m_transport->isOpen())
            return true;

        if (m_transport->open(QIODevice::ReadWrite)) {
            qWarning("Could not open Transport IO device");
            return false;
        }
    } else if (m_transportType == QMqttClient::AbstractSocket) {
        auto socket = dynamic_cast<QTcpSocket*>(m_transport);
        Q_ASSERT(socket);
        if (socket->state() == QAbstractSocket::ConnectedState)
            return true;

        socket->connectToHost(m_client->hostname(), m_client->port());
        if (!socket->waitForConnected()) {
            qWarning("Could not establish socket connection for transport");
            return false;
        }
    }
#ifndef QT_NO_SSL
    else if (m_transportType == QMqttClient::SecureSocket) {
        auto socket = dynamic_cast<QSslSocket*>(m_transport);
        Q_ASSERT(socket);
        if (socket->state() == QAbstractSocket::ConnectedState)
            return true;

        socket->connectToHostEncrypted(m_client->hostname(), m_client->port(), sslPeerName);
        if (!socket->waitForConnected()) {
            qWarning("Could not establish socket connection for transport");
            return false;
        }

        if (!socket->waitForEncrypted()) {
            qWarning("Could not initiate encryption.");
            return false;
        }
    }
#else
    Q_UNUSED(sslPeerName);
#endif

    return true;
}

bool QMqttConnection::sendControlConnect()
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO;

    QMqttControlPacket packet(QMqttControlPacket::CONNECT);

    // Variable header
    // 3.1.2.1 Protocol Name
    // 3.1.2.2 Protocol Level
    const quint8 protocolVersion = m_client->protocolVersion();
    if (protocolVersion == 3) {
        packet.append("MQIsdp");
        packet.append(char(3)); // Version 3.1
    } else if (protocolVersion == 4) {
        packet.append("MQTT");
        packet.append(char(4)); // Version 3.1.1
    } else {
        qFatal("Illegal MQTT VERSION");
    }

    // 3.1.2.3 Connect Flags
    quint8 flags = 0;
    // Clean session
    if (m_client->cleanSession())
        flags |= 1 << 1;

    if (!m_client->willMessage().isEmpty()) {
        flags |= 1 << 2;
        if (m_client->willQoS() > 2) {
            qWarning("Will QoS does not have a valid value");
            return false;
        }
        if (m_client->willQoS() == 1)
            flags |= 1 << 3;
        else if (m_client->willQoS() == 2)
            flags |= 1 << 4;
        if (m_client->willRetain())
            flags |= 1 << 5;
    }
    if (m_client->username().size())
        flags |= 1 << 7;

    if (m_client->password().size())
        flags |= 1 << 6;

    packet.append(char(flags));

    // 3.1.2.10 Keep Alive
    packet.append(m_client->keepAlive());

    // 3.1.3 Payload
    // 3.1.3.1 Client Identifier
    // Client id maximum left is 23
    const QByteArray clientStringArray = m_client->clientId().left(23).toUtf8();
    if (clientStringArray.size()) {
        packet.append(clientStringArray);
    } else {
        packet.append(char(0));
        packet.append(char(0));
    }

    if (!m_client->willMessage().isEmpty()) {
        packet.append(m_client->willTopic().toUtf8());
        packet.append(m_client->willMessage());
    }

    if (m_client->username().size())
        packet.append(m_client->username().toUtf8());

    if (m_client->password().size())
        packet.append(m_client->password().toUtf8());

    if (!writePacketToTransport(packet)) {
        qWarning("Could not write CONNECT frame to transport");
        return false;
    }

    m_internalState = BrokerWaitForConnectAck;
    return true;
}

qint32 QMqttConnection::sendControlPublish(const QString &topic, const QByteArray &message, quint8 qos, bool retain)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << topic << " Size:" << message.size() << " bytes."
                              << "QoS:" << qos << " Retain:" << retain;

    if (topic.contains(QLatin1Char('#')) || topic.contains('+'))
        return -1;

    quint8 header = QMqttControlPacket::PUBLISH;
    if (qos == 1)
        header |= 0x02;
    else if (qos == 2)
        header |= 0x04;

    if (retain)
        header |= 0x01;

    QSharedPointer<QMqttControlPacket> packet(new QMqttControlPacket(header));

    QByteArray topicArray = topic.toUtf8();
    const std::uint16_t u16max = std::numeric_limits<std::uint16_t>::max();
    if (topicArray.size() > u16max) {
        qWarning("Published topic is too long. Need to truncate");
        topicArray.truncate(u16max);
    }

    packet->append(topicArray);
    quint16 identifier = 0;
    if (qos > 0) {
        // Add Packet Identifier
        static quint16 publishIdCounter = 0;
        if (publishIdCounter + 1 == u16max)
            publishIdCounter = 0;
        else
            publishIdCounter++;

        identifier = publishIdCounter;
        packet->append(identifier);
    }
    packet->appendRaw(message);

    if (qos)
        m_pendingMessages.insert(identifier, packet);

    const bool written = writePacketToTransport(*packet.data());

    if (!written)
        m_pendingMessages.remove(identifier);
    return written ? identifier : -1;
}

bool QMqttConnection::sendControlPublishAcknowledge(quint16 id)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << id;
    QMqttControlPacket packet(QMqttControlPacket::PUBACK);
    packet.append(id);
    return writePacketToTransport(packet);
}

bool QMqttConnection::sendControlPublishRelease(quint16 id)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << id;
    quint8 header = QMqttControlPacket::PUBREL;
    header |= 0x02; // MQTT-3.6.1-1

    QMqttControlPacket packet(header);
    packet.append(id);
    return writePacketToTransport(packet);
}

bool QMqttConnection::sendControlPublishReceive(quint16 id)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << id;
    QMqttControlPacket packet(QMqttControlPacket::PUBREC);
    packet.append(id);
    return writePacketToTransport(packet);
}

bool QMqttConnection::sendControlPublishComp(quint16 id)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << id;
    QMqttControlPacket packet(QMqttControlPacket::PUBCOMP);
    packet.append(id);
    return writePacketToTransport(packet);
}

QSharedPointer<QMqttSubscription> QMqttConnection::sendControlSubscribe(const QString &topic, quint8 qos)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << " Topic:" << topic << " qos:" << qos;

    if (m_activeSubscriptions.contains(topic))
        return m_activeSubscriptions[topic];

    // has to have 0010 as bits 3-0, maybe update SUBSCRIBE instead?
    // MQTT-3.8.1-1
    const quint8 header = QMqttControlPacket::SUBSCRIBE + 0x02;
    QMqttControlPacket packet(header);

    // Add Packet Identifier
    const quint16 identifier =
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
            qrand();
#else
            QRandomGenerator::get32();
#endif

    packet.append(identifier);

    // Overflow protection
    QByteArray topicArray = topic.toUtf8();
    if (topicArray.size() > std::numeric_limits<std::uint16_t>::max()) {
        qWarning("Subscribed topic is too long.");
        return QSharedPointer<QMqttSubscription>();
    }

    packet.append(topicArray);

    switch (qos) {
    case 0: packet.append(char(0x0)); break;
    case 1: packet.append(char(0x1)); break;
    case 2: packet.append(char(0x2)); break;
    default: return QSharedPointer<QMqttSubscription>();
    }

    QSharedPointer<QMqttSubscription> result(new QMqttSubscription);
    result->setTopic(QString::fromUtf8(topicArray));
    result->setClient(m_client);
    result->setQos(qos);
    result->setState(QMqttSubscription::SubscriptionPending);

    if (!writePacketToTransport(packet))
        return QSharedPointer<QMqttSubscription>();

    // SUBACK must contain identifier MQTT-3.8.4-2
    m_pendingSubscriptionAck.insert(identifier, result);
    m_activeSubscriptions.insert(result->topic(), result);
    return result;
}

bool QMqttConnection::sendControlUnsubscribe(const QString &topic)
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO << " Topic:" << topic;

    // MQTT-3.10.3-2
    if (topic.isEmpty())
        return false;

    if (!m_activeSubscriptions.contains(topic))
        return false;

    if (m_internalState != QMqttConnection::BrokerConnected) {
        m_activeSubscriptions.remove(topic);
        return true;
    }

    // has to have 0010 as bits 3-0, maybe update UNSUBSCRIBE instead?
    // MQTT-3.10.1-1
    const quint8 header = QMqttControlPacket::UNSUBSCRIBE + 0x02;
    QMqttControlPacket packet(header);

    // Add Packet Identifier
    const quint16 identifier =
#if QT_VERSION < QT_VERSION_CHECK(5, 10, 0)
            qrand();
#else
            QRandomGenerator::get32();
#endif

    packet.append(identifier);

    packet.append(topic.toUtf8());
    auto sub = m_activeSubscriptions[topic];
    sub->setState(QMqttSubscription::UnsubscriptionPending);

    if (!writePacketToTransport(packet))
        return false;

    // Do not remove from m_activeSubscriptions as there might be QoS1/2 messages to still
    // be sent before UNSUBSCRIBE is acknowledged.
    m_pendingUnsubscriptions.insert(identifier, sub);

    return true;
}

bool QMqttConnection::sendControlPingRequest()
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO;

    if (m_internalState != QMqttConnection::BrokerConnected)
        return false;

    const QMqttControlPacket packet(QMqttControlPacket::PINGREQ);
    if (!writePacketToTransport(packet)) {
        qWarning("Could not write DISCONNECT frame to transport");
        return false;
    }
    return true;
}

bool QMqttConnection::sendControlDisconnect()
{
    qCDebug(lcMqttConnection) << Q_FUNC_INFO;

    m_pingTimer.stop();

    for (auto sub : m_activeSubscriptions)
        sub->unsubscribe();
    m_activeSubscriptions.clear();

    const QMqttControlPacket packet(QMqttControlPacket::DISCONNECT);
    if (!writePacketToTransport(packet)) {
        qWarning("Could not write DISCONNECT frame to transport");
        return false;
    }
    m_internalState = BrokerDisconnected;

    if (m_transport->waitForBytesWritten(30000)) {
        // MQTT-3.14.4-1 must disconnect
        m_transport->close();
        return true;
    }
    return false;
}

void QMqttConnection::setClient(QMqttClient *client)
{
    m_client = client;
}

void QMqttConnection::transportConnectionClosed()
{
    m_readBuffer.clear();
    m_pingTimer.stop();
    m_client->setState(QMqttClient::Disconnected);
}

void QMqttConnection::transportReadReady()
{
    qCDebug(lcMqttConnectionVerbose) << Q_FUNC_INFO;
    m_readBuffer.append(m_transport->readAll());
    processData();
}

void QMqttConnection::readBuffer(char *data, qint64 size)
{
    memcpy(data, m_readBuffer.constData(), size);
    m_readBuffer = m_readBuffer.mid(size);
}

QByteArray QMqttConnection::readBuffer(qint64 size)
{
    QByteArray res = m_readBuffer.left(size);
    m_readBuffer = m_readBuffer.mid(size);
    return res;
}

void QMqttConnection::finalize_connack()
{
    qCDebug(lcMqttConnectionVerbose) << "Finalize CONNACK";
    quint8 ackFlags;
    readBuffer((char*)&ackFlags, 1);
    if (ackFlags > 1) { // MQTT-3.2.2.1
        qWarning("Unexpected CONNACK Flags set");
        // ## SET SOME ERROR
        return;
    }
    bool sessionPresent = ackFlags == 1;

    // MQTT-3.2.2-1 & MQTT-3.2.2-2
    if (sessionPresent) {
        emit m_client->brokerSessionRestored();
        if (m_client->cleanSession())
            qWarning("Connected with a clean session, ack contains session present");
    }

    quint8 connectResultValue;
    readBuffer((char*)&connectResultValue, 1);
    if (connectResultValue != 0) {
        qWarning("Connection has been rejected");
        // MQTT-3.2.2-5
        // ### TODO: ConnectionError
        m_readBuffer.clear();
        m_transport->close();
        m_internalState = BrokerDisconnected;
        m_client->setState(QMqttClient::Disconnected);
        return;
    }
    m_internalState = BrokerConnected;
    m_client->setState(QMqttClient::Connected);

    m_pingTimer.setInterval(m_client->keepAlive() * 1000);
    m_pingTimer.start();
}

void QMqttConnection::finalize_suback()
{
    quint16 id;
    readBuffer((char*)&id, 2);
    id = qFromBigEndian<quint16>(id);
    if (!m_pendingSubscriptionAck.contains(id)) {
        qWarning("Received SUBACK for unknown subscription request");
        return;
    }
    quint8 result;
    readBuffer((char*)&result, 1);
    auto sub = m_pendingSubscriptionAck.take(id);
    qCDebug(lcMqttConnectionVerbose) << "Finalize SUBACK: id:" << id << "qos:" << result;
    if (result <= 2) {
        // The broker might have a different support level for QoS than what
        // the client requested
        if (result != sub->qos()) {
            sub->setQos(result);
            emit sub->qosChanged(result);
        }
        sub->setState(QMqttSubscription::Subscribed);
    } else if (result == 0x80) {
        qWarning() << "Subscription for id " << id << " failed.";
        sub->setState(QMqttSubscription::Error);
    } else {
        qWarning("Received invalid SUBACK result value");
        sub->setState(QMqttSubscription::Error);
    }
}

void QMqttConnection::finalize_unsuback()
{
    quint16 id;
    readBuffer((char*)&id, 2);
    id = qFromBigEndian<quint16>(id);
    qCDebug(lcMqttConnectionVerbose) << "Finalize UNSUBACK: " << id;
    if (!m_pendingUnsubscriptions.contains(id)) {
        qWarning("Received UNSUBACK for unknown request");
        return;
    }
    auto sub = m_pendingUnsubscriptions.take(id);
    sub->setState(QMqttSubscription::Unsubscribed);
    m_activeSubscriptions.remove(sub->topic());
}

void QMqttConnection::finalize_publish()
{
    // String topic
    const quint16 topicLength = qFromBigEndian<quint16>(reinterpret_cast<const quint16 *>(readBuffer(2).constData()));
    const QString topic = QString::fromUtf8(reinterpret_cast<const char *>(readBuffer(topicLength).constData()));

    quint16 id = 0;
    if (m_currentPublish.qos > 0) {
        id = qFromBigEndian<quint16>(reinterpret_cast<const quint16 *>(readBuffer(2).constData()));
    }

    // message
    qint64 payloadLength = m_missingData - (topicLength + 2) - (m_currentPublish.qos > 0 ? 2 : 0);
    const QByteArray message = readBuffer(payloadLength);

    qCDebug(lcMqttConnectionVerbose) << "Finalize PUBLISH: topic:" << topic
                                     << " payloadLength:" << payloadLength;;

    emit m_client->messageReceived(message, topic);

    QMqttMessage qmsg(topic, message, id, m_currentPublish.qos,
                      m_currentPublish.dup, m_currentPublish.retain);

    for (auto sub = m_activeSubscriptions.constBegin(); sub != m_activeSubscriptions.constEnd(); sub++) {
        const QString subTopic = sub.key();

        if (subTopic == topic) {
            emit sub.value()->messageReceived(qmsg);
            continue;
        } else if (subTopic.endsWith(QLatin1Char('#')) && topic.startsWith(subTopic.leftRef(subTopic.size() - 1))) {
            emit sub.value()->messageReceived(qmsg);
            continue;
        }

        if (!subTopic.contains(QLatin1Char('+')))
            continue;

        const QVector<QStringRef> subTopicSplit = subTopic.splitRef(QLatin1Char('/'));
        const QVector<QStringRef> topicSplit = topic.splitRef(QLatin1Char('/'));
        if (subTopicSplit.size() != topicSplit.size())
            continue;
        bool match = true;
        for (int i = 0; i < subTopicSplit.size() && match; ++i) {
            if (subTopicSplit.at(i) == QLatin1Char('+') || subTopicSplit.at(i) == topicSplit.at(i))
                continue;
            match = false;
        }

        if (match) {
            emit sub.value()->messageReceived(qmsg);
        }
    }

    if (m_currentPublish.qos == 1)
        sendControlPublishAcknowledge(id);
    else if (m_currentPublish.qos == 2)
        sendControlPublishReceive(id);
}

void QMqttConnection::finalize_pubAckRecComp()
{
    qCDebug(lcMqttConnectionVerbose) << "Finalize PUBACK/REC/COMP";
    quint16 id;
    readBuffer((char*)&id, 2);
    id = qFromBigEndian<quint16>(id);

    if ((m_currentPacket & 0xF0) == QMqttControlPacket::PUBCOMP) {
        qCDebug(lcMqttConnectionVerbose) << " PUBCOMP:" << id;
        auto pendingRelease = m_pendingReleaseMessages.take(id);
        if (!pendingRelease)
            qWarning("Received PUBCOMP for unknown released message");
        emit m_client->messageSent(id);
        return;
    }

    auto pendingMsg = m_pendingMessages.take(id);
    if (!pendingMsg) {
        qWarning() << QLatin1String("Received PUBACK for unknown message: ") << id;
        return;
    }
    if ((m_currentPacket & 0xF0) == QMqttControlPacket::PUBREC) {
        qCDebug(lcMqttConnectionVerbose) << " PUBREC:" << id;
        m_pendingReleaseMessages.insert(id, pendingMsg);
        sendControlPublishRelease(id);
    } else {
        qCDebug(lcMqttConnectionVerbose) << " PUBACK:" << id;
        emit m_client->messageSent(id);
    }
}

void QMqttConnection::finalize_pubrel()
{
    quint16 id;
    readBuffer((char*)&id, 2);
    id = qFromBigEndian<quint16>(id);

    qCDebug(lcMqttConnectionVerbose) << "Finalize PUBREL:" << id;

    // ### TODO: send to our app now or not???
    // See standard Figure 4.3 Method A or B ???
    sendControlPublishComp(id);
}

void QMqttConnection::finalize_pingresp()
{
    qCDebug(lcMqttConnectionVerbose) << "Finalize PINGRESP";
    quint8 v;
    readBuffer((char*)&v, 1);
    if (v != 0)
        qWarning("Received a PINGRESP with payload!");
    emit m_client->pingResponseReceived();
}

void QMqttConnection::processData()
{
    if (m_missingData > 0) {
        if (m_readBuffer.size() < m_missingData)
            return;

        switch (m_currentPacket & 0xF0) {
        case QMqttControlPacket::CONNACK:
            finalize_connack();
            break;
        case QMqttControlPacket::SUBACK:
            finalize_suback();
            break;
        case QMqttControlPacket::UNSUBACK:
            finalize_unsuback();
            break;
        case QMqttControlPacket::PUBLISH:
            finalize_publish();
            break;
        case QMqttControlPacket::PUBACK:
        case QMqttControlPacket::PUBREC:
        case QMqttControlPacket::PUBCOMP:
            finalize_pubAckRecComp();
            break;
        case QMqttControlPacket::PINGRESP:
            finalize_pingresp();
            break;
        case QMqttControlPacket::PUBREL: {
            finalize_pubrel();
            break;
        }
        default:
            qFatal("Unknown packet to finalize");
            break;
        }
        m_missingData = 0;
    }

    if (m_readBuffer.size() == 0)
        return;

    readBuffer((char*)&m_currentPacket, 1);

    switch (m_currentPacket & 0xF0) {
    case QMqttControlPacket::CONNACK: {
        qCDebug(lcMqttConnectionVerbose) << "Received CONNACK";
        if (m_internalState != BrokerWaitForConnectAck) {
            qWarning("Received CONNACK at unexpected time!");
            break;
        }

        quint8 payloadSize;
        readBuffer((char*)&payloadSize, 1);
        if (payloadSize != 2) {
            qWarning("Unexpected FRAME size in CONNACK");
            // ## SET SOME ERROR
            break;
        }
        m_missingData = 2;
        break;
    }
    case QMqttControlPacket::SUBACK: {
        qCDebug(lcMqttConnectionVerbose) << "Received SUBACK";
        quint8 remaining;
        readBuffer((char*)&remaining, 1);
        m_missingData = remaining;
        break;
    }
    case QMqttControlPacket::PUBLISH: {
        qCDebug(lcMqttConnectionVerbose) << "Received PUBLISH";
        m_currentPublish.dup = m_currentPacket & 0x08;
        m_currentPublish.qos = (m_currentPacket & 0x06) >> 1;
        m_currentPublish.retain = m_currentPacket & 0x01;
        // remaining length
        quint32 multiplier = 1;
        quint32 msgLength = 0;
        quint8 b = 0;
        quint8 iteration = 0;
        do {
            readBuffer((char*)&b, 1);
            msgLength += (b & 127) * multiplier;
            multiplier *= 128;
            iteration++;
            if (iteration > 4)
                qFatal("Publish message is too big to handle");
        } while ((b & 128) != 0);
        m_missingData = msgLength;
        break;
    }
    case QMqttControlPacket::PINGRESP:
        qCDebug(lcMqttConnectionVerbose) << "Received PINGRESP";
        m_missingData = 1;
        break;
    case QMqttControlPacket::UNSUBACK:
    case QMqttControlPacket::PUBACK:
    case QMqttControlPacket::PUBREC:
    case QMqttControlPacket::PUBCOMP:
    case QMqttControlPacket::PUBREL: {
        qCDebug(lcMqttConnectionVerbose) << "Received UNSUBACK/PUBACK/PUBREC/PUBCOMP/PUBREL";
        char remaining;
        readBuffer(&remaining, 1); // ### TODO: verify this is 2
        if (remaining != 0x02)
            qWarning("Received 2 byte message with invalid remaining length");
        m_missingData = 2;
        break;
    }
    default:
        qFatal("Received unknown command");
        break;
    }

    /* set current command CONNACK - PINGRESP */
    /* read command size */
    /* calculate missing_data */
    processData(); // implicitly finishes and enqueues
    return;
}

bool QMqttConnection::writePacketToTransport(const QMqttControlPacket &p)
{
    const QByteArray writeData = p.serialize();
    const qint64 res = m_transport->write(writeData.constData(), writeData.size());
    if (Q_UNLIKELY(res == -1)) {
        qWarning("Could not write frame to transport");
        return false;
    }
    return true;
}

QT_END_NAMESPACE
