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
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QTcpServer>
#include <QUrlQuery>

#include "ui/AppController.h"
#include "ui/MapDocument.h"
#include "ui/MapWindow.h"
#include "ui/MapWindowManager.h"

#include "mdl/Brush.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "kd/overload.h"

#include "vm/bbox.h"
#include "vm/vec.h"

#include <algorithm>
#include <cstdint>
#include <optional>

namespace tb::ui
{
namespace
{
using StatusCode = QHttpServerResponse::StatusCode;

QHttpServerResponse jsonError(const QString& message, const StatusCode status)
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

MapDocument* findDocumentByHandle(AppController& appController, const quint64 handle)
{
  for (auto* window : appController.mapWindowManager().mapWindows())
  {
    if (window->document().id() == handle)
    {
      return &window->document();
    }
  }
  return nullptr;
}

// Resolution result for the required `doc` selector: either the resolved document or
// a ready-to-return error response (missing -> 409, malformed -> 400, unknown -> 404).
struct DocOrError
{
  MapDocument* document = nullptr;
  std::optional<QHttpServerResponse> error;
};

struct BodyAndDoc
{
  QJsonObject body;
  MapDocument* document = nullptr;
  std::optional<QHttpServerResponse> error;
};

DocOrError resolveDocFromQuery(
  AppController& appController, const QHttpServerRequest& request)
{
  const auto query = request.query();
  if (!query.hasQueryItem("doc"))
  {
    return {nullptr, jsonError("doc is required", StatusCode::Conflict)};
  }

  auto ok = false;
  const auto handle = query.queryItemValue("doc").toULongLong(&ok);
  if (!ok)
  {
    return {
      nullptr, jsonError("doc must be a non-negative integer", StatusCode::BadRequest)};
  }

  auto* document = findDocumentByHandle(appController, handle);
  if (!document)
  {
    return {
      nullptr,
      jsonError(
        QStringLiteral("no open document with handle %1").arg(handle),
        StatusCode::NotFound)};
  }
  return {document, std::nullopt};
}

BodyAndDoc resolveBodyAndDoc(
  AppController& appController, const QHttpServerRequest& request)
{
  auto parseError = QJsonParseError{};
  const auto json = QJsonDocument::fromJson(request.body(), &parseError);
  if (parseError.error != QJsonParseError::NoError)
  {
    return {
      {},
      nullptr,
      jsonError(
        QStringLiteral("invalid JSON: %1").arg(parseError.errorString()),
        StatusCode::BadRequest)};
  }
  if (!json.isObject())
  {
    return {
      {}, nullptr, jsonError("request body must be a JSON object", StatusCode::BadRequest)};
  }

  const auto body = json.object();
  const auto docValue = body.value("doc");
  if (docValue.isUndefined() || docValue.isNull())
  {
    return {{}, nullptr, jsonError("doc is required", StatusCode::Conflict)};
  }
  if (!docValue.isDouble())
  {
    return {{}, nullptr, jsonError("doc must be an integer", StatusCode::BadRequest)};
  }

  const auto handle = static_cast<quint64>(docValue.toInteger());
  auto* document = findDocumentByHandle(appController, handle);
  if (!document)
  {
    return {
      {},
      nullptr,
      jsonError(
        QStringLiteral("no open document with handle %1").arg(handle),
        StatusCode::NotFound)};
  }
  return {body, document, std::nullopt};
}

QString nodeTypeName(const mdl::Node& node)
{
  return node.accept(kdl::overload(
    [](const mdl::WorldNode&) { return QStringLiteral("world"); },
    [](const mdl::LayerNode&) { return QStringLiteral("layer"); },
    [](const mdl::GroupNode&) { return QStringLiteral("group"); },
    [](const mdl::EntityNode&) { return QStringLiteral("entity"); },
    [](const mdl::BrushNode&) { return QStringLiteral("brush"); },
    [](const mdl::PatchNode&) { return QStringLiteral("patch"); }));
}

const mdl::LayerNode* containingLayer(const mdl::Node& node)
{
  for (const auto* ancestor = node.parent(); ancestor != nullptr;
       ancestor = ancestor->parent())
  {
    if (const auto* layer = dynamic_cast<const mdl::LayerNode*>(ancestor))
    {
      return layer;
    }
  }
  return nullptr;
}

QJsonObject nodeSummaryJson(const mdl::Node& node)
{
  auto summary = QJsonObject{
    {"handle", static_cast<qint64>(node.id())},
    {"type", nodeTypeName(node)},
    {"bounds", boundsToJson(node.logicalBounds())},
    {"selected", node.selected()},
    {"visible", node.visible()},
    {"locked", node.locked()},
  };
  if (const auto* parent = node.parent())
  {
    summary["parent"] = static_cast<qint64>(parent->id());
  }
  if (const auto* layer = containingLayer(node))
  {
    summary["layer"] = static_cast<qint64>(layer->id());
  }

  node.accept(kdl::overload(
    [](const mdl::WorldNode&) {},
    [](const mdl::LayerNode&) {},
    [](const mdl::GroupNode&) {},
    [&](const mdl::EntityNode& entityNode) {
      summary["classname"] = QString::fromStdString(entityNode.entity().classname());
    },
    [&](const mdl::BrushNode& brushNode) {
      const auto& faces = brushNode.brush().faces();
      if (!faces.empty())
      {
        const auto& material = faces.front().attributes().materialName();
        const auto uniform =
          std::all_of(faces.begin(), faces.end(), [&](const auto& face) {
            return face.attributes().materialName() == material;
          });
        if (uniform)
        {
          summary["material"] = QString::fromStdString(material);
        }
      }
    },
    [](const mdl::PatchNode&) {}));
  return summary;
}

QJsonObject brushFaceJson(const mdl::BrushFace& face, const size_t index)
{
  const auto& attributes = face.attributes();
  const auto& points = face.points();
  return QJsonObject{
    {"index", static_cast<qint64>(index)},
    {"points",
     QJsonArray{vec3ToJson(points[0]), vec3ToJson(points[1]), vec3ToJson(points[2])}},
    {"normal", vec3ToJson(face.normal())},
    {"material", QString::fromStdString(attributes.materialName())},
    {"offset", QJsonArray{attributes.xOffset(), attributes.yOffset()}},
    {"scale", QJsonArray{attributes.xScale(), attributes.yScale()}},
    {"rotation", attributes.rotation()},
  };
}

QJsonObject nodeDetailJson(const mdl::Node& node)
{
  auto detail = nodeSummaryJson(node);

  node.accept(kdl::overload(
    [](const mdl::WorldNode&) {},
    [](const mdl::LayerNode&) {},
    [](const mdl::GroupNode&) {},
    [&](const mdl::EntityNode& entityNode) {
      const auto& entity = entityNode.entity();
      detail["origin"] = vec3ToJson(entity.origin());
      detail["pointEntity"] = entity.pointEntity();
      auto properties = QJsonObject{};
      for (const auto& property : entity.properties())
      {
        properties[QString::fromStdString(property.key())] =
          QString::fromStdString(property.value());
      }
      detail["properties"] = properties;
    },
    [&](const mdl::BrushNode& brushNode) {
      const auto& faces = brushNode.brush().faces();
      auto facesJson = QJsonArray{};
      for (size_t i = 0; i < faces.size(); ++i)
      {
        facesJson.append(brushFaceJson(faces[i], i));
      }
      detail["faces"] = facesJson;
    },
    [](const mdl::PatchNode&) {}));

  if (node.hasChildren())
  {
    auto children = QJsonArray{};
    for (const auto* child : node.children())
    {
      children.append(static_cast<qint64>(child->id()));
    }
    detail["children"] = children;
  }
  return detail;
}

void collectNodes(
  const mdl::Node& node, const std::optional<QString>& typeFilter, QJsonArray& out)
{
  const auto type = nodeTypeName(node);
  if (!typeFilter || *typeFilter == type)
  {
    out.append(QJsonObject{
      {"handle", static_cast<qint64>(node.id())},
      {"type", type},
    });
  }
  for (const auto* child : node.children())
  {
    collectNodes(*child, typeFilter, out);
  }
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

QHttpServerResponse handleGetSelection(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveDocFromQuery(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  const auto& map = resolved.document->map();
  const auto& selection = map.selection();

  auto nodes = QJsonArray{};
  for (const auto* node : selection.nodes)
  {
    nodes.append(static_cast<qint64>(node->id()));
  }

  auto faces = QJsonArray{};
  for (const auto& faceHandle : selection.brushFaces)
  {
    faces.append(QJsonObject{
      {"brush", static_cast<qint64>(faceHandle.node()->id())},
      {"face", static_cast<qint64>(faceHandle.faceIndex())},
    });
  }

  return QHttpServerResponse{QJsonObject{{"nodes", nodes}, {"faces", faces}}};
}

QHttpServerResponse handleGetNodes(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveDocFromQuery(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  const auto& map = resolved.document->map();

  const auto query = request.query();
  const auto typeFilter = query.hasQueryItem("type")
                            ? std::optional<QString>{query.queryItemValue("type")}
                            : std::nullopt;

  auto nodes = QJsonArray{};
  collectNodes(map.worldNode(), typeFilter, nodes);
  return QHttpServerResponse{QJsonObject{{"nodes", nodes}}};
}

QHttpServerResponse handleNodesGet(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  const auto& map = resolved.document->map();
  const auto full = resolved.body.value("detail").toString() == QStringLiteral("full");

  auto nodes = QJsonArray{};
  auto missing = QJsonArray{};
  for (const auto& handleValue : resolved.body.value("handles").toArray())
  {
    const auto handle = static_cast<std::uint64_t>(handleValue.toInteger());
    if (const auto* node = map.findNodeById(handle))
    {
      nodes.append(full ? nodeDetailJson(*node) : nodeSummaryJson(*node));
    }
    else
    {
      missing.append(static_cast<qint64>(handle));
    }
  }
  return QHttpServerResponse{QJsonObject{{"nodes", nodes}, {"missing", missing}}};
}

QHttpServerResponse handleHandlesValidate(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  const auto& map = resolved.document->map();

  auto valid = QJsonArray{};
  auto invalid = QJsonArray{};
  for (const auto& handleValue : resolved.body.value("handles").toArray())
  {
    const auto handle = static_cast<std::uint64_t>(handleValue.toInteger());
    if (map.findNodeById(handle))
    {
      valid.append(static_cast<qint64>(handle));
    }
    else
    {
      invalid.append(static_cast<qint64>(handle));
    }
  }
  return QHttpServerResponse{QJsonObject{{"valid", valid}, {"invalid", invalid}}};
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

  m_httpServer->route(
    "/selection",
    QHttpServerRequest::Method::Get,
    [this](const QHttpServerRequest& request) {
      return handleGetSelection(m_appController, request);
    });

  m_httpServer->route(
    "/nodes", QHttpServerRequest::Method::Get, [this](const QHttpServerRequest& request) {
      return handleGetNodes(m_appController, request);
    });

  m_httpServer->route(
    "/nodes/get",
    QHttpServerRequest::Method::Post,
    [this](const QHttpServerRequest& request) {
      return handleNodesGet(m_appController, request);
    });

  m_httpServer->route(
    "/handles/validate",
    QHttpServerRequest::Method::Post,
    [this](const QHttpServerRequest& request) {
      return handleHandlesValidate(m_appController, request);
    });
}

} // namespace tb::ui
