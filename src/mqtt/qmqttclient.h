/****************************************************************************
**
** Copyright (C) 2017 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Mqtt module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:GPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 3 or (at your option) any later version
** approved by the KDE Free Qt Foundation. The licenses are as published by
** the Free Software Foundation and appearing in the file LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#ifndef QTMQTTCLIENT_H
#define QTMQTTCLIENT_H

#include <QtMqtt/qmqttglobal.h>

#include <QObject>
#include <QtCore/QIODevice>

QT_BEGIN_NAMESPACE

class QMqttClientPrivate;

class Q_MQTT_EXPORT QMqttClient : public QObject
{
public:
    enum TransportType {
        IODevice = 0,
        AbstractSocket
    };
private:
    Q_OBJECT
    Q_PROPERTY(QString clientId READ clientId WRITE setClientId NOTIFY clientIdChanged)
    Q_PROPERTY(QString hostname READ hostname WRITE setHostname NOTIFY hostnameChanged)
    Q_PROPERTY(quint16 keepAlive READ keepAlive WRITE setKeepAlive NOTIFY keepAliveChanged)
    Q_PROPERTY(quint16 port READ port WRITE setPort NOTIFY portChanged)
    Q_PROPERTY(quint8 protocolVersion READ protocolVersion WRITE setProtocolVersion NOTIFY protocolVersionChanged)
public:
    explicit QMqttClient(QObject *parent = 0);

    void setTransport(QIODevice *device, TransportType transport);
    QIODevice *transport() const;

    bool subscribe(const QString& topic);
    void unsubscribe(const QString& topic);

    //bool publish(const QString& message);

    QString hostname() const;
    quint16 port() const;
    QString clientId() const;
    quint16 keepAlive() const;
    quint8 protocolVersion() const;

    void connectToHost();
    void disconnectFromHost();

signals:
    void connected();
    void disconnected();
    void error();
    void subscribed();
    void unsubscribed();

    void hostnameChanged(QString hostname);
    void portChanged(quint16 port);
    void clientIdChanged(QString clientId);
    void keepAliveChanged(quint16 keepAlive);
    void protocolVersionChanged(quint8 protocolVersion);

public slots:
    void setHostname(QString hostname);
    void setPort(quint16 port);
    void setClientId(QString clientId);
    void setKeepAlive(quint16 keepAlive);
    void setProtocolVersion(quint8 protocolVersion);

private:
    Q_DECLARE_PRIVATE(QMqttClient)
};

QT_END_NAMESPACE

#endif // QTMQTTCLIENT_H