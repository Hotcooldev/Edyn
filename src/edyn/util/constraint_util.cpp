#include "edyn/util/constraint_util.hpp"
#include "edyn/collision/contact_manifold.hpp"
#include "edyn/comp/material.hpp"
#include "edyn/comp/tag.hpp"
#include "edyn/comp/graph_edge.hpp"
#include "edyn/comp/graph_node.hpp"
#include "edyn/comp/continuous.hpp"
#include "edyn/comp/delta_linvel.hpp"
#include "edyn/comp/delta_angvel.hpp"
#include "edyn/constraints/constraint_impulse.hpp"
#include "edyn/math/matrix3x3.hpp"
#include "edyn/math/scalar.hpp"
#include "edyn/parallel/entity_graph.hpp"
#include "edyn/constraints/constraint_row.hpp"

namespace edyn {

namespace internal {
    bool pre_make_constraint(entt::entity entity, entt::registry &registry,
                             entt::entity body0, entt::entity body1, bool is_graph_edge) {
        // Multiple constraints of different types can be assigned to the same
        // entity. If this entity already has a graph edge, just do a few
        // consistency checks.
        if (registry.has<graph_edge>(entity)) {
            auto &edge = registry.get<graph_edge>(entity);
            auto [ent0, ent1] = registry.ctx<entity_graph>().edge_node_entities(edge.edge_index);
            EDYN_ASSERT(ent0 == body0 && ent1 == body1);
            EDYN_ASSERT(registry.has<constraint_impulse>(entity));
            EDYN_ASSERT(registry.has<procedural_tag>(entity));
            return false;
        }

        registry.emplace<constraint_impulse>(entity);
        auto &con_dirty = registry.get_or_emplace<dirty>(entity);
        con_dirty.created<constraint_impulse>();

        // If the constraint is not a graph edge (e.g. when it's a `contact_constraint`
        // in a contact manifold), it means it is handled as a child of another entity
        // that is a graph edge and thus creating an edge for this would be redundant.
        if (is_graph_edge) {
            auto node_index0 = registry.get<graph_node>(body0).node_index;
            auto node_index1 = registry.get<graph_node>(body1).node_index;
            auto edge_index = registry.ctx<entity_graph>().insert_edge(entity, node_index0, node_index1);
            registry.emplace<procedural_tag>(entity);
            registry.emplace<graph_edge>(entity, edge_index);
            con_dirty.created<procedural_tag>();
        }

        return true;
    }
}

entt::entity make_contact_manifold(entt::registry &registry, entt::entity body0, entt::entity body1, scalar separation_threshold) {
    auto manifold_entity = registry.create();
    make_contact_manifold(manifold_entity, registry, body0, body1, separation_threshold);
    return manifold_entity;
}

void make_contact_manifold(entt::entity manifold_entity, entt::registry &registry, entt::entity body0, entt::entity body1, scalar separation_threshold) {
    EDYN_ASSERT(registry.valid(body0) && registry.valid(body1));
    registry.emplace<procedural_tag>(manifold_entity);
    registry.emplace<contact_manifold>(manifold_entity, body0, body1, separation_threshold);

    auto material_view = registry.view<material>();

    if (material_view.contains(body0) && material_view.contains(body1)) {
        auto &material0 = material_view.get(body0);
        auto &material1 = material_view.get(body1);
        auto restitution = std::min(material0.restitution, material1.restitution);

        if (restitution > EDYN_EPSILON) {
            registry.emplace<contact_manifold_with_restitution>(manifold_entity);
        }
    }

    auto node_index0 = registry.get<graph_node>(body0).node_index;
    auto node_index1 = registry.get<graph_node>(body1).node_index;
    auto edge_index = registry.ctx<entity_graph>().insert_edge(manifold_entity, node_index0, node_index1);
    registry.emplace<graph_edge>(manifold_entity, edge_index);

    registry.get_or_emplace<dirty>(manifold_entity)
        .set_new()
        .created<procedural_tag,
                 contact_manifold>();
}

scalar get_effective_mass(const constraint_row &row) {
    return get_effective_mass(row.J, row.inv_mA, row.inv_IA, row.inv_mB, row.inv_IB);
}

scalar get_effective_mass(const std::array<vector3, 4> &J,
                          scalar inv_mA, const matrix3x3 &inv_IA,
                          scalar inv_mB, const matrix3x3 &inv_IB) {
    auto J_invM_JT = dot(J[0], J[0]) * inv_mA +
                     dot(inv_IA * J[1], J[1]) +
                     dot(J[2], J[2]) * inv_mB +
                     dot(inv_IB * J[3], J[3]);
    auto eff_mass = scalar(1) / J_invM_JT;
    return eff_mass;
}

scalar get_relative_speed(const std::array<vector3, 4> &J,
                          const vector3 &linvelA,
                          const vector3 &angvelA,
                          const vector3 &linvelB,
                          const vector3 &angvelB) {
    auto relspd = dot(J[0], linvelA) +
                  dot(J[1], angvelA) +
                  dot(J[2], linvelB) +
                  dot(J[3], angvelB);
    return relspd;
}

void prepare_row(constraint_row &row,
                 const constraint_row_options &options,
                 const vector3 &linvelA, const vector3 &angvelA,
                 const vector3 &linvelB, const vector3 &angvelB) {
    auto J_invM_JT = dot(row.J[0], row.J[0]) * row.inv_mA +
                     dot(row.inv_IA * row.J[1], row.J[1]) +
                     dot(row.J[2], row.J[2]) * row.inv_mB +
                     dot(row.inv_IB * row.J[3], row.J[3]);
    row.eff_mass = 1 / J_invM_JT;

    auto relvel = dot(row.J[0], linvelA) +
                  dot(row.J[1], angvelA) +
                  dot(row.J[2], linvelB) +
                  dot(row.J[3], angvelB);

    row.rhs = -(options.error * options.erp + relvel * (1 + options.restitution));
}

void apply_impulse(scalar impulse, constraint_row &row) {
    // Apply linear impulse.
    *row.dvA += row.inv_mA * row.J[0] * impulse;
    *row.dvB += row.inv_mB * row.J[2] * impulse;

    // Apply angular impulse.
    *row.dwA += row.inv_IA * row.J[1] * impulse;
    *row.dwB += row.inv_IB * row.J[3] * impulse;
}

void warm_start(constraint_row &row) {
    apply_impulse(row.impulse, row);
}

}
