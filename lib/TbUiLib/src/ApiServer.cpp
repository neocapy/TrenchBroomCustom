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
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityProperties.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Picking.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/WorldNode.h"

#include "gl/Material.h"
#include "gl/MaterialCollection.h"
#include "gl/MaterialManager.h"

#include "kd/overload.h"

#include "vm/bbox.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <unordered_set>

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

std::optional<vm::vec3d> vec3FromJson(const QJsonValue& value)
{
  if (!value.isArray())
  {
    return std::nullopt;
  }
  const auto array = value.toArray();
  if (
    array.size() != 3 || !array[0].isDouble() || !array[1].isDouble()
    || !array[2].isDouble())
  {
    return std::nullopt;
  }
  return vm::vec3d{array[0].toDouble(), array[1].toDouble(), array[2].toDouble()};
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

QHttpServerResponse handleGetMaterials(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveDocFromQuery(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  const auto& map = resolved.document->map();

  auto collections = QJsonArray{};
  for (const auto& collection : map.materialManager().collections())
  {
    auto materials = QJsonArray{};
    for (const auto& material : collection.materials())
    {
      materials.append(QString::fromStdString(material.name()));
    }
    collections.append(QJsonObject{
      {"name", QString::fromStdString(collection.path().generic_string())},
      {"materials", materials},
    });
  }
  return QHttpServerResponse{QJsonObject{{"collections", collections}}};
}

QHttpServerResponse handleGetEntityClasses(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveDocFromQuery(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  const auto& map = resolved.document->map();

  auto classes = QJsonArray{};
  for (const auto& definition : map.entityDefinitionManager().definitions())
  {
    const auto point = getType(definition) == mdl::EntityDefinitionType::Point;
    auto classJson = QJsonObject{
      {"classname", QString::fromStdString(definition.name)},
      {"type", point ? QStringLiteral("point") : QStringLiteral("brush")},
    };
    if (!definition.description.empty())
    {
      classJson["description"] = QString::fromStdString(definition.description);
    }
    if (definition.pointEntityDefinition)
    {
      classJson["bounds"] = boundsToJson(definition.pointEntityDefinition->bounds);
    }
    classes.append(classJson);
  }
  return QHttpServerResponse{QJsonObject{{"classes", classes}}};
}

QHttpServerResponse handleContains(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  auto& map = resolved.document->map();

  const auto pointsValue = resolved.body.value("points");
  if (!pointsValue.isArray())
  {
    return jsonError("points must be an array of Vec3", StatusCode::BadRequest);
  }

  auto results = QJsonArray{};
  for (const auto& pointValue : pointsValue.toArray())
  {
    const auto point = vec3FromJson(pointValue);
    if (!point)
    {
      return jsonError(
        "each point must be an array of three numbers", StatusCode::BadRequest);
    }

    auto handles = QJsonArray{};
    for (const auto* node : mdl::findNodesContaining(map, *point))
    {
      handles.append(static_cast<qint64>(node->id()));
    }
    results.append(QJsonObject{{"point", vec3ToJson(*point)}, {"handles", handles}});
  }
  return QHttpServerResponse{QJsonObject{{"results", results}}};
}

QJsonObject rayHitJson(const mdl::Hit& hit)
{
  auto json = QJsonObject{
    {"point", vec3ToJson(hit.hitPoint())},
    {"distance", hit.distance()},
  };
  if (const auto* node = mdl::hitToNode(hit))
  {
    json["handle"] = static_cast<qint64>(node->id());
  }
  if (const auto faceHandle = mdl::hitToFaceHandle(hit))
  {
    json["face"] = static_cast<qint64>(faceHandle->faceIndex());
    json["normal"] = vec3ToJson(faceHandle->face().normal());
    json["material"] =
      QString::fromStdString(faceHandle->face().attributes().materialName());
  }
  return json;
}

QHttpServerResponse handleRaycast(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  auto& map = resolved.document->map();

  const auto raysValue = resolved.body.value("rays");
  if (!raysValue.isArray())
  {
    return jsonError("rays must be an array", StatusCode::BadRequest);
  }

  auto results = QJsonArray{};
  for (const auto& rayValue : raysValue.toArray())
  {
    if (!rayValue.isObject())
    {
      return jsonError("each ray must be an object", StatusCode::BadRequest);
    }
    const auto rayJson = rayValue.toObject();

    const auto origin = vec3FromJson(rayJson.value("origin"));
    const auto direction = vec3FromJson(rayJson.value("direction"));
    if (!origin || !direction)
    {
      return jsonError(
        "each ray must have Vec3 origin and direction", StatusCode::BadRequest);
    }
    if (vm::is_zero(*direction, vm::Cd::almost_zero()))
    {
      return jsonError("ray direction must not be zero", StatusCode::BadRequest);
    }

    const auto maxDistanceValue = rayJson.value("maxDistance");
    const auto maxDistance = maxDistanceValue.isDouble()
                               ? std::optional{maxDistanceValue.toDouble()}
                               : std::nullopt;

    auto ignore = std::unordered_set<std::uint64_t>{};
    for (const auto& handleValue : rayJson.value("ignore").toArray())
    {
      ignore.insert(static_cast<std::uint64_t>(handleValue.toInteger()));
    }

    const auto filter = mdl::HitFilters::type(mdl::nodeHitType())
                        && mdl::HitFilter{[&](const mdl::Hit& hit) {
                             if (maxDistance && hit.distance() > *maxDistance)
                             {
                               return false;
                             }
                             const auto* node = mdl::hitToNode(hit);
                             return !node || !ignore.contains(node->id());
                           }};

    // PickResult::byDistance keeps hits sorted on insertion, so all(filter) is
    // already front to back.
    auto pickResult = mdl::PickResult::byDistance();
    mdl::pick(map, vm::ray3d{*origin, vm::normalize(*direction)}, pickResult);

    auto hits = QJsonArray{};
    for (const auto& hit : pickResult.all(filter))
    {
      hits.append(rayHitJson(hit));
    }
    results.append(hits);
  }
  return QHttpServerResponse{QJsonObject{{"results", results}}};
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

  m_httpServer->route(
    "/materials",
    QHttpServerRequest::Method::Get,
    [this](const QHttpServerRequest& request) {
      return handleGetMaterials(m_appController, request);
    });

  m_httpServer->route(
    "/entityclasses",
    QHttpServerRequest::Method::Get,
    [this](const QHttpServerRequest& request) {
      return handleGetEntityClasses(m_appController, request);
    });

  m_httpServer->route(
    "/contains",
    QHttpServerRequest::Method::Post,
    [this](const QHttpServerRequest& request) {
      return handleContains(m_appController, request);
    });

  m_httpServer->route(
    "/raycast",
    QHttpServerRequest::Method::Post,
    [this](const QHttpServerRequest& request) {
      return handleRaycast(m_appController, request);
    });
}

} // namespace tb::ui
