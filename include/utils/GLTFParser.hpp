#pragma once

#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <filesystem>
#include <iostream>

#include <cgltf.h>

#include "../math/Vec.hpp"
#include "../math/Mat.hpp"
#include "../render/buffers/VertexBuffer.hpp"
#include "../render/RenderCommand.hpp"
#include "../math/Bounds.hpp"

namespace valo {

/*
    The raw parse of a .gltf / .glb: CPU-side geometry, materials and nodes.
    Most programs want loadModel() in Model.hpp instead, which puts all of this
    on the GPU and hands back ready-made draws. parseGLTF is for the cases that
    need the data itself - a BVH over the triangles, say.
*/

enum class AlphaMode : uint32_t { Opaque = 0, Mask = 1, Blend = 2 };

/*
    Where a texture's pixels come from: a file beside the .gltf, or encoded
    PNG/JPEG bytes embedded in a .glb or a data: URI. At most one is set.
*/
struct GLTFImage {
    std::string             path;
    std::vector<uint8_t>    encoded;
};

struct GLTFMaterial {
    std::string name;
    Vec4f       baseColor       = {1.f, 1.f, 1.f, 1.f};
    float       metallic        = 1.f;
    float       roughness       = 1.f;
    Vec3f       emissive        = {0.f, 0.f, 0.f};
    AlphaMode   alphaMode       = AlphaMode::Opaque;
    float       alphaCutoff     = 0.5f;
    bool        doubleSided     = false;

    // indices into GLTF::images, -1 for an empty slot
    int baseColorTex            = -1;   // sRGB
    int metallicRoughnessTex    = -1;   // G = roughness, B = metallic
    int normalTex               = -1;
    int occlusionTex            = -1;   // R
    int emissiveTex             = -1;   // sRGB
};

struct GLTFVertex {
    Vec3f pos;
    Vec3f normal;
    Vec2f uv;
    Vec4f tangent;      // xyz points along +u; w is the bitangent's handedness, +-1

    static VtxLayout layout() {
        return {{
            VtxAttrib::Vec3f,
            VtxAttrib::Vec3f,
            VtxAttrib::Vec2f,
            VtxAttrib::Vec4f
        }};
    }
};

/*
    A glTF material with its textures already loaded - what loadModel() in
    Model.hpp hands back. It lives here rather than there so a shader header
    can include it without pulling in the whole renderer.

    Laid out to survive the trip to GLSL - no bool and no enum field, since both
    change size between C++ and a GLSL block - so it can sit directly inside a
    shader's Uniforms struct and the shader reads it field for field.
*/
struct ModelMaterial {
    Vec4f     baseColor             = {1.f, 1.f, 1.f, 1.f};
    Vec3f     emissive              = {0.f, 0.f, 0.f};
    float     metallic              = 1.f;
    float     roughness             = 1.f;
    float     alphaCutoff           = 0.5f;     // AlphaMode::Mask: discard below this
    uint32_t  alphaMode             = 0;        // an AlphaMode
    uint32_t  doubleSided           = 0;

