#pragma once

#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <filesystem>
#include <iostream>

#include <cgltf.h>

#include "../math/Vec.hpp"
#include "../math/Mat.hpp"

namespace valo {

struct GLTFMaterial {
    std::string name;
    Vec4f baseColor     = {1.f, 1.f, 1.f, 1.f};
    float metallic      = 1.f;
    float roughness     = 1.f;
    Vec3f emissive      = {0, 0, 0};
    std::string 
        baseColorTex, 
        metallicRoughnessTex, 
        normalTex, occlusionTex, 
        emissiveTex;
};

struct GLTFVertex { Vec3f pos; Vec3f normal; Vec2f uv; };

struct GLTFPrimitive {
    uint32_t    firstIndex, indexCount;
    int         material;
};

struct GLTFMesh {
    std::string name;
    std::vector<GLTFPrimitive> primitives;
};

struct GLTFNode {
    std::string name;
    Mat4f       world;
    int         mesh        = -1;
};

struct GLTF {
    std::vector<GLTFVertex>     vertices;
    std::vector<uint32_t>       indices;
    std::vector<GLTFMesh>       meshes;
    std::vector<GLTFMaterial>   materials;
    std::vector<GLTFNode>       nodes;
};

inline GLTF parseGLTF(const std::string& path) {
    GLTF out;

    cgltf_options   opts    = {};
    cgltf_data*     data    = nullptr;

    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) {
        std::cerr << "parseGLTF(): Could not parse file \"" << path << "\" " << std::endl;
        return out;
    }

    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        std::cerr << "parseGLTF(): Could not load buffers for file \"" << path << "\" " << std::endl;
        return out;
    }

    const std::string baseDir = std::filesystem::path(path).parent_path().string() + "/";

    auto texPath = [&](const cgltf_texture_view& tv) -> std::string {
        if (!tv.texture || !tv.texture->image || !tv.texture->image->uri) return "";
        return baseDir + tv.texture->image->uri;
    };

    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        GLTFMaterial mat;
        mat.name = m.name ? m.name : "";

        if (m.has_pbr_metallic_roughness) {
            const auto& pbr = m.pbr_metallic_roughness;

            mat.baseColor = {
                pbr.base_color_factor[0], 
                pbr.base_color_factor[1], 
                pbr.base_color_factor[2], 
                pbr.base_color_factor[3]
            };

            mat.metallic = pbr.metallic_factor;
            mat.roughness = pbr.roughness_factor;
            mat.baseColorTex = texPath(pbr.base_color_texture);
            mat.metallicRoughnessTex = texPath(pbr.metallic_roughness_texture);
        }

        mat.emissive = {
            m.emissive_factor[0], 
            m.emissive_factor[1], 
            m.emissive_factor[2]
        };

        mat.normalTex = texPath(m.normal_texture);
        mat.occlusionTex = texPath(m.occlusion_texture);
        mat.emissiveTex = texPath(m.emissive_texture);

        out.materials.push_back(mat);
    }

        for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& m = data->meshes[mi];
        GLTFMesh mesh;
        mesh.name = m.name ? m.name : "";

        for (cgltf_size pi = 0; pi < m.primitives_count; ++pi) {
            const cgltf_primitive& p = m.primitives[pi];
            if (p.type != cgltf_primitive_type_triangles || !p.indices) continue;

            // find the accessors we care about
            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr;
            for (cgltf_size a = 0; a < p.attributes_count; ++a) {
                const cgltf_attribute& at = p.attributes[a];
                if      (at.type == cgltf_attribute_type_position)                 pos = at.data;
                else if (at.type == cgltf_attribute_type_normal)                   nrm = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv  = at.data;
            }
            if (!pos) continue;

            // unpack into flat float arrays: 3 per pos/normal, 2 per uv
            const cgltf_size n = pos->count;
            std::vector<float> P(n * 3), N(nrm ? n * 3 : 0), T(uv ? n * 2 : 0);
            cgltf_accessor_unpack_floats(pos, P.data(), n * 3);
            if (nrm) cgltf_accessor_unpack_floats(nrm, N.data(), n * 3);
            if (uv)  cgltf_accessor_unpack_floats(uv,  T.data(), n * 2);

            // append vertices; remember where this primitive's verts start
            const uint32_t vertexBase = uint32_t(out.vertices.size());
            for (cgltf_size v = 0; v < n; ++v) {
                GLTFVertex vert;
                vert.pos    = {P[v*3], P[v*3+1], P[v*3+2]};
                vert.normal = nrm ? Vec3f{N[v*3], N[v*3+1], N[v*3+2]} : Vec3f{0.f, 1.f, 0.f};
                vert.uv     = uv  ? Vec2f{T[v*2], T[v*2+1]}          : Vec2f{0.f, 0.f};
                out.vertices.push_back(vert);
            }

            // append indices, rebased onto the global vertex array
            GLTFPrimitive prim;
            prim.firstIndex = uint32_t(out.indices.size());
            prim.indexCount = uint32_t(p.indices->count);
            for (cgltf_size k = 0; k < p.indices->count; ++k)
                out.indices.push_back(vertexBase + uint32_t(cgltf_accessor_read_index(p.indices, k)));
            prim.material = p.material ? int(p.material - data->materials) : -1;

            mesh.primitives.push_back(prim);
        }
        out.meshes.push_back(mesh);
    }

        for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& nd = data->nodes[ni];
        if (!nd.mesh) continue;

        GLTFNode node;
        node.name = nd.name ? nd.name : "";
        node.mesh = int(nd.mesh - data->meshes);

        float m[16];
        cgltf_node_transform_world(&nd, m);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                node.world.cols[c][r] = m[c * 4 + r];

        out.nodes.push_back(node);
    }

    return out;
}

}