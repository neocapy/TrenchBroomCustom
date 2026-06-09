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

#include "mdl/ApplyAndSwap.h"
#include "mdl/Brush.h"
#include "mdl/BrushBuilder.h"
#include "mdl/BrushFace.h"
#include "mdl/BrushFaceAttributes.h"
#include "mdl/BrushFaceHandle.h"
#include "mdl/BrushNode.h"
#include "mdl/Entity.h"
#include "mdl/EntityDefinition.h"
#include "mdl/EntityDefinitionManager.h"
#include "mdl/EntityNode.h"
#include "mdl/EntityNodeBase.h"
#include "mdl/EntityProperties.h"
#include "mdl/GameConfig.h"
#include "mdl/GameInfo.h"
#include "mdl/GroupNode.h"
#include "mdl/Hit.h"
#include "mdl/HitAdapter.h"
#include "mdl/HitFilter.h"
#include "mdl/LayerNode.h"
#include "mdl/Map.h"
#include "mdl/MapFormat.h"
#include "mdl/Map_Geometry.h"
#include "mdl/Map_Nodes.h"
#include "mdl/Map_Picking.h"
#include "mdl/Map_Selection.h"
#include "mdl/ModelUtils.h"
#include "mdl/Node.h"
#include "mdl/NodeContents.h"
#include "mdl/PatchNode.h"
#include "mdl/PickResult.h"
#include "mdl/Selection.h"
#include "mdl/Transaction.h"
#include "mdl/UpdateBrushFaceAttributes.h"
#include "mdl/WorldNode.h"

#include "gl/Material.h"
#include "gl/MaterialCollection.h"
#include "gl/MaterialManager.h"

#include "kd/overload.h"

#include "vm/bbox.h"
#include "vm/mat.h"
#include "vm/mat_ext.h"
#include "vm/ray.h"
#include "vm/vec.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_set>
#include <variant>
#include <vector>

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
    [&](const mdl::WorldNode& worldNode) {
      summary["classname"] = QString::fromStdString(worldNode.entity().classname());
    },
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

QJsonObject entityPropertiesJson(const mdl::Entity& entity)
{
  auto properties = QJsonObject{};
  for (const auto& property : entity.properties())
  {
    properties[QString::fromStdString(property.key())] =
      QString::fromStdString(property.value());
  }
  return properties;
}