    // EMPTY_TEXTURE for a slot the material does not use
    TextureID baseColorTex          = EMPTY_TEXTURE;    // sRGB
    TextureID metallicRoughnessTex  = EMPTY_TEXTURE;    // G = roughness, B = metallic
    TextureID normalTex             = EMPTY_TEXTURE;    // tangent space
    TextureID occlusionTex          = EMPTY_TEXTURE;    // R
    TextureID emissiveTex           = EMPTY_TEXTURE;    // sRGB
};


struct GLTFPrimitive {
    uint32_t    firstIndex  = 0;
    uint32_t    indexCount  = 0;
    int         material    = -1;   // -1: the file gave this primitive none
};

struct GLTFMesh {
    std::string                 name;
    std::vector<GLTFPrimitive>  primitives;
};

struct GLTFNode {
    std::string name;
    Mat4f       world;
    int         mesh = -1;
};

struct GLTF {
    std::vector<GLTFVertex>     vertices;
    std::vector<uint32_t>       indices;
    std::vector<GLTFMesh>       meshes;
    std::vector<GLTFMaterial>   materials;
    std::vector<GLTFImage>      images;
    std::vector<GLTFNode>       nodes;
    Bounds                      bounds;
};


namespace detail {

/*
    Smooth normals from the triangles, for a primitive that ships none. glTF
    asks for flat shading in that case, which would mean un-sharing every
    vertex; area-weighted smooth normals keep the index buffer intact and look
    right on everything but hard-edged models.
*/
inline void computeNormals(std::vector<GLTFVertex>& v, const uint32_t* idx, size_t count) {
    std::vector<Vec3f> acc(v.size(), {0.f, 0.f, 0.f});

    for (size_t i = 0; i + 2 < count; i += 3) {
        const Vec3f& a = v[idx[i]].pos;
        const Vec3f& b = v[idx[i + 1]].pos;
        const Vec3f& c = v[idx[i + 2]].pos;
        const Vec3f n = (b - a).cross(c - a);     // length is twice the area

        for (int k = 0; k < 3; ++k) acc[idx[i + k]] += n;
    }

    for (size_t i = 0; i < v.size(); ++i)
        v[i].normal = acc[i].length() > 1e-12f ? acc[i].normalized() : Vec3f{0.f, 1.f, 0.f};
}


/*
    Tangents from UV derivatives (Lengyel's method), for a primitive that ships
    none. For each triangle solve for the direction u increases in, accumulate
    it onto all three vertices, then orthonormalise against the normal - summing
    per vertex is what keeps the result continuous across shared edges.
*/
inline void computeTangents(std::vector<GLTFVertex>& v, const uint32_t* idx, size_t count) {
    std::vector<Vec3f> tan(v.size(), {0.f, 0.f, 0.f});
    std::vector<Vec3f> bit(v.size(), {0.f, 0.f, 0.f});

    for (size_t i = 0; i + 2 < count; i += 3) {
        const GLTFVertex& a = v[idx[i]];
        const GLTFVertex& b = v[idx[i + 1]];
        const GLTFVertex& c = v[idx[i + 2]];

        const Vec3f e1 = b.pos - a.pos;
        const Vec3f e2 = c.pos - a.pos;
        const Vec2f d1 = b.uv  - a.uv;
        const Vec2f d2 = c.uv  - a.uv;

        // singular on degenerate or unmapped triangles
        const float det = d1.x * d2.y - d2.x * d1.y;
        if (std::fabs(det) < 1e-12f) continue;

        const float r  = 1.f / det;
        const Vec3f t  = (e1 * d2.y - e2 * d1.y) * r;
        const Vec3f bt = (e2 * d1.x - e1 * d2.x) * r;

        for (int k = 0; k < 3; ++k) {
            tan[idx[i + k]] += t;
            bit[idx[i + k]] += bt;
        }
    }

    for (size_t i = 0; i < v.size(); ++i) {
        const Vec3f n = v[i].normal;
        Vec3f t = tan[i] - n * n.dot(tan[i]);

        // nothing contributed: any frame is better than a zero vector
        if (t.length() < 1e-8f) {
            t = std::fabs(n.x) < 0.9f ? Vec3f{1.f, 0.f, 0.f} : Vec3f{0.f, 1.f, 0.f};
            t = t - n * n.dot(t);
        }
        t = t.normalized();

        // a mirrored UV island has its bitangent the other way round
        const float w = n.cross(t).dot(bit[i]) < 0.f ? -1.f : 1.f;
        v[i].tangent = {t.x, t.y, t.z, w};
    }
}


// standard base64, as used by data: URIs; cgltf's decoder wants the exact size
inline size_t base64Size(const char* s) {
    const size_t len = std::strlen(s);
    size_t pad = 0;
    if (len >= 1 && s[len - 1] == '=') ++pad;
    if (len >= 2 && s[len - 2] == '=') ++pad;
    return len / 4 * 3 - pad;
}

} // namespace detail


inline GLTF parseGLTF(const std::string& path) {
    GLTF out;

    cgltf_options   opts = {};
    cgltf_data*     data = nullptr;

    if (cgltf_parse_file(&opts, path.c_str(), &data) != cgltf_result_success) {
        std::cerr << "parseGLTF(): could not parse \"" << path << "\"" << std::endl;
        return out;
    }

    // every return below this frees it
    struct Guard { cgltf_data* d; ~Guard() { cgltf_free(d); } } guard{data};

    if (cgltf_load_buffers(&opts, data, path.c_str()) != cgltf_result_success) {
        std::cerr << "parseGLTF(): could not load the buffers of \"" << path << "\"" << std::endl;
        return out;
    }

    const std::string baseDir = std::filesystem::path(path).parent_path().string();


    /* ---- images: a file path, or the encoded bytes themselves ----------- */
    for (cgltf_size i = 0; i < data->images_count; ++i) {
        const cgltf_image& im = data->images[i];
        GLTFImage img;

        if (im.buffer_view && im.buffer_view->buffer && im.buffer_view->buffer->data) {
            // .glb: the PNG/JPEG sits inside the binary chunk
            const cgltf_buffer_view* bv = im.buffer_view;
            const uint8_t* p = static_cast<const uint8_t*>(bv->buffer->data) + bv->offset;
            img.encoded.assign(p, p + bv->size);
        }
        else if (im.uri && std::strncmp(im.uri, "data:", 5) == 0) {
            const char* comma = std::strchr(im.uri, ',');
            if (comma && std::strstr(im.uri, ";base64,")) {
                void* bytes = nullptr;
                const size_t size = detail::base64Size(comma + 1);

                if (cgltf_load_buffer_base64(&opts, size, comma + 1, &bytes) == cgltf_result_success) {
                    const uint8_t* p = static_cast<const uint8_t*>(bytes);
                    img.encoded.assign(p, p + size);
                    std::free(bytes);
                }
            }
        }
        else if (im.uri) {
            // a URI is percent-encoded: "my%20texture.png"
            std::string uri = im.uri;
            cgltf_decode_uri(uri.data());
            uri.resize(std::strlen(uri.c_str()));

            img.path = baseDir.empty() ? uri : baseDir + "/" + uri;
        }

        out.images.push_back(std::move(img));
    }

    auto imageOf = [&](const cgltf_texture_view& tv) -> int {
        if (!tv.texture || !tv.texture->image) return -1;
        return int(tv.texture->image - data->images);
    };


    /* ---- materials ------------------------------------------------------ */
    for (cgltf_size i = 0; i < data->materials_count; ++i) {
        const cgltf_material& m = data->materials[i];
        GLTFMaterial mat;
        mat.name = m.name ? m.name : "";

        if (m.has_pbr_metallic_roughness) {
            const auto& pbr = m.pbr_metallic_roughness;

            mat.baseColor = {
                pbr.base_color_factor[0], pbr.base_color_factor[1],
                pbr.base_color_factor[2], pbr.base_color_factor[3]
            };
            mat.metallic             = pbr.metallic_factor;
            mat.roughness            = pbr.roughness_factor;
            mat.baseColorTex         = imageOf(pbr.base_color_texture);
            mat.metallicRoughnessTex = imageOf(pbr.metallic_roughness_texture);
        }
        else if (m.has_pbr_specular_glossiness) {
            // older exporters (Sketchfab especially) still write the spec/gloss
            // extension: its diffuse is the closest thing to a base colour
            const auto& sg = m.pbr_specular_glossiness;

            mat.baseColor = {
                sg.diffuse_factor[0], sg.diffuse_factor[1],
                sg.diffuse_factor[2], sg.diffuse_factor[3]
            };
            mat.metallic     = 0.f;
            mat.roughness    = 1.f - sg.glossiness_factor;
            mat.baseColorTex = imageOf(sg.diffuse_texture);
        }

        mat.emissive     = {m.emissive_factor[0], m.emissive_factor[1], m.emissive_factor[2]};
        mat.normalTex    = imageOf(m.normal_texture);
        mat.occlusionTex = imageOf(m.occlusion_texture);
        mat.emissiveTex  = imageOf(m.emissive_texture);

        mat.alphaMode    = m.alpha_mode == cgltf_alpha_mode_mask  ? AlphaMode::Mask
                         : m.alpha_mode == cgltf_alpha_mode_blend ? AlphaMode::Blend
                                                                  : AlphaMode::Opaque;
        mat.alphaCutoff  = m.alpha_cutoff;
        mat.doubleSided  = m.double_sided;

        out.materials.push_back(mat);
    }


    /* ---- meshes: every primitive appended to one shared vertex/index pair  */
    for (cgltf_size mi = 0; mi < data->meshes_count; ++mi) {
        const cgltf_mesh& m = data->meshes[mi];
        GLTFMesh mesh;
        mesh.name = m.name ? m.name : "";

        for (cgltf_size pi = 0; pi < m.primitives_count; ++pi) {
            const cgltf_primitive& p = m.primitives[pi];
            const bool strip = p.type == cgltf_primitive_type_triangle_strip;
            const bool fan   = p.type == cgltf_primitive_type_triangle_fan;
            if (p.type != cgltf_primitive_type_triangles && !strip && !fan) continue;  // points and lines

            const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *tng = nullptr;
            for (cgltf_size a = 0; a < p.attributes_count; ++a) {
                const cgltf_attribute& at = p.attributes[a];
                if      (at.type == cgltf_attribute_type_position)                  pos = at.data;
                else if (at.type == cgltf_attribute_type_normal)                    nrm = at.data;
                else if (at.type == cgltf_attribute_type_tangent)                   tng = at.data;
                else if (at.type == cgltf_attribute_type_texcoord && at.index == 0) uv  = at.data;
            }
            if (!pos) continue;

            const cgltf_size n = pos->count;
            std::vector<float> P(n * 3), N(nrm ? n * 3 : 0), T(uv ? n * 2 : 0), G(tng ? n * 4 : 0);
            cgltf_accessor_unpack_floats(pos, P.data(), n * 3);
            if (nrm) cgltf_accessor_unpack_floats(nrm, N.data(), n * 3);
            if (uv)  cgltf_accessor_unpack_floats(uv,  T.data(), n * 2);
            if (tng) cgltf_accessor_unpack_floats(tng, G.data(), n * 4);

            std::vector<GLTFVertex> verts(n);
            for (cgltf_size v = 0; v < n; ++v) {
                verts[v].pos     = {P[v*3], P[v*3+1], P[v*3+2]};
                verts[v].normal  = nrm ? Vec3f{N[v*3], N[v*3+1], N[v*3+2]} : Vec3f{0.f, 1.f, 0.f};
                verts[v].uv      = uv  ? Vec2f{T[v*2], T[v*2+1]}          : Vec2f{0.f, 0.f};
                verts[v].tangent = tng ? Vec4f{G[v*4], G[v*4+1], G[v*4+2], G[v*4+3]}
                                       : Vec4f{1.f, 0.f, 0.f, 1.f};
            }

            // no index accessor means the vertices are already in triangle order
            std::vector<uint32_t> local;
            if (p.indices) {
                local.resize(p.indices->count);
                for (cgltf_size k = 0; k < p.indices->count; ++k)
                    local[k] = uint32_t(cgltf_accessor_read_index(p.indices, k));
            } else {
                local.resize(n);
                for (cgltf_size k = 0; k < n; ++k) local[k] = uint32_t(k);
            }

            // strips and fans become plain triangles, wound the way the glTF spec defines them
            if ((strip || fan) && local.size() >= 3) {
                std::vector<uint32_t> tris;
                tris.reserve((local.size() - 2) * 3);
                for (size_t i = 0; i + 2 < local.size(); ++i) {
                    if (strip) {
                        const bool odd = i % 2 == 1;
                        tris.insert(tris.end(), {local[i], local[i + (odd ? 2 : 1)], local[i + (odd ? 1 : 2)]});
                    } else {
                        tris.insert(tris.end(), {local[i + 1], local[i + 2], local[0]});
                    }
                }
                local = std::move(tris);
            } else if (strip || fan) {
                local.clear();
            }

            if (!nrm)       detail::computeNormals(verts, local.data(), local.size());
            if (!tng && uv) detail::computeTangents(verts, local.data(), local.size());

            const uint32_t base = uint32_t(out.vertices.size());
            out.vertices.insert(out.vertices.end(), verts.begin(), verts.end());

            GLTFPrimitive prim;
            prim.firstIndex = uint32_t(out.indices.size());
            prim.indexCount = uint32_t(local.size());
            prim.material   = p.material ? int(p.material - data->materials) : -1;

            for (uint32_t k : local) out.indices.push_back(base + k);

            mesh.primitives.push_back(prim);
        }

        out.meshes.push_back(mesh);
    }


    /* ---- nodes: the ones that place a mesh, with their world transform --- */

    // only what the default scene reaches; a file with no scenes shows every node
    std::vector<bool> inScene(data->nodes_count, data->scenes_count == 0);
    if (data->scenes_count > 0) {
        const cgltf_scene& scene = data->scene ? *data->scene : data->scenes[0];
        std::vector<const cgltf_node*> stack(scene.nodes, scene.nodes + scene.nodes_count);
        while (!stack.empty()) {
            const cgltf_node* nd = stack.back();
            stack.pop_back();
            const size_t ni = size_t(nd - data->nodes);
            if (ni >= inScene.size() || inScene[ni]) continue;
            inScene[ni] = true;
            stack.insert(stack.end(), nd->children, nd->children + nd->children_count);
        }
    }

    for (cgltf_size ni = 0; ni < data->nodes_count; ++ni) {
        const cgltf_node& nd = data->nodes[ni];
        if (!nd.mesh || !inScene[ni]) continue;

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

    for (const GLTFNode& node : out.nodes) {
        if (node.mesh < 0 || size_t(node.mesh) >= out.meshes.size()) continue;

        for (const GLTFPrimitive& prim : out.meshes[size_t(node.mesh)].primitives)
            for (uint32_t k = prim.firstIndex; k < prim.firstIndex + prim.indexCount; ++k)
                out.bounds.grow((node.world * Vec4f(out.vertices[out.indices[k]].pos, 1.f)).xyz);
    }

    return out;
}

} // namespace valo
