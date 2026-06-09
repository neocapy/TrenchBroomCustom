"""Typed response/request shapes for the TrenchBroom HTTP control API.

Vectors are accepted as any ``Sequence[float]`` (lists, tuples, numpy
arrays) and returned as plain ``tuple[float, float, float]``.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Literal, Sequence

Vec3 = tuple[float, float, float]
Vec2 = tuple[float, float]
VecLike = Sequence[float]
Handle = int
NodeType = Literal["world", "layer", "group", "entity", "brush", "patch"]


def _vec3(v: Sequence[float]) -> Vec3:
    if len(v) != 3:
        raise ValueError(f"expected 3 components, got {len(v)}")
    return (float(v[0]), float(v[1]), float(v[2]))


def _vec2(v: Sequence[float]) -> Vec2:
    if len(v) != 2:
        raise ValueError(f"expected 2 components, got {len(v)}")
    return (float(v[0]), float(v[1]))


@dataclass(frozen=True)
class Bounds:
    min: Vec3
    max: Vec3

    @staticmethod
    def of(min: VecLike, max: VecLike) -> Bounds:
        return Bounds(_vec3(min), _vec3(max))

    def to_json(self) -> dict[str, Any]:
        return {"min": list(self.min), "max": list(self.max)}

    @staticmethod
    def from_json(d: dict[str, Any]) -> Bounds:
        return Bounds(_vec3(d["min"]), _vec3(d["max"]))


BoundsLike = "Bounds | tuple[VecLike, VecLike]"


def _bounds(b: Bounds | tuple[VecLike, VecLike]) -> Bounds:
    if isinstance(b, Bounds):
        return b
    return Bounds.of(b[0], b[1])


@dataclass(frozen=True)
class LayerInfo:
    handle: Handle
    name: str


@dataclass(frozen=True)
class DocumentInfo:
    handle: Handle
    active: bool
    name: str
    path: str | None
    map_format: str
    world_bounds: Bounds
    modified: bool
    layers: tuple[LayerInfo, ...]
    node_count: int

    @staticmethod
    def from_json(d: dict[str, Any]) -> DocumentInfo:
        return DocumentInfo(
            handle=d["handle"],
            active=d["active"],
            name=d["name"],
            path=d["path"],
            map_format=d["mapFormat"],
            world_bounds=Bounds.from_json(d["worldBounds"]),
            modified=d["modified"],
            layers=tuple(LayerInfo(l["handle"], l["name"]) for l in d["layers"]),
            node_count=d["nodeCount"],
        )


@dataclass(frozen=True)
class NodeRef:
    handle: Handle
    type: NodeType


@dataclass(frozen=True)
class BrushFaceInfo:
    index: int
    points: tuple[Vec3, Vec3, Vec3]
    normal: Vec3
    material: str
    offset: Vec2
    scale: Vec2
    rotation: float

    @staticmethod
    def from_json(d: dict[str, Any]) -> BrushFaceInfo:
        p = d["points"]
        return BrushFaceInfo(
            index=d["index"],
            points=(_vec3(p[0]), _vec3(p[1]), _vec3(p[2])),
            normal=_vec3(d["normal"]),
            material=d["material"],
            offset=_vec2(d["offset"]),
            scale=_vec2(d["scale"]),
            rotation=d["rotation"],
        )


@dataclass(frozen=True)
class NodeSummary:
    handle: Handle
    type: NodeType
    bounds: Bounds
    selected: bool
    visible: bool
    locked: bool
    parent: Handle | None = None
    layer: Handle | None = None
    classname: str | None = None
    material: str | None = None

    @staticmethod
    def from_json(d: dict[str, Any]) -> NodeSummary:
        return NodeSummary(**NodeSummary._kwargs(d))

    @staticmethod
    def _kwargs(d: dict[str, Any]) -> dict[str, Any]:
        return dict(
            handle=d["handle"],
            type=d["type"],
            bounds=Bounds.from_json(d["bounds"]),
            selected=d["selected"],
            visible=d["visible"],
            locked=d["locked"],
            parent=d.get("parent"),
            layer=d.get("layer"),
            classname=d.get("classname"),
            material=d.get("material"),
        )


@dataclass(frozen=True)
class NodeDetail(NodeSummary):
    properties: dict[str, str] | None = None
    origin: Vec3 | None = None
    point_entity: bool | None = None
    faces: tuple[BrushFaceInfo, ...] | None = None
    children: tuple[Handle, ...] | None = None

    @staticmethod
    def from_json(d: dict[str, Any]) -> NodeDetail:
        kw = NodeSummary._kwargs(d)
        origin = d.get("origin")
        faces = d.get("faces")
        children = d.get("children")
        return NodeDetail(
            **kw,
            properties=d.get("properties"),
            origin=_vec3(origin) if origin is not None else None,
            point_entity=d.get("pointEntity"),
            faces=tuple(BrushFaceInfo.from_json(f) for f in faces) if faces is not None else None,
            children=tuple(children) if children is not None else None,
        )


@dataclass(frozen=True)
class FaceSel:
    """A selected face: brush handle + positional face index."""

    brush: Handle
    face: int


@dataclass(frozen=True)
class Selection:
    nodes: tuple[Handle, ...]
    faces: tuple[FaceSel, ...]


@dataclass(frozen=True)
class RayHit:
    point: Vec3
    distance: float
    handle: Handle
    normal: Vec3 | None = None
    face: int | None = None
    material: str | None = None

    @staticmethod
    def from_json(d: dict[str, Any]) -> RayHit:
        normal = d.get("normal")
        return RayHit(
            point=_vec3(d["point"]),
            distance=d["distance"],
            handle=d["handle"],
            normal=_vec3(normal) if normal is not None else None,
            face=d.get("face"),
            material=d.get("material"),
        )


@dataclass(frozen=True)
class MaterialCollection:
    name: str
    materials: tuple[str, ...]


@dataclass(frozen=True)
class EntityClass:
    classname: str
    type: Literal["point", "brush"]
    description: str | None = None
    bounds: Bounds | None = None


@dataclass(frozen=True)
class OpResult:
    """Result of one op in an /edit batch."""

    ok: bool
    handle: Handle | None = None
    handles: tuple[Handle, ...] | None = None
    error: str | None = None
    skipped: bool = False

    @staticmethod
    def from_json(d: dict[str, Any]) -> OpResult:
        handles = d.get("handles")
        return OpResult(
            ok=d["ok"],
            handle=d.get("handle"),
            handles=tuple(handles) if handles is not None else None,
            error=d.get("error"),
            skipped=d.get("skipped", False),
        )


@dataclass(frozen=True)
class HistoryResult:
    ok: bool
    name: str | None = None
