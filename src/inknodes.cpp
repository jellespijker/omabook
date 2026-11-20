#include "inknodes.h"

#include <QSGGeometryNode>
#include <QSGNode>
#include <QSGVertexColorMaterial>

#include <algorithm>
#include <memory>
#include <span>

namespace {

std::unique_ptr<QSGGeometryNode> newNode() {
    auto node = std::make_unique<QSGGeometryNode>();
    auto geometry = std::make_unique<QSGGeometry>(QSGGeometry::defaultAttributes_ColoredPoint2D(), 0, 0, QSGGeometry::UnsignedShortType);
    geometry->setDrawingMode(QSGGeometry::DrawTriangles);
    // The node owns both from here (its OwnsGeometry and OwnsMaterial flags).
    node->setGeometry(geometry.release());
    node->setFlag(QSGNode::OwnsGeometry);
    node->setMaterial(std::make_unique<QSGVertexColorMaterial>().release());
    node->setFlag(QSGNode::OwnsMaterial);
    return node;
}

} // namespace

QSGNode *InkNodes::root(QSGNode *old) {
    if (old == nullptr || old != m_root) {
        // The scene graph is new (the window's first frame, or after it lost
        // its own): every node of before went with the old one.
        m_nodes.clear();
        m_root = std::make_unique<QSGNode>().release();
    }
    return m_root;
}

void InkNodes::sync(std::span<const Ink::Stroke *const> strokes, const QSet<QString> &dirty, bool restructured, bool all, Look look) {
    QSet<QString> drawn;
    if (restructured) {
        reorder(strokes, drawn, look);
    }
    for (const Ink::Stroke *stroke : strokes) {
        if (drawn.contains(stroke->id) || !(all || dirty.contains(stroke->id))) {
            continue;
        }
        bool made = false;
        QSGGeometryNode *node = nodeOf(stroke->id, made);
        if (made) {
            m_root->appendChildNode(node);
        }
        redraw(*node, *stroke, look);
    }
}

void InkNodes::reorder(std::span<const Ink::Stroke *const> strokes, QSet<QString> &drawn, Look look) {
    QSet<QString> present;
    present.reserve(static_cast<qsizetype>(strokes.size()));
    for (const Ink::Stroke *stroke : strokes) {
        present.insert(stroke->id);
    }
    for (auto it = m_nodes.begin(); it != m_nodes.end();) {
        if (present.contains(it.key())) {
            ++it;
            continue;
        }
        m_root->removeChildNode(it.value());
        const std::unique_ptr<QSGGeometryNode> gone{it.value()};
        it = m_nodes.erase(it);
    }
    // The nodes in the strokes' order: each one not where it should be is moved there.
    QSGNode *next = m_root->firstChild();
    for (const Ink::Stroke *stroke : strokes) {
        bool made = false;
        QSGGeometryNode *node = nodeOf(stroke->id, made);
        if (made) {
            redraw(*node, *stroke, look);
            drawn.insert(stroke->id);
        }
        if (node == next) {
            next = next->nextSibling();
            continue;
        }
        if (node->parent() != nullptr) {
            m_root->removeChildNode(node);
        }
        if (next != nullptr) {
            m_root->insertChildNodeBefore(node, next);
        } else {
            m_root->appendChildNode(node);
        }
    }
}

QSGGeometryNode *InkNodes::nodeOf(const QString &id, bool &made) {
    made = false;
    if (QSGGeometryNode *node = m_nodes.value(id)) {
        return node;
    }
    made = true;
    // The scene graph owns it once it is a child of the root, which the caller makes it straight away.
    QSGGeometryNode *node = newNode().release();
    m_nodes.insert(id, node);
    return node;
}

void InkNodes::redraw(QSGGeometryNode &node, const Ink::Stroke &stroke, Look look) {
    Ink::tessellate(stroke, look.scale, look.feather, m_mesh);
    QSGGeometry *geometry = node.geometry();
    geometry->allocate(static_cast<int>(m_mesh.vertices.size()), static_cast<int>(m_mesh.indices.size()));
    const std::span vertices{geometry->vertexDataAsColoredPoint2D(), m_mesh.vertices.size()};
    for (std::size_t i = 0; i < m_mesh.vertices.size(); ++i) {
        const Ink::Vertex &v = m_mesh.vertices[i];
        vertices[i].set(v.x, v.y, v.r, v.g, v.b, v.a);
    }
    std::ranges::copy(m_mesh.indices, geometry->indexDataAsUShort());
    node.markDirty(QSGNode::DirtyGeometry);
}
