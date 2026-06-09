/*
 Copyright (C) 2025 Kristian Duske

 This file is part of TrenchBroom.

 TrenchBroom is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 TrenchBroom is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with TrenchBroom. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ui/ApiServer.h"

#include <QDebug>
#include <QHostAddress>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTcpServer>

#include "ui/AppController.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"

#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Node.h"
#include "mdl/WorldNode.h"

#include "vm/bbox.h"
#include "vm/vec.h"

namespace tb::ui
{
namespace
{
using StatusCode = QHttpServerResponse::StatusCode;

[[maybe_unused]] QHttpServerResponse jsonError(
  const QString& message, const StatusCode status)
{
  return QHttpServerResponse{QJsonObject{{"error", message}}, status};
}

QJsonArray vec3ToJson(const vm::vec3d& v)
{
  return QJsonArray{v[0], v[1], v[2]};
}

QJsonObject boundsToJson(const vm::bbox3d& bounds)
{
  return QJsonObject{
    {"min", vec3ToJson(bounds.min)},
    {"max", vec3ToJson(bounds.max)},
  };
}

QJsonObject documentToJson(const MapDocument& document, const bool active)
{
  const auto& map = document.map();
  const auto& worldNode = map.worldNode();

  auto layers = QJsonArray{};
  for (const auto* layerNode : worldNode.allLayers())
  {
    layers.append(QJsonObject{
      {"handle", static_cast<qint64>(layerNode->id())},
      {"name", QString::fromStdString(layerNode->name())},
    });
  }

  const auto pathValue =
    map.persistent() ? QJsonValue{QString::fromStdString(map.path().generic_string())}
                     : QJsonValue{QJsonValue::Null};

  return QJsonObject{
    {"handle", static_cast<qint64>(document.id())},
    {"active", active},
    {"name", QString::fromStdString(map.path().filename().generic_string())},
    {"path", pathValue},
    {"mapFormat", QString::fromStdString(mdl::formatName(worldNode.mapFormat()))},
    {"worldBounds", boundsToJson(map.worldBounds())},
    {"modified", map.modified()},
    {"layers", layers},
    {"nodeCount", static_cast<qint64>(worldNode.familySize())},
  };
}

QHttpServerResponse handleGetDocuments(AppController& appController)
{
  // mapWindows() is focus-ordered, so the front window is the active document.
  const auto windows = appController.mapWindowManager().mapWindows();
  auto documents = QJsonArray{};
  auto active = true;
  for (auto* window : windows)
  {
    documents.append(documentToJson(window->document(), active));
    active = false;
  }
  return QHttpServerResponse{documents};
}

} // namespace

ApiServer::ApiServer(AppController& appController, QObject* parent)
  : QObject{parent}
  , m_appController{appController}
{
}

ApiServer::~ApiServer() = default;

quint16 ApiServer::start(const quint16 port)
{
  m_tcpServer = new QTcpServer{this};
  if (!m_tcpServer->listen(QHostAddress::LocalHost, port))
  {
    qWarning() << "TrenchBroom API server: failed to listen on 127.0.0.1 port" << port
               << ":" << m_tcpServer->errorString();
    delete m_tcpServer;
    m_tcpServer = nullptr;
    return 0;
  }

  m_httpServer = new QHttpServer{this};
  registerRoutes();

  if (!m_httpServer->bind(m_tcpServer))
  {
    qWarning() << "TrenchBroom API server: failed to bind QHttpServer to the TCP server";
    return 0;
  }

  m_port = m_tcpServer->serverPort();
  qInfo().noquote() << QStringLiteral("TrenchBroom API server listening on "
                                      "http://127.0.0.1:%1")
                         .arg(m_port);
  return m_port;
}

quint16 ApiServer::port() const
{
  return m_port;
}

void ApiServer::registerRoutes()
{
  m_httpServer->route("/documents", QHttpServerRequest::Method::Get, [this]() {
    return handleGetDocuments(m_appController);
  });
}

} // namespace tb::ui