QJsonObject nodeDetailJson(const mdl::Node& node)
{
  auto detail = nodeSummaryJson(node);

  node.accept(kdl::overload(
    [&](const mdl::WorldNode& worldNode) {
      detail["properties"] = entityPropertiesJson(worldNode.entity());
    },
    [](const mdl::LayerNode&) {},
    [](const mdl::GroupNode&) {},
    [&](const mdl::EntityNode& entityNode) {
      const auto& entity = entityNode.entity();
      detail["origin"] = vec3ToJson(entity.origin());
      detail["pointEntity"] = entity.pointEntity();
      detail["properties"] = entityPropertiesJson(entity);
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

// ---- POST /edit ----
//
// The batch executor. Ops run in order inside one transaction; `as: "<name>"` binds an
// op's resulting handle(s), and later ops reference them as "@name" wherever a handle
// is accepted.

using RefValue = std::variant<std::uint64_t, std::vector<std::uint64_t>>;

struct EditContext
{
  mdl::Map& map;
  std::map<QString, RefValue> refs;
};

// Resolves a HandleOrRef to a live node. On failure, sets `error` and returns null.
mdl::Node* resolveNode(EditContext& ctx, const QJsonValue& value, QString& error)
{
  const auto fromId = [&](const std::uint64_t id) -> mdl::Node* {
    if (auto* node = ctx.map.findNodeById(id))
    {
      return node;
    }
    error = QStringLiteral("stale handle %1").arg(id);
    return nullptr;
  };

  if (value.isDouble())
  {
    return fromId(static_cast<std::uint64_t>(value.toInteger()));
  }
  if (value.isString())
  {
    const auto name = value.toString();
    if (!name.startsWith('@'))
    {
      error = QStringLiteral("expected a handle or \"@ref\", got \"%1\"").arg(name);
      return nullptr;
    }
    const auto it = ctx.refs.find(name.mid(1));
    if (it == ctx.refs.end())
    {
      error = QStringLiteral("unknown ref \"%1\"").arg(name);
      return nullptr;
    }
    if (const auto* id = std::get_if<std::uint64_t>(&it->second))
    {
      return fromId(*id);
    }
    // a list ref is acceptable where a single handle is required iff it has exactly
    // one element
    const auto& ids = std::get<std::vector<std::uint64_t>>(it->second);
    if (ids.size() == 1)
    {
      return fromId(ids.front());
    }
    error = QStringLiteral("ref \"%1\" names a list of %2 handles, but a single handle "
                           "is required here")
              .arg(name)
              .arg(ids.size());
    return nullptr;
  }
  error = QStringLiteral("expected a handle or \"@ref\"");
  return nullptr;
}

// Resolves an array of HandleOrRefs; a "@ref" bound to a list expands in place.
std::optional<std::vector<mdl::Node*>> resolveNodeList(
  EditContext& ctx, const QJsonValue& value, QString& error)
{
  if (!value.isArray())
  {
    error = QStringLiteral("expected an array of handles");
    return std::nullopt;
  }

  auto nodes = std::vector<mdl::Node*>{};
  for (const auto& element : value.toArray())
  {
    if (element.isString())
    {
      const auto name = element.toString();
      if (name.startsWith('@'))
      {
        const auto it = ctx.refs.find(name.mid(1));
        if (it == ctx.refs.end())
        {
          error = QStringLiteral("unknown ref \"%1\"").arg(name);
          return std::nullopt;
        }
        if (const auto* ids = std::get_if<std::vector<std::uint64_t>>(&it->second))
        {
          for (const auto id : *ids)
          {
            if (auto* node = ctx.map.findNodeById(id))
            {
              nodes.push_back(node);
            }
            else
            {
              error = QStringLiteral("ref \"%1\" contains stale handle %2")
                        .arg(name)
                        .arg(id);
              return std::nullopt;
            }
          }
          continue;
        }
      }
    }
    if (auto* node = resolveNode(ctx, element, error))
    {
      nodes.push_back(node);
    }
    else
    {
      return std::nullopt;
    }
  }
  return nodes;
}

std::optional<mdl::BrushFaceHandle> resolveFaceRef(
  EditContext& ctx, const QJsonValue& value, QString& error)
{
  if (!value.isObject())
  {
    error = QStringLiteral("a face ref must be an object { brush, face }");
    return std::nullopt;
  }
  const auto faceRef = value.toObject();

  auto* node = resolveNode(ctx, faceRef.value("brush"), error);
  if (!node)
  {
    return std::nullopt;
  }
  auto* brushNode = dynamic_cast<mdl::BrushNode*>(node);
  if (!brushNode)
  {
    error = QStringLiteral("handle %1 is not a brush").arg(node->id());
    return std::nullopt;
  }

  const auto faceValue = faceRef.value("face");
  const auto faceIndex = faceValue.toInteger(-1);
  if (!faceValue.isDouble() || faceIndex < 0
      || static_cast<size_t>(faceIndex) >= brushNode->brush().faceCount())
  {
    error = QStringLiteral("invalid face index for brush %1").arg(brushNode->id());
    return std::nullopt;
  }
  return mdl::BrushFaceHandle{brushNode, static_cast<size_t>(faceIndex)};
}

std::optional<std::vector<mdl::BrushFaceHandle>> resolveFaceList(
  EditContext& ctx, const QJsonValue& value, QString& error)
{
  if (!value.isArray())
  {
    error = QStringLiteral("expected an array of face refs");
    return std::nullopt;
  }
  auto faces = std::vector<mdl::BrushFaceHandle>{};
  for (const auto& element : value.toArray())
  {
    if (const auto face = resolveFaceRef(ctx, element, error))
    {
      faces.push_back(*face);
    }
    else
    {
      return std::nullopt;
    }
  }
  return faces;
}

std::optional<vm::bbox3d> boundsFromJson(const QJsonValue& value, QString& error)
{
  const auto obj = value.toObject();
  const auto min = vec3FromJson(obj.value("min"));
  const auto max = vec3FromJson(obj.value("max"));
  if (!value.isObject() || !min || !max)
  {
    error = QStringLiteral("bounds must be { min: Vec3, max: Vec3 }");
    return std::nullopt;
  }
  for (size_t i = 0; i < 3; ++i)
  {
    if ((*min)[i] >= (*max)[i])
    {
      error = QStringLiteral("bounds min must be strictly less than max on every axis");
      return std::nullopt;
    }
  }
  return vm::bbox3d{*min, *max};
}

// Replaces the selection with the given handles if the op carries any; many verbs
// operate on the selection, so an explicit handle list is "select these, then act".
// Returns false (and sets `error`) only if resolution fails.
bool selectHandlesIfGiven(EditContext& ctx, const QJsonObject& op, QString& error)
{
  if (!op.contains("handles"))
  {
    return true;
  }
  const auto nodes = resolveNodeList(ctx, op.value("handles"), error);
  if (!nodes)
  {
    return false;
  }
  deselectAll(ctx.map);
  selectNodes(ctx.map, *nodes);
  return true;
}

QJsonArray selectionHandles(const mdl::Map& map)
{
  auto handles = QJsonArray{};
  for (const auto* node : map.selection().nodes)
  {
    handles.append(static_cast<qint64>(node->id()));
  }
  return handles;
}

QJsonObject opSelect(EditContext& ctx, const QJsonObject& op, QString& error)
{
  const auto mode = op.contains("mode") ? op.value("mode").toString()
                                        : QStringLiteral("replace");
  if (mode != "replace" && mode != "add" && mode != "remove")
  {
    error = QStringLiteral("mode must be \"replace\", \"add\", or \"remove\"");
    return {};
  }

  auto nodes = std::optional<std::vector<mdl::Node*>>{std::vector<mdl::Node*>{}};
  if (op.contains("nodes"))
  {
    nodes = resolveNodeList(ctx, op.value("nodes"), error);
    if (!nodes)
    {
      return {};
    }
  }
  auto faces = std::optional<std::vector<mdl::BrushFaceHandle>>{
    std::vector<mdl::BrushFaceHandle>{}};
  if (op.contains("faces"))
  {
    faces = resolveFaceList(ctx, op.value("faces"), error);
    if (!faces)
    {
      return {};
    }
  }

  if (mode == "remove")
  {
    deselectNodes(ctx.map, *nodes);
    deselectBrushFaces(ctx.map, *faces);
  }
  else
  {
    if (mode == "replace")
    {
      deselectAll(ctx.map);
    }
    selectNodes(ctx.map, *nodes);
    selectBrushFaces(ctx.map, *faces);
  }
  return QJsonObject{{"ok", true}};
}

QJsonObject opBrush(EditContext& ctx, const QJsonObject& op, QString& error)
{
  auto& map = ctx.map;

  const auto material = op.contains("material")
                          ? op.value("material").toString().toStdString()
                          : map.currentMaterialName();
  const auto builder = mdl::BrushBuilder{
    map.worldNode().mapFormat(),
    map.worldBounds(),
    map.gameInfo().gameConfig.faceAttribsConfig.defaults};

  const auto shape = op.value("shape").toObject();
  auto brushResult = std::optional<Result<mdl::Brush>>{};

  if (shape.contains("box"))
  {
    if (const auto bounds = boundsFromJson(shape.value("box"), error))
    {
      brushResult = builder.createCuboid(*bounds, material);
    }
  }
  else if (shape.contains("cylinder"))
  {
    const auto cylinder = shape.value("cylinder").toObject();
    const auto bounds = boundsFromJson(cylinder.value("bounds"), error);
    const auto axisName = cylinder.value("axis").toString();
    const auto sides = cylinder.value("sides").toInteger(0);
    if (bounds)
    {
      if (axisName != "x" && axisName != "y" && axisName != "z")
      {
        error = QStringLiteral("cylinder axis must be \"x\", \"y\", or \"z\"");
      }
      else if (sides < 3 || sides > 256)
      {
        error = QStringLiteral("cylinder sides must be between 3 and 256");
      }
      else
      {
        const auto axis = axisName == "x"   ? vm::axis::x
                          : axisName == "y" ? vm::axis::y
                                            : vm::axis::z;
        brushResult = builder.createCylinder(
          *bounds,
          mdl::EdgeAlignedCircle{static_cast<size_t>(sides)},
          axis,
          material);
      }
    }
  }
  else if (shape.contains("points"))
  {
    const auto pointsValue = shape.value("points");
    auto points = std::vector<vm::vec3d>{};
    for (const auto& pointValue : pointsValue.toArray())
    {
      if (const auto point = vec3FromJson(pointValue))
      {
        points.push_back(*point);
      }
      else
      {
        error = QStringLiteral("each point must be an array of three numbers");
        return {};
      }
    }
    if (!pointsValue.isArray() || points.size() < 4)
    {
      error = QStringLiteral("points must be an array of at least four Vec3");
      return {};
    }
    brushResult = builder.createBrush(points, material);
  }
  else
  {
    error = QStringLiteral("shape must contain \"box\", \"cylinder\", or \"points\"");
  }

  if (!brushResult)
  {
    return {};
  }
  if (brushResult->is_error())
  {
    error = QString::fromStdString(std::get<Error>(brushResult->error()).msg);
    return {};
  }

  auto* parent = mdl::parentForNodes(map);
  if (op.contains("layer"))
  {
    auto* layerNode = resolveNode(ctx, op.value("layer"), error);
    if (!layerNode)
    {
      return {};
    }
    if (
      dynamic_cast<mdl::LayerNode*>(layerNode) == nullptr
      && dynamic_cast<mdl::GroupNode*>(layerNode) == nullptr)
    {
      error = QStringLiteral("layer must name a layer or group");
      return {};
    }
    parent = layerNode;
  }

  auto* brushNode = new mdl::BrushNode{std::move(brushResult->value())};
  if (addNodes(map, {{parent, {brushNode}}}).empty())
  {
    error = QStringLiteral("could not add brush to the document");
    return {};
  }
  return QJsonObject{{"ok", true}, {"handle", static_cast<qint64>(brushNode->id())}};
}

QJsonObject opEntity(EditContext& ctx, const QJsonObject& op, QString& error)
{
  auto& map = ctx.map;

  const auto classname = op.value("classname").toString();
  if (classname.isEmpty())
  {
    error = QStringLiteral("classname is required");
    return {};
  }

  auto brushNodes = std::vector<mdl::Node*>{};
  if (op.contains("brushes"))
  {
    const auto resolved = resolveNodeList(ctx, op.value("brushes"), error);
    if (!resolved)
    {
      return {};
    }
    for (auto* node : *resolved)
    {
      if (dynamic_cast<mdl::BrushNode*>(node) == nullptr)
      {
        error = QStringLiteral("handle %1 is not a brush").arg(node->id());
        return {};
      }
    }
    brushNodes = *resolved;
  }

  auto entity = mdl::Entity{
    {{mdl::EntityPropertyKeys::Classname, classname.toStdString()}}};

  if (map.worldNode().entityPropertyConfig().setDefaultProperties)
  {
    if (
      const auto* definition =
        map.entityDefinitionManager().definition(classname.toStdString()))
    {
      mdl::setDefaultProperties(*definition, entity, mdl::SetDefaultPropertyMode::SetAll);
    }
  }

  if (op.contains("position"))
  {
    const auto position = vec3FromJson(op.value("position"));
    if (!position)
    {
      error = QStringLiteral("position must be an array of three numbers");
      return {};
    }
    entity.addOrUpdateProperty(
      mdl::EntityPropertyKeys::Origin,
      QStringLiteral("%1 %2 %3")
        .arg((*position)[0])
        .arg((*position)[1])
        .arg((*position)[2])
        .toStdString());
  }

  for (const auto& propertiesValue = op.value("properties");
       const auto& key : propertiesValue.toObject().keys())
  {
    const auto value = propertiesValue.toObject().value(key);
    if (!value.isString())
    {
      error = QStringLiteral("property \"%1\" must have a string value").arg(key);
      return {};
    }
    entity.addOrUpdateProperty(key.toStdString(), value.toString().toStdString());
  }

  auto* entityNode = new mdl::EntityNode{std::move(entity)};
  if (addNodes(map, {{mdl::parentForNodes(map), {entityNode}}}).empty())
  {
    error = QStringLiteral("could not add entity to the document");
    return {};
  }
  if (!brushNodes.empty() && !reparentNodes(map, {{entityNode, brushNodes}}))
  {
    error = QStringLiteral("could not move the given brushes into the entity");
    return {};
  }
  return QJsonObject{{"ok", true}, {"handle", static_cast<qint64>(entityNode->id())}};
}

QJsonObject opSetProps(EditContext& ctx, const QJsonObject& op, QString& error)
{
  auto* node = resolveNode(ctx, op.value("handle"), error);
  if (!node)
  {
    return {};
  }
  auto* entityNode = dynamic_cast<mdl::EntityNodeBase*>(node);
  if (!entityNode)
  {
    error = QStringLiteral("handle %1 has no entity properties").arg(node->id());
    return {};
  }

  auto entity = entityNode->entity();
  for (const auto& setValue = op.value("set");
       const auto& key : setValue.toObject().keys())
  {
    const auto value = setValue.toObject().value(key);
    if (!value.isString())
    {
      error = QStringLiteral("property \"%1\" must have a string value").arg(key);
      return {};
    }
    entity.addOrUpdateProperty(key.toStdString(), value.toString().toStdString());
  }
  for (const auto& removeValue : op.value("remove").toArray())
  {
    entity.removeProperty(removeValue.toString().toStdString());
  }

  if (!updateNodeContents(
        ctx.map, "Set Properties", {{node, mdl::NodeContents{std::move(entity)}}}))
  {
    error = QStringLiteral("could not update properties of handle %1").arg(node->id());
    return {};
  }
  return QJsonObject{{"ok", true}};
}

QJsonObject opPaint(EditContext& ctx, const QJsonObject& op, QString& error)
{
  const auto faces = resolveFaceList(ctx, op.value("faces"), error);
  if (!faces)
  {
    return {};
  }
  if (faces->empty())
  {
    error = QStringLiteral("faces must not be empty");
    return {};
  }

  const auto attributes = op.value("attributes").toObject();
  auto update = mdl::UpdateBrushFaceAttributes{};
  if (attributes.contains("material"))
  {
    update.materialName = attributes.value("material").toString().toStdString();
  }
  const auto setFloat = [&](const char* key, auto& target) {
    if (attributes.contains(key))
    {
      target =
        mdl::SetValue{static_cast<float>(attributes.value(key).toDouble())};
    }
  };
  setFloat("xOffset", update.xOffset);
  setFloat("yOffset", update.yOffset);
  setFloat("rotation", update.rotation);
  setFloat("xScale", update.xScale);
  setFloat("yScale", update.yScale);

  if (!applyAndSwap(
        ctx.map, "Change Face Attributes", *faces, [&](mdl::BrushFace& face) {
          evaluate(update, face);
          return true;
        }))
  {
    error = QStringLiteral("could not change face attributes");
    return {};
  }
  return QJsonObject{{"ok", true}};
}

QJsonObject opTransform(EditContext& ctx, const QJsonObject& op, QString& error)
{
  auto& map = ctx.map;
  if (!selectHandlesIfGiven(ctx, op, error))
  {
    return {};
  }
  if (map.selection().nodes.empty())
  {
    error = QStringLiteral("nothing to transform: the selection is empty");
    return {};
  }

  const auto transform = op.value("transform").toObject();
  auto ok = false;
  if (transform.contains("translate"))
  {
    const auto delta = vec3FromJson(transform.value("translate"));
    if (!delta)
    {
      error = QStringLiteral("translate must be a Vec3");
      return {};
    }
    ok = translateSelection(map, *delta);
  }
  else if (transform.contains("rotate"))
  {
    const auto rotate = transform.value("rotate").toObject();
    const auto center = vec3FromJson(rotate.value("center"));
    const auto axis = vec3FromJson(rotate.value("axis"));
    const auto angle = rotate.value("angle");
    if (!center || !axis || !angle.isDouble())
    {
      error = QStringLiteral("rotate must be { center: Vec3, axis: Vec3, "
                             "angle: number }");
      return {};
    }
    if (vm::is_zero(*axis, vm::Cd::almost_zero()))
    {
      error = QStringLiteral("rotation axis must not be zero");
      return {};
    }
    ok = rotateSelection(map, *center, vm::normalize(*axis), angle.toDouble());
  }
  else if (transform.contains("scale"))
  {
    const auto scale = transform.value("scale").toObject();
    const auto center = vec3FromJson(scale.value("center"));
    const auto factors = vec3FromJson(scale.value("factors"));
    if (!center || !factors)
    {
      error = QStringLiteral("scale must be { center: Vec3, factors: Vec3 }");
      return {};
    }
    ok = scaleSelection(map, *center, *factors);
  }
  else if (transform.contains("matrix"))
  {
    const auto matrixValue = transform.value("matrix").toArray();
    if (matrixValue.size() != 16)
    {
      error = QStringLiteral("matrix must be 16 numbers, row-major 4x4");
      return {};
    }
    auto matrix = vm::mat4x4d{};
    for (size_t i = 0; i < 16; ++i)
    {
      matrix[i % 4][i / 4] = matrixValue[static_cast<qsizetype>(i)].toDouble();
    }
    ok = transformSelection(map, "Transform Objects", matrix);
  }
  else
  {
    error = QStringLiteral("transform must contain \"translate\", \"rotate\", "
                           "\"scale\", or \"matrix\"");
    return {};
  }

  if (!ok)
  {
    error = QStringLiteral("the transformation could not be applied");
    return {};
  }
  return QJsonObject{{"ok", true}};
}

QJsonObject opDelete(EditContext& ctx, const QJsonObject& op, QString& error)
{
  const auto nodes = resolveNodeList(ctx, op.value("handles"), error);
  if (!nodes)
  {
    return {};
  }
  for (const auto* node : *nodes)
  {
    if (
      dynamic_cast<const mdl::WorldNode*>(node)
      || dynamic_cast<const mdl::LayerNode*>(node))
    {
      error = QStringLiteral("cannot delete the world or a layer (handle %1)")
                .arg(node->id());
      return {};
    }
  }

  // removing a node does not deselect it or its descendants, so do that first
  auto toDeselect = std::vector<mdl::Node*>{};
  for (auto* node : *nodes)
  {
    node->accept(kdl::overload(
      [&](auto&& thisLambda, mdl::Node& n) {
        if (n.selected())
        {
          toDeselect.push_back(&n);
        }
        n.visitChildren(thisLambda);
      }));
  }
  deselectNodes(ctx.map, toDeselect);

  auto facesToDeselect = std::vector<mdl::BrushFaceHandle>{};
  for (const auto& faceHandle : ctx.map.selection().brushFaces)
  {
    if (std::any_of(nodes->begin(), nodes->end(), [&](const auto* node) {
          return node == faceHandle.node() || node->isAncestorOf(*faceHandle.node());
        }))
    {
      facesToDeselect.push_back(faceHandle);
    }
  }
  deselectBrushFaces(ctx.map, facesToDeselect);

  removeNodes(ctx.map, *nodes);
  return QJsonObject{{"ok", true}};
}

QJsonObject opCsg(EditContext& ctx, const QJsonObject& op, QString& error)
{
  auto& map = ctx.map;
  if (!selectHandlesIfGiven(ctx, op, error))
  {
    return {};
  }

  const auto operation = op.value("operation").toString();
  auto ok = false;
  if (operation == "subtract")
  {
    ok = csgSubtract(map);
  }
  else if (operation == "convexMerge")
  {
    ok = csgConvexMerge(map);
  }
  else if (operation == "intersect")
  {
    ok = csgIntersect(map);
  }
  else if (operation == "hollow")
  {
    ok = csgHollow(map);
  }
  else
  {
    error = QStringLiteral("operation must be \"subtract\", \"convexMerge\", "
                           "\"intersect\", or \"hollow\"");
    return {};
  }

  if (!ok)
  {
    error = QStringLiteral("the CSG operation could not be applied");
    return {};
  }
  return QJsonObject{{"ok", true}, {"handles", selectionHandles(map)}};
}

QJsonObject opClip(EditContext& ctx, const QJsonObject& op, QString& error)
{
  auto& map = ctx.map;
  if (!selectHandlesIfGiven(ctx, op, error))
  {
    return {};
  }

  const auto keep = op.value("keep").toString();
  if (keep != "front" && keep != "back" && keep != "both")
  {
    error = QStringLiteral("keep must be \"front\", \"back\", or \"both\"");
    return {};
  }

  const auto plane = op.value("plane").toObject();
  auto points = std::optional<std::array<vm::vec3d, 3>>{};
  if (plane.contains("points"))
  {
    const auto pointsValue = plane.value("points").toArray();
    const auto p1 = vec3FromJson(pointsValue.at(0));
    const auto p2 = vec3FromJson(pointsValue.at(1));
    const auto p3 = vec3FromJson(pointsValue.at(2));
    if (pointsValue.size() != 3 || !p1 || !p2 || !p3)
    {
      error = QStringLiteral("plane points must be three Vec3");
      return {};
    }
    points = {*p1, *p2, *p3};
  }
  else if (plane.contains("point") && plane.contains("normal"))
  {
    const auto point = vec3FromJson(plane.value("point"));
    const auto normalValue = vec3FromJson(plane.value("normal"));
    if (!point || !normalValue)
    {
      error = QStringLiteral("plane must be { point: Vec3, normal: Vec3 }");
      return {};
    }
    if (vm::is_zero(*normalValue, vm::Cd::almost_zero()))
    {
      error = QStringLiteral("plane normal must not be zero");
      return {};
    }
    // build two tangents so that cross(t1, t2) == normal; "front" is then the
    // half-space the normal points into
    const auto normal = vm::normalize(*normalValue);
    const auto up = vm::abs(normal.z()) < 0.9 ? vm::vec3d{0, 0, 1} : vm::vec3d{1, 0, 0};
    const auto t1 = vm::normalize(vm::cross(normal, up));
    const auto t2 = vm::cross(normal, t1);
    points = {*point, *point + t1, *point + t2};
  }
  else
  {
    error = QStringLiteral("plane must be { points: [Vec3, Vec3, Vec3] } or "
                           "{ point: Vec3, normal: Vec3 }");
    return {};
  }

  if (map.selection().brushes.empty())
  {
    error = QStringLiteral("nothing to clip: no brushes are selected");
    return {};
  }

  if (!clipSelectedBrushes(
        map,
        (*points)[0],
        (*points)[1],
        (*points)[2],
        keep != "back",
        keep != "front"))
  {
    error = QStringLiteral("the clip could not be applied");
    return {};
  }
  return QJsonObject{{"ok", true}, {"handles", selectionHandles(map)}};
}

QJsonObject runOp(EditContext& ctx, const QJsonObject& op, QString& error)
{
  const auto opName = op.value("op").toString();
  if (opName == "select")
  {
    return opSelect(ctx, op, error);
  }
  if (opName == "brush")
  {
    return opBrush(ctx, op, error);
  }
  if (opName == "entity")
  {
    return opEntity(ctx, op, error);
  }
  if (opName == "setProps")
  {
    return opSetProps(ctx, op, error);
  }
  if (opName == "paint")
  {
    return opPaint(ctx, op, error);
  }
  if (opName == "transform")
  {
    return opTransform(ctx, op, error);
  }
  if (opName == "delete")
  {
    return opDelete(ctx, op, error);
  }
  if (opName == "csg")
  {
    return opCsg(ctx, op, error);
  }
  if (opName == "clip")
  {
    return opClip(ctx, op, error);
  }
  error = QStringLiteral("unknown op \"%1\"").arg(opName);
  return {};
}

// Binds an op's `as` label to its result handle(s) so later ops can use "@label".
bool bindRef(
  EditContext& ctx, const QJsonObject& op, const QJsonObject& result, QString& error)
{
  if (!op.contains("as"))
  {
    return true;
  }
  const auto name = op.value("as").toString();
  if (name.isEmpty())
  {
    error = QStringLiteral("as must be a non-empty string");
    return false;
  }

  if (result.contains("handle"))
  {
    ctx.refs[name] = static_cast<std::uint64_t>(result.value("handle").toInteger());
    return true;
  }
  if (result.contains("handles"))
  {
    auto ids = std::vector<std::uint64_t>{};
    for (const auto& handleValue : result.value("handles").toArray())
    {
      ids.push_back(static_cast<std::uint64_t>(handleValue.toInteger()));
    }
    ctx.refs[name] = std::move(ids);
    return true;
  }
  error = QStringLiteral("this op produces no handle to bind to \"%1\"").arg(name);
  return false;
}

QHttpServerResponse handleEdit(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  auto& map = resolved.document->map();

  const auto query = request.query();
  const auto onError = query.queryItemValue("onError");
  if (!onError.isEmpty() && onError != "continue" && onError != "abort")
  {
    return jsonError(
      "onError must be \"abort\" or \"continue\"", StatusCode::BadRequest);
  }
  const auto continueMode = onError == QStringLiteral("continue");

  const auto opsValue = resolved.body.value("ops");
  if (!opsValue.isArray())
  {
    return jsonError("ops must be an array", StatusCode::BadRequest);
  }
  const auto ops = opsValue.toArray();
  if (ops.isEmpty())
  {
    return QHttpServerResponse{QJsonArray{}};
  }

  for (auto* window : appController.mapWindowManager().mapWindows())
  {
    if (&window->document() == resolved.document && window->anyModalToolActive())
    {
      return jsonError("a modal tool is active", StatusCode::Conflict);
    }
  }

  // one batch = one undo step; disable collation so two batches sent close together
  // stay separately undoable
  const auto collationWasEnabled = map.isCommandCollationEnabled();
  map.setIsCommandCollationEnabled(false);

  auto ctx = EditContext{map, {}};
  auto transaction = mdl::Transaction{map, "API Edit"};
  auto results = QJsonArray{};
  auto failed = false;

  for (const auto& opValue : ops)
  {
    if (failed)
    {
      results.append(QJsonObject{
        {"ok", false},
        {"error", QStringLiteral("skipped due to an earlier failure")},
        {"skipped", true},
      });
      continue;
    }

    auto error = QString{};
    auto result = QJsonObject{};
    if (!opValue.isObject())
    {
      error = QStringLiteral("each op must be an object");
    }
    else
    {
      result = runOp(ctx, opValue.toObject(), error);
      if (error.isEmpty())
      {
        bindRef(ctx, opValue.toObject(), result, error);
      }
    }

    if (error.isEmpty())
    {
      results.append(result);
    }
    else
    {
      results.append(QJsonObject{{"ok", false}, {"error", error}});
      if (!continueMode)
      {
        failed = true;
      }
    }
  }

  if (failed)
  {
    transaction.cancel();
  }
  else if (!transaction.commit())
  {
    map.setIsCommandCollationEnabled(collationWasEnabled);
    return jsonError(
      "the edit transaction could not be committed", StatusCode::UnprocessableEntity);
  }
  map.setIsCommandCollationEnabled(collationWasEnabled);

  return QHttpServerResponse{
    results, failed ? StatusCode::UnprocessableEntity : StatusCode::Ok};
}

// ---- History and IO ----

QHttpServerResponse handleUndo(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  auto& map = resolved.document->map();

  if (!map.canUndoCommand())
  {
    return QHttpServerResponse{QJsonObject{{"ok", false}}};
  }
  const auto name = QString::fromStdString(*map.undoCommandName());
  map.undoCommand();
  return QHttpServerResponse{QJsonObject{{"ok", true}, {"name", name}}};
}

QHttpServerResponse handleRedo(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  auto& map = resolved.document->map();

  if (!map.canRedoCommand())
  {
    return QHttpServerResponse{QJsonObject{{"ok", false}}};
  }
  const auto name = QString::fromStdString(*map.redoCommandName());
  map.redoCommand();
  return QHttpServerResponse{QJsonObject{{"ok", true}, {"name", name}}};
}

QHttpServerResponse handleSave(
  AppController& appController, const QHttpServerRequest& request)
{
  auto resolved = resolveBodyAndDoc(appController, request);
  if (resolved.error)
  {
    return std::move(*resolved.error);
  }
  auto& map = resolved.document->map();

  if (!map.persistent())
  {
    return jsonError(
      "the document has never been saved; save it in the UI first",
      StatusCode::UnprocessableEntity);
  }
  const auto saveResult = map.save();
  if (saveResult.is_error())
  {
    return jsonError(
      QString::fromStdString(std::get<Error>(saveResult.error()).msg),
      StatusCode::UnprocessableEntity);
  }
  return QHttpServerResponse{QJsonObject{
    {"ok", true},
    {"path", QString::fromStdString(map.path().generic_string())},
  }};
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

  m_httpServer->route(
    "/edit", QHttpServerRequest::Method::Post, [this](const QHttpServerRequest& request) {
      return handleEdit(m_appController, request);
    });

  m_httpServer->route(
    "/undo", QHttpServerRequest::Method::Post, [this](const QHttpServerRequest& request) {
      return handleUndo(m_appController, request);
    });

  m_httpServer->route(
    "/redo", QHttpServerRequest::Method::Post, [this](const QHttpServerRequest& request) {
      return handleRedo(m_appController, request);
    });

  m_httpServer->route(
    "/save", QHttpServerRequest::Method::Post, [this](const QHttpServerRequest& request) {
      return handleSave(m_appController, request);
    });
}

} // namespace tb::ui
